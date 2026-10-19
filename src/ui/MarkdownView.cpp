#include "ui/MarkdownView.h"

#include "ui/Desktop.h"
#include "ui/SyntaxHighlighting.h"

#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QPointer>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QWheelEvent>

#include <algorithm>
#include <memory>

namespace aiedit::ui {

namespace {

// The column is as wide as so many CJK characters, as in reading mode.
constexpr int kColumnChars = 42;

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

struct Style
{
    QColor text;
    QColor background;
    QColor dim;            // quotes
    QColor rule;           // table borders, the line under big headings
    QColor codeBackground; // code, and the header row of tables
    QColor link;
    QPalette palette;      // for the code's syntax colours
};

Style styleFor(const QPalette& palette)
{
    const EditorColors colors = editorColors(palette);
    return {colors.text,
            colors.background,
            colors.lineNumber,
            mix(colors.text, colors.background, 0.78f),
            mix(colors.background, colors.text, colors.dark ? 0.09f : 0.055f),
            palette.color(QPalette::Link),
            palette};
}

// On paper, whatever the scheme on screen.
Style printStyle()
{
    QPalette palette;
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, QColor(0x22, 0x22, 0x22));
    palette.setColor(QPalette::Link, QColor(0x1A, 0x5F, 0xB4));
    return {QColor(0x22, 0x22, 0x22), Qt::white,           QColor(0x66, 0x66, 0x66), QColor(0xCC, 0xCC, 0xCC),
            QColor(0xF3, 0xF3, 0xF3), QColor(0x1A, 0x5F, 0xB4), palette};
}

// The syntax a code block's language names: "cpp", "python", "C++", "sh" …
QString syntaxForLanguage(const QString& language)
{
    if (language.isEmpty())
        return {};
    static const QHash<QString, QString> extensions{
        {QStringLiteral("python"), QStringLiteral("py")},     {QStringLiteral("javascript"), QStringLiteral("js")},
        {QStringLiteral("typescript"), QStringLiteral("ts")}, {QStringLiteral("bash"), QStringLiteral("sh")},
        {QStringLiteral("shell"), QStringLiteral("sh")},      {QStringLiteral("c++"), QStringLiteral("cpp")},
        {QStringLiteral("rust"), QStringLiteral("rs")},       {QStringLiteral("golang"), QStringLiteral("go")},
        {QStringLiteral("csharp"), QStringLiteral("cs")},     {QStringLiteral("ruby"), QStringLiteral("rb")},
        {QStringLiteral("powershell"), QStringLiteral("ps1")}, {QStringLiteral("markdown"), QStringLiteral("md")}};
    const QString lower = language.toLower();
    if (const QString byName = syntaxForFileName(QStringLiteral("code.") + extensions.value(lower, lower));
        !byName.isEmpty())
        return byName;
    for (const SyntaxInfo& syntax : availableSyntaxes()) {
        if (syntax.name.compare(language, Qt::CaseInsensitive) == 0)
            return syntax.name;
    }
    return {};
}

// `rules`: a line under big headings. On screen only: written out as HTML, such a heading would
// become the line alone.
void styleDocument(QTextDocument* doc, const QFont& body, const Style& style, QList<int>* headings, bool rules)
{
    const qreal size = body.pointSizeF() > 0 ? body.pointSizeF() : 11;
    QFont code = withCjkFallback(fixedFont());
    code.setPointSizeF(size * 0.9);
    if (headings)
        headings->clear();

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    const auto setChars = [doc](int from, int to, const QTextCharFormat& format, bool merge) {
        QTextCursor range(doc);
        range.setPosition(from);
        range.setPosition(to, QTextCursor::KeepAnchor);
        if (merge)
            range.mergeCharFormat(format);
        else
            range.setCharFormat(format);
    };

    std::unique_ptr<SyntaxHighlighter> highlighter; // of the code block we are in
    SyntaxState highlightState;
    QString codeLanguage;
    bool inCode = false;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const int heading = format.headingLevel();
        const bool isCode = format.nonBreakableLines() || format.hasProperty(QTextFormat::BlockCodeFence);
        const int quote = format.intProperty(QTextFormat::BlockQuoteLevel);
        const int from = block.position();
        const int to = block.position() + block.length() - 1;

        if (heading > 0) {
            if (headings)
                *headings << block.blockNumber();
            const qreal scale = heading == 1   ? 1.9
                                : heading == 2 ? 1.5
                                : heading == 3 ? 1.25
                                : heading == 4 ? 1.1
                                               : 1.0;
            format.setTopMargin(size * (heading <= 2 ? 1.5 : 1.1));
            format.setBottomMargin(size * (heading <= 2 ? 0.7 : 0.4));
            format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
            if (heading <= 2 && rules) // a line under it, as on GitHub
                format.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth,
                                   QTextLength(QTextLength::PercentageLength, 100));
            QTextCharFormat chars;
            chars.setFontPointSize(size * scale);
            chars.setFontWeight(heading <= 3 ? QFont::Bold : QFont::DemiBold);
            chars.setForeground(style.text);
            setChars(from, to, chars, true);
        } else if (isCode) {
            // Single spaced: the background leaves out the room between lines.
            format.setBackground(style.codeBackground);
            format.setLeftMargin(size * 0.6);
            format.setRightMargin(size * 0.6);
            format.setLineHeight(100, QTextBlockFormat::SingleHeight);
            format.setTopMargin(0);
            format.setBottomMargin(0);
            QTextCharFormat chars;
            chars.setFontFamilies(code.families());
            chars.setFontPointSize(code.pointSizeF());
            chars.setFontFixedPitch(true);
            chars.setForeground(style.text);
            chars.clearBackground();
            setChars(from, to, chars, false);
            // Coloured by its language, as the editor would.
            const QString language = format.stringProperty(QTextFormat::BlockCodeLanguage);
            if (!inCode || language != codeLanguage) {
                codeLanguage = language;
                highlighter = std::make_unique<SyntaxHighlighter>(syntaxForLanguage(language), style.palette);
                highlightState = SyntaxState();
            }
            if (highlighter && highlighter->isActive()) {
                const QStringList lines = block.text().split(QChar::LineSeparator);
                int lineStart = from;
                for (const QString& line : lines) {
                    for (const QTextLayout::FormatRange& range : highlighter->highlightLine(line, highlightState))
                        setChars(lineStart + range.start, lineStart + range.start + range.length, range.format, true);
                    lineStart += static_cast<int>(line.size()) + 1;
                }
            }
        } else if (QTextCursor(block).currentTable()) {
            format.setLineHeight(130, QTextBlockFormat::ProportionalHeight); // the cell's padding is room enough
            format.setTopMargin(0);
            format.setBottomMargin(0);
        } else {
            if (block.textList()) {
                format.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
                format.setTopMargin(0);
                format.setBottomMargin(size * 0.15);
            } else {
                format.setLineHeight(160, QTextBlockFormat::ProportionalHeight);
                format.setBottomMargin(size * 0.7);
            }
            if (quote > 0) { // lists in quotes too
                QTextCharFormat chars;
                chars.setForeground(style.dim);
                setChars(from, to, chars, true);
            }
        }
        inCode = isCode;
        cursor.setPosition(from);
        cursor.setBlockFormat(format);

        // Within the text: code spans, links and images.
        struct Span
        {
            int from;
            int to;
            QTextCharFormat format;
        };
        QList<Span> spans;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat chars = fragment.charFormat();
            bool changed = false;
            if (!isCode && chars.fontFixedPitch()) {
                chars.setFontFamilies(code.families());
                chars.setFontPointSize(code.pointSizeF());
                chars.setBackground(style.codeBackground);
                changed = true;
            }
            if (chars.isAnchor() && !chars.anchorHref().isEmpty()) {
                chars.setForeground(style.link);
                chars.setFontUnderline(true);
                changed = true;
            }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            if (chars.isImageFormat()) { // no wider than the column
                QTextImageFormat image = chars.toImageFormat();
                image.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
                chars = image;
                changed = true;
            }
#endif
            if (changed)
                spans.append({fragment.position(), fragment.position() + fragment.length(), chars});
        }
        for (const Span& span : spans)
            setChars(span.from, span.to, span.format, false);
    }

    // Tables: thin lines, room in the cells, the header row shaded.
    for (QTextFrame* frame : doc->rootFrame()->childFrames()) {
        auto* table = qobject_cast<QTextTable*>(frame);
        if (!table)
            continue;
        QTextTableFormat format = table->format();
        format.setBorder(1);
        format.setBorderBrush(style.rule);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderCollapse(true);
        format.setCellSpacing(0);
        format.setCellPadding(size * 0.45);
        format.setTopMargin(size * 0.3);
        format.setBottomMargin(size * 0.9);
        table->setFormat(format);
        for (int column = 0; column < table->columns(); ++column) {
            QTextTableCell cell = table->cellAt(0, column);
            QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
            cellFormat.setBackground(style.codeBackground);
            cell.setFormat(cellFormat);
        }
    }
    cursor.endEditBlock();
}

} // namespace

MarkdownView::MarkdownView(QWidget* parent) : QTextBrowser(parent)
{
    setOpenLinks(false);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QTextBrowser::anchorClicked, this, &MarkdownView::followLink);
    // The wheel, the keys and the scroll bar all act through the bar; the value changes after
    // this signal, so the news goes out once it has.
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
            [this] { QMetaObject::invokeMethod(this, &MarkdownView::scrolledByUser, Qt::QueuedConnection); });
}

void MarkdownView::setMarkdown(const QString& text, const QString& baseDirectory)
{
    if (text == m_text && baseDirectory == m_baseDirectory && !document()->isEmpty())
        return;
    m_text = text;
    m_baseDirectory = baseDirectory;
    render();
}

void MarkdownView::render()
{
    const QScopedValueRollback guard(m_restyling, true);
    const int scroll = verticalScrollBar()->value();
    // Read and styled in a document of its own, not laid out yet: in the one on screen, every piece
    // read in would be laid out as it came.
    auto* doc = new QTextDocument(this);
    doc->setDefaultFont(withCjkFallback(font()));
    doc->setDocumentMargin(0);
    doc->setBaseUrl(m_baseDirectory.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_baseDirectory + QLatin1Char('/')));
    doc->setUndoRedoEnabled(false);
    doc->setMarkdown(m_text, QTextDocument::MarkdownDialectGitHub);
    styleDocument(doc, font(), styleFor(palette()), &m_headings, true);
    placeColumn(doc);
    const QPointer<QTextDocument> old = document();
    setDocument(doc);
    if (old && old != doc)
        old->deleteLater();
    m_restyling = false;
    layOutNow();
    verticalScrollBar()->setValue(scroll);
}

void MarkdownView::layOutNow()
{
    QAbstractTextDocumentLayout* layout = document()->documentLayout();
    // documentSize() finishes the layout; the scroll bar hears of the size only later, unless told.
    emit layout->documentSizeChanged(layout->documentSize());
}

void MarkdownView::placeColumn(QTextDocument* doc)
{
    // A column in the middle, as wide as reading mode's, with room around it.
    const QFontMetricsF metrics(font());
    const qreal column = kColumnChars * metrics.horizontalAdvance(QChar(0x4E2D)); // 中
    const qreal side = std::max(metrics.height() * 1.5, (viewport()->width() - column) / 2);
    QTextFrameFormat format = doc->rootFrame()->frameFormat();
    if (qAbs(format.leftMargin() - side) < 1 && format.topMargin() > 0)
        return;
    format.setLeftMargin(side);
    format.setRightMargin(side);
    format.setTopMargin(metrics.height() * 1.2);
    format.setBottomMargin(metrics.height() * 3);
    doc->rootFrame()->setFrameFormat(format);
}

qreal MarkdownView::headingTop(int index) const
{
    if (index < 0)
        return 0;
    QAbstractTextDocumentLayout* layout = document()->documentLayout();
    if (index >= m_headings.size())
        return layout->documentSize().height();
    return layout->blockBoundingRect(document()->findBlockByNumber(m_headings[index])).top();
}

qreal MarkdownView::placeY(const Place& place) const
{
    const qreal top = headingTop(place.heading);
    return top + std::clamp(place.fraction, 0.0, 1.0) * (headingTop(place.heading + 1) - top);
}

MarkdownView::Place MarkdownView::topPlace() const
{
    const qreal y = verticalScrollBar()->value();
    // The last heading at or above the top.
    int low = 0;
    int high = static_cast<int>(m_headings.size());
    while (low < high) {
        const int middle = (low + high) / 2;
        if (headingTop(middle) <= y + 0.5)
            low = middle + 1;
        else
            high = middle;
    }
    const int heading = low - 1;
    const qreal top = headingTop(heading);
    const qreal next = headingTop(heading + 1);
    return {heading, next > top ? std::clamp((y - top) / (next - top), 0.0, 1.0) : 0.0};
}

void MarkdownView::scrollToPlace(const Place& place)
{
    verticalScrollBar()->setValue(qRound(placeY(place)));
}

void MarkdownView::scrollToFraction(double fraction)
{
    verticalScrollBar()->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * verticalScrollBar()->maximum()));
}

bool MarkdownView::selectText(const QString& text, const Place& place)
{
    const qreal y = placeY(place);
    QTextDocument* doc = document();
    const int at = doc->documentLayout()->hitTest(QPointF(doc->rootFrame()->frameFormat().leftMargin() + 1, y),
                                                  Qt::FuzzyHit);
    QTextCursor near(doc);
    near.setPosition(std::max(at, 0));
    // The nearer of the one after the place and the one before it.
    QTextCursor found;
    if (!text.isEmpty()) {
        const QTextCursor after = doc->find(text, near, QTextDocument::FindCaseSensitively);
        const QTextCursor before =
            doc->find(text, near, QTextDocument::FindCaseSensitively | QTextDocument::FindBackward);
        const auto distance = [&](const QTextCursor& cursor) {
            return qAbs(doc->documentLayout()->blockBoundingRect(cursor.block()).top() - y);
        };
        found = after.isNull() ? before : before.isNull() ? after : distance(before) < distance(after) ? before : after;
    }
    if (found.isNull()) {
        verticalScrollBar()->setValue(qRound(y - viewport()->height() / 3.0));
        return false;
    }
    setTextCursor(found);
    // A third of the way down, as the editor shows what it finds, unless already in view.
    const QRect shown = cursorRect(found);
    if (shown.top() < 0 || shown.bottom() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + shown.top() - viewport()->height() / 3);
    return true;
}

namespace {

// The document as written out: on paper, in `font`.
void buildForExport(QTextDocument* doc, const QString& markdown, const QString& baseDirectory, const QFont& font,
                    const QString& title, bool rules)
{
    doc->setDefaultFont(withCjkFallback(font));
    doc->setBaseUrl(baseDirectory.isEmpty() ? QUrl() : QUrl::fromLocalFile(baseDirectory + QLatin1Char('/')));
    doc->setUndoRedoEnabled(false);
    doc->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    doc->setMetaInformation(QTextDocument::DocumentTitle, title);
    styleDocument(doc, font, printStyle(), nullptr, rules);
}

} // namespace

bool MarkdownView::exportHtml(const QString& markdown, const QString& baseDirectory, const QFont& font,
                              const QString& path, QString* error)
{
    QTextDocument doc;
    // Without the lines under big headings: written as HTML, such a heading would be the line alone.
    buildForExport(&doc, markdown, baseDirectory, font, QFileInfo(path).completeBaseName(), false);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(doc.toHtml().toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool MarkdownView::exportPdf(const QString& markdown, const QString& baseDirectory, const QFont& font,
                             const QString& path, QString* error)
{
    QTextDocument doc;
    buildForExport(&doc, markdown, baseDirectory, font, QFileInfo(path).completeBaseName(), true);
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageMargins(QMarginsF(18, 18, 18, 18), QPageLayout::Millimeter);
        writer.setTitle(QFileInfo(path).completeBaseName());
        writer.setCreator(QStringLiteral("AIEdit"));
        doc.print(&writer);
    } // written as the writer goes
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
        if (error)
            *error = tr("The PDF could not be written");
        return false;
    }
    return true;
}

void MarkdownView::resizeEvent(QResizeEvent* event)
{
    // Laid out for the new width, it shows from where it did. (While drawn anew, the scroll bar
    // comes and goes: render() puts it back itself.)
    if (m_restyling || m_text.isEmpty()) {
        QTextBrowser::resizeEvent(event);
        placeColumn(document());
        return;
    }
    const Place place = topPlace();
    QTextBrowser::resizeEvent(event);
    placeColumn(document());
    layOutNow();
    scrollToPlace(place);
}

void MarkdownView::changeEvent(QEvent* event)
{
    QTextBrowser::changeEvent(event);
    // Drawn again in the new colours or size.
    if ((event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange) && !m_restyling &&
        !m_text.isEmpty())
        render();
}

void MarkdownView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoomed with the other views, not on its own.
        m_zoomRemainder += event->angleDelta().y();
        const int steps = m_zoomRemainder / QWheelEvent::DefaultDeltasPerStep;
        m_zoomRemainder -= steps * QWheelEvent::DefaultDeltasPerStep;
        if (steps != 0)
            emit zoomRequested(steps);
        event->accept();
        return;
    }
    QTextBrowser::wheelEvent(event);
}

void MarkdownView::setContextActions(const QList<QAction*>& actions)
{
    m_contextActions.clear();
    for (QAction* action : actions)
        m_contextActions << action;
}

void MarkdownView::contextMenuEvent(QContextMenuEvent* event)
{
    const std::unique_ptr<QMenu> menu(createStandardContextMenu(event->pos()));
    bool separated = false;
    for (const QPointer<QAction>& action : std::as_const(m_contextActions)) {
        if (!action)
            continue;
        if (!separated)
            menu->addSeparator();
        separated = true;
        menu->addAction(action);
    }
    menu->exec(event->globalPos());
}

QVariant MarkdownView::loadResource(int type, const QUrl& name)
{
    // Nothing from the network: a picture fetched would tell its server who reads the file, when.
    if (!name.isLocalFile() && !name.scheme().isEmpty() && name.scheme() != u"qrc")
        return {};
    return QTextBrowser::loadResource(type, name);
}

void MarkdownView::followLink(const QUrl& url)
{
    if (url.scheme() == u"http" || url.scheme() == u"https" || url.scheme() == u"mailto") {
        QDesktopServices::openUrl(url); // clicked: the user's choice
        return;
    }
    if (url.toString().startsWith(u'#'))
        return; // headings have no anchors here
    const QUrl resolved = document()->baseUrl().resolved(url);
    if (resolved.isLocalFile())
        emit fileLinkActivated(QDir::cleanPath(resolved.toLocalFile()));
}

} // namespace aiedit::ui
