#include "ui/Replace.h"

#include "core/FileSource.h"
#include "ui/TextDecoder.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringEncoder>

#include <algorithm>
#include <chrono>
#include <thread>

namespace aiedit::ui {

namespace {

// As searching does: regular expressions run over this much decoded text at a time.
constexpr std::uint64_t kBlockBytes = 1024 * 1024;
constexpr std::size_t kMaxRowsPerBlock = 4096;
constexpr auto kIndexWait = std::chrono::milliseconds(10);
constexpr int kMaxIndexWaits = 500;

// The same expression the search uses: text is escaped, case is ignored unless asked.
std::optional<QRegularExpression> buildRegex(const ReplaceRequest& request, QString* error)
{
    if (request.query.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("SearchJob", "Enter something to search for.");
        return std::nullopt;
    }
    QRegularExpression regex(request.mode == SearchMode::Regex ? request.query
                                                               : QRegularExpression::escape(request.query));
    QRegularExpression::PatternOptions options =
        QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
    if (!request.caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    regex.setPatternOptions(options);
    if (!regex.isValid()) {
        if (error)
            *error = QCoreApplication::translate("SearchJob", "Invalid regular expression: %1").arg(regex.errorString());
        return std::nullopt;
    }
    return regex;
}

// A regex replacement with its groups, line breaks and tabs filled in. Line breaks typed as \n
// become the file's terminator; those inside a group stay as the file has them.
QString expand(const QString& replacement, const QRegularExpressionMatch& match, const QString& lineTerminator)
{
    QString text;
    for (qsizetype i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement.at(i);
        const QChar next = i + 1 < replacement.size() ? replacement.at(i + 1) : QChar();
        if ((c == u'\\' || c == u'$') && next.isDigit()) {
            text += match.captured(next.digitValue());
        } else if (c == u'\\' && next == u'n') {
            text += lineTerminator;
        } else if (c == u'\\' && next == u't') {
            text += u'\t';
        } else if ((c == u'\\' && next == u'\\') || (c == u'$' && next == u'$')) {
            text += c;
        } else {
            text += c;
            continue;
        }
        ++i;
    }
    return text;
}

// A document small enough to edit is indexed at once, or within moments when just opened.
bool waitForIndex(const core::LineIndex& index)
{
    for (int i = 0; i < kMaxIndexWaits && !index.isComplete(); ++i)
        std::this_thread::sleep_for(kIndexWait);
    return index.isComplete();
}

QString cannotWrite(const SearchContext& context)
{
    return QCoreApplication::translate("SearchJob", "The text cannot be written in %1.")
        .arg(QString::fromLatin1(context.codec.name.data()));
}

} // namespace

std::optional<QString> replacementFor(const ReplaceRequest& request, const SearchContext& context,
                                      std::uint64_t start, std::uint64_t end, QString* error)
{
    if (request.mode != SearchMode::Regex)
        return request.replacement;
    const std::optional<QRegularExpression> regex = buildRegex(request, error);
    if (!regex)
        return std::nullopt;

    // Match again at the same place, in the rows around it, to get the groups.
    const core::LineIndex& index = *context.index;
    const std::uint64_t firstRow = index.rowIndexAt(start);
    const std::uint64_t lastRow = index.rowIndexAt(std::max(start, end > 0 ? end - 1 : 0));
    const std::vector<core::Row> rows = index.rows(firstRow, static_cast<std::size_t>(lastRow - firstRow + 1));
    const std::uint64_t blockStart = rows.front().start;
    const TextDecoder decoder(context.codec);
    const DecodedText text = decoder.decode(
        context.source->bytes().subspan(blockStart, static_cast<std::size_t>(rows.back().end - blockStart)), false);
    const QRegularExpressionMatch match =
        regex->match(text.text, text.positionAt(static_cast<std::uint32_t>(start - blockStart)),
                     QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch() || blockStart + text.byteAt(match.capturedEnd()) != end) {
        if (error)
            *error = QCoreApplication::translate("Replace", "The match changed; search again");
        return std::nullopt;
    }
    return expand(request.replacement, match, request.lineTerminator);
}

ReplaceAllResult replaceAll(const ReplaceRequest& request, const SearchContext& context)
{
    ReplaceAllResult result;
    const std::optional<QRegularExpression> regex = buildRegex(request, &result.error);
    if (!regex)
        return result;
    const core::LineIndex& index = *context.index;
    if (!waitForIndex(index)) {
        result.error = QCoreApplication::translate("Replace", "The file is still being read; try again in a moment");
        return result;
    }

    const auto data = context.source->bytes();
    const TextDecoder decoder(context.codec);
    QStringEncoder encoder(context.codec.name.data());
    const bool regexMode = request.mode == SearchMode::Regex;
    std::uint64_t copied = 0; // the bytes up to here are in the result already
    const std::uint64_t rowCount = index.rowCount();
    for (std::uint64_t row = 0; row < rowCount;) {
        const std::vector<core::Row> rows = index.rows(row, kMaxRowsPerBlock);
        std::size_t count = 0;
        std::uint64_t size = 0;
        while (count < rows.size() && (count == 0 || size < kBlockBytes)) {
            size += rows[count].end - rows[count].start;
            ++count;
        }
        const std::uint64_t blockStart = rows.front().start;
        const core::Row& last = rows[count - 1];
        const DecodedText text =
            decoder.decode(data.subspan(blockStart, static_cast<std::size_t>(last.end - blockStart)), false);
        // An empty match at the end of the block, after its last line break, is the start of the
        // next block rather than a line end of its own (think of $).
        const bool endIsLineEnd = last.end == data.size() && last.contentEnd == last.end;

        auto matches = regex->globalMatch(text.text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            if (match.capturedLength() == 0 &&
                (!regexMode || (match.capturedStart() == text.text.size() && !endIsLineEnd)))
                continue;
            const std::uint64_t start = blockStart + text.byteAt(match.capturedStart());
            const std::uint64_t end = blockStart + text.byteAt(match.capturedEnd());
            const QByteArray encoded =
                encoder.encode(regexMode ? expand(request.replacement, match, request.lineTerminator)
                                         : request.replacement);
            if (encoder.hasError()) {
                result = {};
                result.error = cannotWrite(context);
                return result;
            }
            if (result.count == 0)
                result.start = copied = start;
            result.bytes.insert(result.bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(copied),
                                data.begin() + static_cast<std::ptrdiff_t>(start));
            result.bytes.insert(result.bytes.end(), encoded.begin(), encoded.end());
            copied = end;
            ++result.count;
        }
        row += count;
    }
    result.end = copied;
    return result;
}

} // namespace aiedit::ui
