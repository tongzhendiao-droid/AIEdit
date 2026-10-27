#include "ui/Extract.h"

#include "core/Zip.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <optional>

namespace aiedit::ui {

namespace {

// Each part of a document read from its archive; beyond this it is left out, as is anything
// past kMaxText of text in all.
constexpr std::size_t kMaxPart = 256u * 1024 * 1024;
constexpr qsizetype kMaxText = 64 * 1024 * 1024;
constexpr int kMaxRows = 100'000; // of each sheet

std::optional<QByteArray> readPart(const core::ZipArchive& zip, const QString& name)
{
    const auto bytes = zip.read(name.toStdString(), kMaxPart);
    if (!bytes)
        return std::nullopt;
    return QByteArray(reinterpret_cast<const char*>(bytes->data()), static_cast<qsizetype>(bytes->size()));
}

// An attribute by its local name, whatever namespace prefix the writer chose.
QString attribute(const QXmlStreamReader& xml, QStringView name)
{
    for (const QXmlStreamAttribute& a : xml.attributes()) {
        if (a.name() == name)
            return a.value().toString();
    }
    return {};
}

// A Markdown table: the first row is its header; rows are padded to the widest.
QString markdownTable(const QList<QStringList>& rows)
{
    qsizetype columns = 0;
    for (const QStringList& row : rows)
        columns = std::max(columns, row.size());
    if (columns == 0)
        return {};
    QString out;
    auto addRow = [&](const QStringList& row) {
        out += QLatin1Char('|');
        for (qsizetype i = 0; i < columns; ++i)
            out += QLatin1Char(' ') + (i < row.size() ? markdownCell(row[i]) : QString()) + QStringLiteral(" |");
        out += QLatin1Char('\n');
    };
    addRow(rows.first());
    out += QLatin1Char('|');
    for (qsizetype i = 0; i < columns; ++i)
        out += QStringLiteral(" --- |");
    out += QLatin1Char('\n');
    for (qsizetype i = 1; i < rows.size(); ++i)
        addRow(rows[i]);
    return out;
}

// --- Word ---------------------------------------------------------------------------------------

struct WordParagraph
{
    QString text;
    int heading = 0; // 1 to 6, or 0 for body text
    bool listItem = false;
};

class WordReader
{
public:
    explicit WordReader(const QHash<QString, int>& headingStyles) : m_headingStyles(headingStyles) {}

    // The blocks of the body: paragraphs and tables, in Markdown.
    QStringList read(const QByteArray& documentXml)
    {
        m_xml.addData(documentXml);
        while (m_xml.readNextStartElement()) {
            if (m_xml.name() == u"document")
                continue; // into it
            if (m_xml.name() == u"body") {
                blocks();
                break;
            }
            m_xml.skipCurrentElement();
        }
        return m_blocks;
    }

    bool hasError() const { return m_xml.hasError(); }
    bool truncated() const { return m_size > kMaxText; }

private:
    // The children of the current element, which holds blocks: paragraphs, tables, and content
    // controls around them.
    void blocks()
    {
        while (m_xml.readNextStartElement()) {
            if (m_size > kMaxText)
                return;
            const QStringView name = m_xml.name();
            if (name == u"p")
                add(paragraphMarkdown(paragraph()));
            else if (name == u"tbl")
                add(markdownTable(table()));
            else if (name == u"sdt" || name == u"sdtContent" || name == u"customXml")
                blocks();
            else
                m_xml.skipCurrentElement();
        }
    }

    void add(QString block)
    {
        while (block.endsWith(u'\n'))
            block.chop(1);
        if (block.trimmed().isEmpty())
            return;
        m_size += block.size();
        m_blocks.append(block);
    }

    static QString paragraphMarkdown(const WordParagraph& p)
    {
        if (p.heading > 0) {
            QString text = p.text.simplified();
            return text.isEmpty() ? QString() : QString(p.heading, QLatin1Char('#')) + QLatin1Char(' ') + text;
        }
        QString text = p.text;
        text.replace(QLatin1Char('\n'), QStringLiteral("  \n")); // a line break within it
        return p.listItem ? QStringLiteral("- ") + text : text;
    }

    // From the start of a w:p to its end.
    WordParagraph paragraph()
    {
        WordParagraph p;
        int depth = 1;
        bool inProperties = false;
        while (depth > 0 && !m_xml.atEnd()) {
            const auto token = m_xml.readNext();
            if (token == QXmlStreamReader::EndElement) {
                if (m_xml.name() == u"pPr")
                    inProperties = false;
                --depth;
                continue;
            }
            if (token != QXmlStreamReader::StartElement)
                continue;
            const QStringView name = m_xml.name();
            if (name == u"t") {
                p.text += m_xml.readElementText();
                continue;
            }
            if (name == u"delText" || name == u"instrText" || name == u"rPr" ||
                (name == u"tabs" && inProperties)) {
                m_xml.skipCurrentElement();
                continue;
            }
            ++depth;
            if (name == u"pPr") {
                inProperties = true;
            } else if (inProperties && name == u"pStyle") {
                p.heading = std::max(p.heading, m_headingStyles.value(attribute(m_xml, u"val")));
            } else if (inProperties && name == u"outlineLvl") {
                bool ok = false;
                const int level = attribute(m_xml, u"val").toInt(&ok);
                if (ok && level < 6)
                    p.heading = level + 1;
            } else if (inProperties && name == u"numPr") {
                p.listItem = true;
            } else if (!inProperties && name == u"tab") {
                p.text += QLatin1Char('\t');
            } else if (!inProperties && (name == u"br" || name == u"cr")) {
                p.text += QLatin1Char('\n');
            } else if (name == u"p") {
                p.text += QLatin1Char(' '); // a text box's paragraph, inside this one
            }
        }
        return p;
    }

    // From the start of a w:tbl to its end.
    QList<QStringList> table()
    {
        QList<QStringList> rows;
        while (m_xml.readNextStartElement()) {
            if (m_xml.name() != u"tr") {
                m_xml.skipCurrentElement();
                continue;
            }
            QStringList row;
            while (m_xml.readNextStartElement()) {
                if (m_xml.name() != u"tc") {
                    m_xml.skipCurrentElement();
                    continue;
                }
                QStringList lines;
                int span = 1;
                while (m_xml.readNextStartElement()) {
                    const QStringView name = m_xml.name();
                    if (name == u"p") {
                        const QString text = paragraph().text.trimmed();
                        if (!text.isEmpty())
                            lines.append(text);
                    } else if (name == u"tbl") {
                        for (const QStringList& inner : table())
                            lines.append(inner.join(QStringLiteral(" / ")));
                    } else if (name == u"tcPr") {
                        while (m_xml.readNextStartElement()) {
                            if (m_xml.name() == u"gridSpan")
                                span = std::clamp(attribute(m_xml, u"val").toInt(), 1, 64);
                            m_xml.skipCurrentElement();
                        }
                    } else {
                        m_xml.skipCurrentElement();
                    }
                }
                row.append(lines.join(QLatin1Char('\n')));
                for (int i = 1; i < span; ++i)
                    row.append(QString());
            }
            rows.append(row);
        }
        return rows;
    }

    QXmlStreamReader m_xml;
    const QHash<QString, int>& m_headingStyles;
    QStringList m_blocks;
    qsizetype m_size = 0;
};

// Which paragraph styles are headings, and of what level: by the style's name ("heading 2",
// "Title"), its outline level, or the style it is based on.
QHash<QString, int> wordHeadingStyles(const QByteArray& stylesXml)
{
    struct Style
    {
        QString basedOn;
        int level = 0;
    };
    QHash<QString, Style> styles;
    QXmlStreamReader xml(stylesXml);
    static const QRegularExpression headingName(QStringLiteral("^heading\\s*([1-9])$"),
                                                QRegularExpression::CaseInsensitiveOption);
    while (xml.readNextStartElement()) {
        if (xml.name() == u"styles")
            continue;
        if (xml.name() != u"style" || attribute(xml, u"type") != u"paragraph") {
            xml.skipCurrentElement();
            continue;
        }
        const QString id = attribute(xml, u"styleId");
        Style style;
        while (xml.readNextStartElement()) {
            const QStringView name = xml.name();
            if (name == u"name") {
                const QString value = attribute(xml, u"val");
                if (const auto match = headingName.match(value); match.hasMatch())
                    style.level = match.captured(1).toInt();
                else if (value.compare(u"Title", Qt::CaseInsensitive) == 0)
                    style.level = 1;
                xml.skipCurrentElement();
            } else if (name == u"basedOn") {
                style.basedOn = attribute(xml, u"val");
                xml.skipCurrentElement();
            } else if (name == u"pPr") {
                while (xml.readNextStartElement()) {
                    if (xml.name() == u"outlineLvl" && style.level == 0) {
                        bool ok = false;
                        const int level = attribute(xml, u"val").toInt(&ok);
                        if (ok && level < 9)
                            style.level = level + 1;
                    }
                    xml.skipCurrentElement();
                }
            } else {
                xml.skipCurrentElement();
            }
        }
        styles.insert(id, style);
    }

    QHash<QString, int> levels;
    for (auto it = styles.cbegin(); it != styles.cend(); ++it) {
        int level = it->level;
        QString base = it->basedOn;
        for (int hops = 0; level == 0 && !base.isEmpty() && hops < 10; ++hops) {
            const Style next = styles.value(base);
            level = next.level;
            base = next.basedOn;
        }
        if (level > 0)
            levels.insert(it.key(), std::min(level, 6));
    }
    return levels;
}

// --- Excel --------------------------------------------------------------------------------------

// "B12" → 1 (the column, from 0); -1 if there is none.
int columnOfCell(QStringView reference)
{
    int column = 0;
    qsizetype i = 0;
    for (; i < reference.size() && reference[i].isLetter(); ++i)
        column = column * 26 + (reference[i].toUpper().unicode() - 'A' + 1);
    return i == 0 ? -1 : column - 1;
}

// The text of the current element and everything in it, leaving out phonetic guides.
QString richText(QXmlStreamReader& xml)
{
    QString text;
    int depth = 1;
    while (depth > 0 && !xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == u"t")
                text += xml.readElementText();
            else if (xml.name() == u"rPh")
                xml.skipCurrentElement();
            else
                ++depth;
        } else if (token == QXmlStreamReader::EndElement) {
            --depth;
        }
    }
    return text;
}

QStringList sharedStrings(const QByteArray& xmlBytes)
{
    QStringList strings;
    QXmlStreamReader xml(xmlBytes);
    while (xml.readNextStartElement()) {
        if (xml.name() == u"sst")
            continue;
        if (xml.name() == u"si")
            strings.append(richText(xml));
        else
            xml.skipCurrentElement();
    }
    return strings;
}

// The rows of a sheet, as text, without the empty ones; `truncated` if rows were left out.
QList<QStringList> sheetRows(const QByteArray& xmlBytes, const QStringList& strings, bool* truncated)
{
    QList<QStringList> rows;
    QXmlStreamReader xml(xmlBytes);
    while (xml.readNextStartElement()) {
        const QStringView name = xml.name();
        if (name == u"worksheet" || name == u"sheetData")
            continue;
        if (name != u"row") {
            xml.skipCurrentElement();
            continue;
        }
        if (rows.size() >= kMaxRows) {
            *truncated = true;
            break;
        }
        QStringList row;
        while (xml.readNextStartElement()) {
            if (xml.name() != u"c") {
                xml.skipCurrentElement();
                continue;
            }
            const QString type = attribute(xml, u"t");
            int column = columnOfCell(attribute(xml, u"r"));
            if (column < 0)
                column = static_cast<int>(row.size());
            QString value;
            while (xml.readNextStartElement()) {
                if (xml.name() == u"v")
                    value = xml.readElementText();
                else if (xml.name() == u"is")
                    value = richText(xml);
                else
                    xml.skipCurrentElement();
            }
            if (type == u"s") {
                bool ok = false;
                const int index = value.toInt(&ok);
                value = ok && index >= 0 && index < strings.size() ? strings[index] : QString();
            } else if (type == u"b") {
                value = value == u"1" ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            }
            if (value.isEmpty() || column > 16384)
                continue;
            while (row.size() <= column)
                row.append(QString());
            row[column] = value;
        }
        if (std::ranges::any_of(row, [](const QString& cell) { return !cell.isEmpty(); }))
            rows.append(row);
    }
    return rows;
}

// --- EPUB ---------------------------------------------------------------------------------------

// XHTML as Markdown: headings, paragraphs, list items and line breaks, without the styling.
class HtmlToMarkdown
{
public:
    QString convert(QByteArray html)
    {
        // Named entities other than XML's own are not declared in most books' XHTML; the XML
        // reader stops at the first. The common ones become characters.
        static const QList<std::pair<QByteArray, QByteArray>> entities = {
            {"&nbsp;", "&#160;"},   {"&mdash;", "&#8212;"}, {"&ndash;", "&#8211;"}, {"&hellip;", "&#8230;"},
            {"&ldquo;", "&#8220;"}, {"&rdquo;", "&#8221;"}, {"&lsquo;", "&#8216;"}, {"&rsquo;", "&#8217;"},
            {"&copy;", "&#169;"},   {"&middot;", "&#183;"}, {"&times;", "&#215;"},  {"&emsp;", "&#8195;"},
            {"&ensp;", "&#8194;"},  {"&laquo;", "&#171;"},  {"&raquo;", "&#187;"},  {"&bull;", "&#8226;"},
        };
        for (const auto& [name, number] : entities)
            html.replace(name, number);

        QXmlStreamReader xml(html);
        int skipDepth = 0; // inside <head>, <script> or <style>
        int preDepth = 0;
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::StartElement) {
                const QString name = xml.name().toString().toLower();
                if (skipDepth > 0 || name == u"head" || name == u"script" || name == u"style") {
                    ++skipDepth;
                    continue;
                }
                if (name == u"br") {
                    m_line += QStringLiteral("  \n");
                } else if (name.size() == 2 && name[0] == u'h' && name[1] >= u'1' && name[1] <= u'6') {
                    flush();
                    m_heading = name[1].unicode() - '0';
                } else if (name == u"li") {
                    flush();
                    m_listItem = true;
                } else if (name == u"pre") {
                    flush();
                    ++preDepth;
                } else if (name == u"td" || name == u"th") {
                    if (!m_line.trimmed().isEmpty())
                        m_line += QStringLiteral(" | ");
                } else if (isBlock(name)) {
                    flush();
                }
            } else if (token == QXmlStreamReader::EndElement) {
                const QString name = xml.name().toString().toLower();
                if (skipDepth > 0) {
                    --skipDepth;
                    continue;
                }
                if (name == u"pre")
                    preDepth = std::max(0, preDepth - 1);
                if (isBlock(name) || name == u"li" || name == u"pre" ||
                    (name.size() == 2 && name[0] == u'h' && name[1] >= u'1' && name[1] <= u'6'))
                    flush();
            } else if (token == QXmlStreamReader::Characters && skipDepth == 0) {
                QString text = xml.text().toString();
                if (preDepth == 0) {
                    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
                    if (m_line.isEmpty() || m_line.endsWith(u'\n')) {
                        while (text.startsWith(u' '))
                            text.remove(0, 1);
                    }
                }
                m_line += text;
            }
        }
        flush();
        if (xml.hasError() && m_blocks.isEmpty()) {
            // Not XML after all: the text between the tags.
            QString text = QString::fromUtf8(html);
            text.remove(QRegularExpression(QStringLiteral("<(script|style|head)[^>]*>.*?</\\1>"),
                                           QRegularExpression::CaseInsensitiveOption |
                                               QRegularExpression::DotMatchesEverythingOption));
            text.replace(QRegularExpression(QStringLiteral("<(p|div|br|h[1-6]|li|tr)\\b[^>]*>"),
                                            QRegularExpression::CaseInsensitiveOption),
                         QStringLiteral("\n\n"));
            text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
            for (const QString& block : text.split(QRegularExpression(QStringLiteral("\\n\\s*\\n"))))
                if (!block.trimmed().isEmpty())
                    m_blocks.append(block.simplified());
        }
        return m_blocks.join(QStringLiteral("\n\n"));
    }

private:
    static bool isBlock(const QString& name)
    {
        static const QStringList blocks = {QStringLiteral("p"),          QStringLiteral("div"),
                                           QStringLiteral("blockquote"), QStringLiteral("section"),
                                           QStringLiteral("article"),    QStringLiteral("tr"),
                                           QStringLiteral("table"),      QStringLiteral("ul"),
                                           QStringLiteral("ol"),         QStringLiteral("dd"),
                                           QStringLiteral("dt"),         QStringLiteral("figcaption"),
                                           QStringLiteral("body")};
        return blocks.contains(name);
    }

    void flush()
    {
        QString text = m_line.trimmed();
        m_line.clear();
        if (!text.isEmpty()) {
            if (m_heading > 0)
                text = QString(m_heading, QLatin1Char('#')) + QLatin1Char(' ') + text.simplified();
            else if (m_listItem)
                text = QStringLiteral("- ") + text;
            m_blocks.append(text);
        }
        m_heading = 0;
        m_listItem = false;
    }

    QStringList m_blocks;
    QString m_line;
    int m_heading = 0;
    bool m_listItem = false;
};

QString resolvePath(const QString& base, const QString& href)
{
    const QString path = QUrl::fromPercentEncoding(href.section(QLatin1Char('#'), 0, 0).toUtf8());
    return QDir::cleanPath(base.isEmpty() ? path : base + QLatin1Char('/') + path);
}

QString truncatedNote()
{
    return QCoreApplication::translate("Extract", "Only the start is shown: the document's text is longer than %L1 MiB.")
        .arg(kMaxText / (1024 * 1024));
}

} // namespace

QString markdownCell(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("<br>"));
    text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    text.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return text.trimmed();
}

ExtractedText extractWord(std::span<const std::uint8_t> bytes)
{
    ExtractedText result;
    result.markdown = true;
    const auto zip = core::ZipArchive::open(bytes);
    const auto document = zip ? readPart(*zip, QStringLiteral("word/document.xml")) : std::nullopt;
    if (!document) {
        result.note = QCoreApplication::translate("Extract", "This Word document cannot be read: it is broken or encrypted.");
        return result;
    }
    const QHash<QString, int> headings =
        wordHeadingStyles(readPart(*zip, QStringLiteral("word/styles.xml")).value_or(QByteArray()));
    WordReader reader(headings);
    result.text = reader.read(*document).join(QStringLiteral("\n\n"));
    if (reader.truncated())
        result.note = truncatedNote();
    else if (reader.hasError())
        result.note = QCoreApplication::translate("Extract", "Part of this Word document is broken; the text before it is shown.");
    if (!result.text.isEmpty())
        result.text += QLatin1Char('\n');
    return result;
}

ExtractedText extractExcel(std::span<const std::uint8_t> bytes)
{
    ExtractedText result;
    result.markdown = true;
    const auto zip = core::ZipArchive::open(bytes);
    const auto workbook = zip ? readPart(*zip, QStringLiteral("xl/workbook.xml")) : std::nullopt;
    if (!workbook) {
        result.note = QCoreApplication::translate("Extract", "This Excel workbook cannot be read: it is broken or encrypted.");
        return result;
    }

    // Where each relationship of the workbook points: its sheets.
    QHash<QString, QString> targets;
    QXmlStreamReader rels(readPart(*zip, QStringLiteral("xl/_rels/workbook.xml.rels")).value_or(QByteArray()));
    while (!rels.atEnd()) {
        if (rels.readNext() == QXmlStreamReader::StartElement && rels.name() == u"Relationship") {
            QString target = attribute(rels, u"Target");
            target = target.startsWith(u'/') ? target.mid(1) : resolvePath(QStringLiteral("xl"), target);
            targets.insert(attribute(rels, u"Id"), target);
        }
    }
    const QStringList strings =
        sharedStrings(readPart(*zip, QStringLiteral("xl/sharedStrings.xml")).value_or(QByteArray()));

    QStringList sections;
    bool truncated = false;
    qsizetype size = 0;
    QXmlStreamReader xml(*workbook);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() != u"sheet")
            continue;
        const QString name = attribute(xml, u"name");
        const QString part = targets.value(attribute(xml, u"id"));
        const auto sheet = part.isEmpty() ? std::nullopt : readPart(*zip, part);
        if (!sheet)
            continue; // a chart sheet, or one that is missing
        const QList<QStringList> rows = sheetRows(*sheet, strings, &truncated);
        QString section = QStringLiteral("## ") + name.simplified();
        section += rows.isEmpty() ? QStringLiteral("\n\n") + QCoreApplication::translate("Extract", "(empty)")
                                  : QStringLiteral("\n\n") + markdownTable(rows);
        size += section.size();
        sections.append(section.trimmed());
        if (size > kMaxText) {
            truncated = true;
            break;
        }
    }
    result.text = sections.join(QStringLiteral("\n\n"));
    if (!result.text.isEmpty())
        result.text += QLatin1Char('\n');
    if (truncated)
        result.note = QCoreApplication::translate("Extract", "Only the first %L1 rows of each sheet are shown.").arg(kMaxRows);
    return result;
}

ExtractedText extractEpub(std::span<const std::uint8_t> bytes)
{
    ExtractedText result;
    result.markdown = true;
    const auto zip = core::ZipArchive::open(bytes);
    const auto container = zip ? readPart(*zip, QStringLiteral("META-INF/container.xml")) : std::nullopt;
    if (!container) {
        result.note = QCoreApplication::translate("Extract", "This EPUB book cannot be read: it is broken.");
        return result;
    }
    QString packagePath;
    QXmlStreamReader xml(*container);
    while (!xml.atEnd() && packagePath.isEmpty()) {
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == u"rootfile")
            packagePath = attribute(xml, u"full-path");
    }
    const auto package = packagePath.isEmpty() ? std::nullopt : readPart(*zip, packagePath);
    if (!package) {
        result.note = QCoreApplication::translate("Extract", "This EPUB book cannot be read: it is broken.");
        return result;
    }

    const QString base = packagePath.contains(u'/') ? packagePath.section(u'/', 0, -2) : QString();
    QHash<QString, QString> manifest; // id → path in the archive
    QStringList spine;
    QString title;
    QXmlStreamReader opf(*package);
    while (!opf.atEnd()) {
        if (opf.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (opf.name() == u"item")
            manifest.insert(attribute(opf, u"id"), resolvePath(base, attribute(opf, u"href")));
        else if (opf.name() == u"itemref")
            spine.append(attribute(opf, u"idref"));
        else if (opf.name() == u"title" && title.isEmpty())
            title = opf.readElementText().simplified();
    }

    QStringList chapters;
    if (!title.isEmpty())
        chapters.append(QStringLiteral("# ") + title);
    qsizetype size = 0;
    bool missing = false;
    for (const QString& id : std::as_const(spine)) {
        const auto page = readPart(*zip, manifest.value(id));
        if (!page) {
            missing = true;
            continue;
        }
        QString text = HtmlToMarkdown().convert(*page);
        if (text.isEmpty())
            continue;
        size += text.size();
        chapters.append(text);
        if (size > kMaxText) {
            result.note = truncatedNote();
            break;
        }
    }
    result.text = chapters.join(QStringLiteral("\n\n"));
    if (!result.text.isEmpty())
        result.text += QLatin1Char('\n');
    if (missing && result.note.isEmpty())
        result.note = QCoreApplication::translate("Extract", "Some chapters of this book are missing or broken and are left out.");
    return result;
}

ExtractedFormat extractableFormat(std::span<const std::uint8_t> bytes)
{
    auto startsWith = [&](std::string_view magic) {
        return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
    };
    if (startsWith("CNWTEPRG") || startsWith("WTLE"))
        return ExtractedFormat::Epl;
    // A PDF may have a few bytes of junk before its header.
    const std::string_view head(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 1024));
    if (head.find("%PDF-") != std::string_view::npos)
        return ExtractedFormat::Pdf;
    if (!startsWith("PK"))
        return ExtractedFormat::None;
    const auto zip = core::ZipArchive::open(bytes);
    if (!zip)
        return ExtractedFormat::None;
    if (zip->find("word/document.xml"))
        return ExtractedFormat::Word;
    if (zip->find("xl/workbook.xml"))
        return ExtractedFormat::Excel;
    if (zip->find("META-INF/container.xml") && zip->find("mimetype")) {
        const auto type = zip->read("mimetype", 100);
        if (type && std::string_view(reinterpret_cast<const char*>(type->data()), type->size())
                            .starts_with("application/epub+zip"))
            return ExtractedFormat::Epub;
    }
    return ExtractedFormat::None;
}

QString formatName(ExtractedFormat format)
{
    switch (format) {
    case ExtractedFormat::None:
        break;
    case ExtractedFormat::Word:
        return QCoreApplication::translate("Extract", "Word document");
    case ExtractedFormat::Excel:
        return QCoreApplication::translate("Extract", "Excel workbook");
    case ExtractedFormat::Epub:
        return QCoreApplication::translate("Extract", "EPUB book");
    case ExtractedFormat::Pdf:
        return QCoreApplication::translate("Extract", "PDF document");
    case ExtractedFormat::Epl:
        return QCoreApplication::translate("Extract", "易语言 source");
    }
    return {};
}

ExtractedText extractText(ExtractedFormat format, std::span<const std::uint8_t> bytes)
{
    switch (format) {
    case ExtractedFormat::Word:
        return extractWord(bytes);
    case ExtractedFormat::Excel:
        return extractExcel(bytes);
    case ExtractedFormat::Epub:
        return extractEpub(bytes);
    case ExtractedFormat::Pdf:
    case ExtractedFormat::Epl:
    case ExtractedFormat::None:
        break;
    }
    return {};
}

} // namespace aiedit::ui
