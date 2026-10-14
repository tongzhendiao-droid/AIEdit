#include "ui/AiTools.h"

#include "core/FileSource.h"
#include "core/FileSummary.h"
#include "core/Offset.h"
#include "ui/SearchJob.h"
#include "ui/TextDecoder.h"

#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

constexpr std::uint64_t kDefaultReadBytes = 256;
constexpr std::uint64_t kMaxReadBytes = 4096;
constexpr std::uint64_t kDefaultReadLines = 50;
constexpr std::uint64_t kMaxReadLines = 200;
constexpr qsizetype kMaxLineChars = 1000;
constexpr qsizetype kMaxResultChars = 24000;
constexpr int kDefaultSearchResults = 20;
constexpr int kMaxSearchResults = 100;
constexpr qsizetype kSnippetChars = 160;
constexpr qsizetype kSnippetLead = 40;
constexpr qsizetype kMaxMarks = 500;
constexpr std::uint64_t kInfoDumpBytes = 64;

QJsonObject function(const QString& name, const QString& description, const QJsonObject& properties,
                     const QStringList& required = {})
{
    const QJsonObject parameters{{QStringLiteral("type"), QStringLiteral("object")},
                                 {QStringLiteral("properties"), properties},
                                 {QStringLiteral("required"), QJsonArray::fromStringList(required)}};
    return {{QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
                                                     {QStringLiteral("description"), description},
                                                     {QStringLiteral("parameters"), parameters}}}};
}

QJsonObject property(const QString& type, const QString& description)
{
    return {{QStringLiteral("type"), type}, {QStringLiteral("description"), description}};
}

QString hex(std::uint64_t value)
{
    return QString::number(value, 16).toUpper();
}

// Models write offsets as numbers or as strings like "0x1F0"; take either.
std::optional<std::uint64_t> numberArgument(const QJsonObject& arguments, const QString& key)
{
    const QJsonValue value = arguments.value(key);
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number < 0 || number > 9.0e15)
            return std::nullopt;
        return static_cast<std::uint64_t>(number);
    }
    if (value.isString())
        return core::parseOffset(value.toString().toStdString(), 0);
    return std::nullopt;
}

QString capped(QString text)
{
    if (text.size() > kMaxResultChars) {
        text.truncate(kMaxResultChars);
        text += QStringLiteral("\n[Output truncated.]");
    }
    return text;
}

QString describeType(const core::FileTypeInfo& type)
{
    switch (type.kind) {
    case core::ContentKind::Empty:
        return QStringLiteral("empty");
    case core::ContentKind::Binary:
        return type.format.empty() ? QStringLiteral("binary")
                                   : QStringLiteral("binary (%1)").arg(QString::fromUtf8(type.format.data(),
                                                                                         static_cast<qsizetype>(type.format.size())));
    case core::ContentKind::Text:
        return QStringLiteral("text");
    }
    return {};
}

QString codecName(const core::TextCodec& codec)
{
    return QString::fromLatin1(codec.name.data(), static_cast<qsizetype>(codec.name.size()));
}

// Where a match is, with the text of its line (or the bytes, for binary files).
QString describeMatch(const ToolTarget& target, const TextDecoder& decoder, std::uint64_t start)
{
    const auto data = target.source->bytes();
    const core::LineIndex& index = *target.index;
    QString where = QStringLiteral("0x") + hex(start);
    if (target.type.kind == core::ContentKind::Binary || index.rowCount() == 0 || start >= index.indexedBytes()) {
        const std::uint64_t end = std::min<std::uint64_t>(data.size(), start + 16);
        return where + QStringLiteral(": ") +
               QString::fromLatin1(core::hexDump(data.subspan(start, end - start), start)).trimmed();
    }
    const core::Row row = index.row(index.rowIndexAt(start));
    const QString before = decoder.decodePlain(data.subspan(row.start, start - row.start));
    const QString line = decoder.decodePlain(data.subspan(row.start, row.contentEnd - row.start));
    const qsizetype from = std::max<qsizetype>(0, before.size() - kSnippetLead);
    return QStringLiteral("%1 (line %2): %3%4")
        .arg(where)
        .arg(row.line + 1)
        .arg(from > 0 ? QStringLiteral("…") : QString(), line.mid(from, kSnippetChars));
}

} // namespace

QJsonArray tools::definitions()
{
    const QJsonObject none;
    return {
        function(QStringLiteral("get_file_info"),
                 QStringLiteral("Basic facts about the open file: name, size, detected type and encoding, number of "
                                "lines, entropy and its first bytes."),
                 none),
        function(QStringLiteral("read_bytes"),
                 QStringLiteral("Read raw bytes of the open file as a hex dump. Use it for binary data and headers."),
                 {{QStringLiteral("offset"),
                   property(QStringLiteral("integer"), QStringLiteral("Byte offset to start at (a number, or a "
                                                                      "hex string such as \"0x1F0\")."))},
                  {QStringLiteral("length"),
                   property(QStringLiteral("integer"), QStringLiteral("Number of bytes, at most 4096. Default 256."))}},
                 {QStringLiteral("offset")}),
        function(QStringLiteral("read_lines"),
                 QStringLiteral("Read lines of the open text file, each prefixed with its line number."),
                 {{QStringLiteral("start_line"),
                   property(QStringLiteral("integer"), QStringLiteral("First line to read; lines start at 1."))},
                  {QStringLiteral("count"),
                   property(QStringLiteral("integer"), QStringLiteral("Number of lines, at most 200. Default 50."))}},
                 {QStringLiteral("start_line")}),
        function(QStringLiteral("search"),
                 QStringLiteral("Search the whole open file and list the matches with byte offsets, line numbers and "
                                "the text around them. Prefer this over reading a large file from the start."),
                 {{QStringLiteral("query"), property(QStringLiteral("string"), QStringLiteral("What to find."))},
                  {QStringLiteral("mode"),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("enum"), QJsonArray{QStringLiteral("text"), QStringLiteral("regex"),
                                                                   QStringLiteral("hex")}},
                               {QStringLiteral("description"),
                                QStringLiteral("text (default), regex (PCRE syntax, matched line by line), or hex "
                                               "(bytes such as \"4D 5A ?? 00\", ?? matches any byte).")}}},
                  {QStringLiteral("case_sensitive"),
                   property(QStringLiteral("boolean"), QStringLiteral("For text and regex. Default false."))},
                  {QStringLiteral("max_results"),
                   property(QStringLiteral("integer"), QStringLiteral("At most 100. Default 20."))},
                  {QStringLiteral("start_offset"),
                   property(QStringLiteral("integer"), QStringLiteral("Byte offset to search from, to continue "
                                                                      "after earlier results. Default 0."))}},
                 {QStringLiteral("query")}),
        function(QStringLiteral("mark_fields"),
                 QStringLiteral("Highlight the fields of a binary structure in the hex view so that the user sees "
                                "them, with names and meanings on hover. Use it after working out a structure "
                                "from the bytes. Marking replaces earlier marks."),
                 {{QStringLiteral("fields"),
                   QJsonObject{
                       {QStringLiteral("type"), QStringLiteral("array")},
                       {QStringLiteral("items"),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                    {QStringLiteral("properties"),
                                     QJsonObject{{QStringLiteral("offset"),
                                                  property(QStringLiteral("integer"), QStringLiteral("Byte offset."))},
                                                 {QStringLiteral("length"),
                                                  property(QStringLiteral("integer"), QStringLiteral("Size in bytes."))},
                                                 {QStringLiteral("name"),
                                                  property(QStringLiteral("string"), QStringLiteral("Field name."))},
                                                 {QStringLiteral("value"),
                                                  property(QStringLiteral("string"), QStringLiteral("Decoded value."))},
                                                 {QStringLiteral("description"),
                                                  property(QStringLiteral("string"),
                                                           QStringLiteral("What the field means."))}}},
                                    {QStringLiteral("required"),
                                     QJsonArray{QStringLiteral("offset"), QStringLiteral("length"),
                                                QStringLiteral("name")}}}}}}},
                 {QStringLiteral("fields")}),
    };
}

QString tools::fileInfo(const ToolTarget& target)
{
    const auto data = target.source->bytes();
    const core::LineIndex& index = *target.index;
    QString text = QStringLiteral("Name: %1\nSize: %2 bytes\nDetected: %3\nDecoded as: %4\n")
                       .arg(target.fileName)
                       .arg(data.size())
                       .arg(describeType(target.type), codecName(target.codec));
    text += index.isComplete() ? QStringLiteral("Lines: %1\n").arg(index.lineCount())
                               : QStringLiteral("Lines: at least %1 (still being counted)\n").arg(index.lineCount());
    text += QStringLiteral("Entropy of the first 64 KiB: %1 bits per byte\n")
                .arg(core::entropy(data.first(std::min<std::size_t>(data.size(), core::kSummaryHeadBytes))), 0, 'f', 2);
    text += QStringLiteral("First bytes:\n") +
            QString::fromLatin1(core::hexDump(data.first(std::min<std::size_t>(data.size(), kInfoDumpBytes))));
    return capped(text);
}

QString tools::readBytes(const ToolTarget& target, const QJsonObject& arguments)
{
    const auto data = target.source->bytes();
    const auto offset = numberArgument(arguments, QStringLiteral("offset"));
    if (!offset)
        return QStringLiteral("Error: offset must be a number.");
    if (*offset >= data.size())
        return QStringLiteral("Error: offset 0x%1 is past the end of the file (%2 bytes).").arg(hex(*offset)).arg(data.size());
    const std::uint64_t requested = numberArgument(arguments, QStringLiteral("length")).value_or(kDefaultReadBytes);
    const std::uint64_t length =
        std::min({std::max<std::uint64_t>(1, requested), kMaxReadBytes, data.size() - *offset});
    QString text = QStringLiteral("Bytes 0x%1-0x%2 (%3 bytes) of a %4-byte file:\n")
                       .arg(hex(*offset), hex(*offset + length - 1))
                       .arg(length)
                       .arg(data.size());
    if (requested > kMaxReadBytes)
        text += QStringLiteral("(At most 4096 bytes per call.)\n");
    return text + QString::fromLatin1(core::hexDump(data.subspan(*offset, length), *offset));
}

QString tools::readLines(const ToolTarget& target, const QJsonObject& arguments)
{
    const auto start = numberArgument(arguments, QStringLiteral("start_line"));
    if (!start || *start == 0)
        return QStringLiteral("Error: start_line must be a line number, starting at 1.");
    const std::uint64_t count =
        std::clamp<std::uint64_t>(numberArgument(arguments, QStringLiteral("count")).value_or(kDefaultReadLines), 1,
                                  kMaxReadLines);
    const core::LineIndex& index = *target.index;
    const auto firstRow = index.rowIndexOfLine(*start - 1);
    if (!firstRow) {
        return index.isComplete() ? QStringLiteral("Error: the file has only %1 lines.").arg(index.lineCount())
                                  : QStringLiteral("Line %1 has not been read yet; try again shortly.").arg(*start);
    }

    const auto data = target.source->bytes();
    const TextDecoder decoder(target.codec);
    const std::uint64_t rows = index.rowCount();
    QString text;
    std::uint64_t row = *firstRow;
    for (std::uint64_t n = 0; n < count && row < rows && text.size() < kMaxResultChars; ++n) {
        // A very long line is split into several rows; join them back, up to a limit.
        const core::Row first = index.row(row);
        core::Row last = first;
        ++row;
        for (; row < rows; ++row) {
            const core::Row next = index.row(row);
            if (next.line != first.line)
                break;
            last = next;
        }
        const std::uint64_t byteLimit = 4 * static_cast<std::uint64_t>(kMaxLineChars);
        const std::uint64_t length = std::min(last.contentEnd - first.start, byteLimit);
        QString content = decoder.decodePlain(data.subspan(first.start, length));
        if (content.size() > kMaxLineChars || last.contentEnd - first.start > byteLimit) {
            content.truncate(kMaxLineChars);
            content += QStringLiteral(" …[line truncated]");
        }
        text += QStringLiteral("%1: %2\n").arg(first.line + 1).arg(content);
    }
    return capped(text);
}

QString tools::search(const ToolTarget& target, const QJsonObject& arguments, const std::atomic<bool>& cancelled)
{
    const QString query = arguments.value(QStringLiteral("query")).toString();
    if (query.isEmpty())
        return QStringLiteral("Error: query must not be empty.");
    const QString mode = arguments.value(QStringLiteral("mode")).toString(QStringLiteral("text"));
    SearchRequest request;
    request.mode = mode == QLatin1String("regex") ? SearchMode::Regex
                   : mode == QLatin1String("hex") ? SearchMode::Hex
                                                  : SearchMode::Text;
    request.query = query;
    request.caseSensitive = arguments.value(QStringLiteral("case_sensitive")).toBool(false);
    request.wrap = false;
    request.from = numberArgument(arguments, QStringLiteral("start_offset")).value_or(0);
    const int maxResults =
        std::clamp(arguments.value(QStringLiteral("max_results")).toInt(kDefaultSearchResults), 1, kMaxSearchResults);

    const SearchContext context{target.source, target.index, target.codec};
    SearchHooks hooks;
    hooks.cancelled = [&cancelled] { return cancelled.load(); };
    const TextDecoder decoder(target.codec);
    QStringList matches;
    bool more = false;
    for (;;) {
        const SearchOutcome outcome = ui::search(request, context, hooks);
        if (outcome.status == SearchOutcome::Status::InvalidQuery)
            return QStringLiteral("Error: ") + outcome.error;
        if (outcome.status != SearchOutcome::Status::Found)
            break;
        if (matches.size() == maxResults) {
            more = true;
            break;
        }
        matches << describeMatch(target, decoder, outcome.start);
        request.from = std::max(outcome.end, outcome.start + 1);
    }
    if (cancelled)
        return QStringLiteral("Cancelled.");
    if (matches.isEmpty())
        return QStringLiteral("No matches.");
    return capped(QStringLiteral("%1 match(es)%2:\n%3")
                      .arg(matches.size())
                      .arg(more ? QStringLiteral(" shown; there are more, continue with start_offset") : QString())
                      .arg(matches.join(QLatin1Char('\n'))));
}

QString tools::markFields(const ToolTarget& target, const QJsonObject& arguments)
{
    if (!target.markFields)
        return QStringLiteral("Error: fields cannot be marked here.");
    const std::uint64_t size = target.source->size();
    QList<FieldMark> marks;
    QStringList skipped;
    for (const QJsonValue& value : arguments.value(QStringLiteral("fields")).toArray()) {
        if (marks.size() == kMaxMarks)
            break;
        const QJsonObject field = value.toObject();
        const auto offset = numberArgument(field, QStringLiteral("offset"));
        const auto length = numberArgument(field, QStringLiteral("length"));
        const QString name = field.value(QStringLiteral("name")).toString();
        if (!offset || !length || *length == 0 || name.isEmpty() || *offset >= size) {
            skipped << (name.isEmpty() ? QStringLiteral("(unnamed)") : name);
            continue;
        }
        QStringList detail;
        if (const QString decoded = field.value(QStringLiteral("value")).toVariant().toString(); !decoded.isEmpty())
            detail << decoded;
        if (const QString meaning = field.value(QStringLiteral("description")).toString(); !meaning.isEmpty())
            detail << meaning;
        marks.append({*offset, std::min(*length, size - *offset), name, detail.join(QStringLiteral(" — "))});
    }
    if (marks.isEmpty())
        return QStringLiteral("Error: no valid fields; each needs offset, length (at least 1) and name within the file.");
    target.markFields(marks);
    return QStringLiteral("Marked %1 field(s) in the hex view.%2")
        .arg(marks.size())
        .arg(skipped.isEmpty() ? QString() : QStringLiteral(" Skipped invalid: ") + skipped.join(QStringLiteral(", ")));
}

ToolRunner::ToolRunner(QObject* parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, [this] {
        auto done = std::exchange(m_done, nullptr);
        if (done && !m_watcher.isCanceled() && m_watcher.future().resultCount() > 0)
            done(m_watcher.result());
    });
}

ToolRunner::~ToolRunner()
{
    cancel();
}

void ToolRunner::run(const ToolCall& call, const ToolTarget& target, std::function<void(const QString&)> done)
{
    cancel();
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(call.arguments.trimmed().isEmpty() ? QByteArray("{}") : call.arguments.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        done(QStringLiteral("Error: the arguments are not valid JSON (%1).").arg(error.errorString()));
        return;
    }
    const QJsonObject arguments = document.object();

    if (call.name == QLatin1String("search")) {
        // Searching a big file takes a while; the target owns everything the search reads.
        m_cancelled = std::make_shared<std::atomic<bool>>(false);
        m_done = std::move(done);
        m_watcher.setFuture(QtConcurrent::run(
            [target, arguments, cancelled = m_cancelled] { return tools::search(target, arguments, *cancelled); }));
        return;
    }

    QString result;
    if (call.name == QLatin1String("get_file_info"))
        result = tools::fileInfo(target);
    else if (call.name == QLatin1String("read_bytes"))
        result = tools::readBytes(target, arguments);
    else if (call.name == QLatin1String("read_lines"))
        result = tools::readLines(target, arguments);
    else if (call.name == QLatin1String("mark_fields"))
        result = tools::markFields(target, arguments);
    else
        result = QStringLiteral("Error: there is no tool called \"%1\".").arg(call.name);
    done(result);
}

void ToolRunner::cancel()
{
    if (m_cancelled)
        *m_cancelled = true;
    m_done = nullptr;
}

} // namespace aiedit::ui
