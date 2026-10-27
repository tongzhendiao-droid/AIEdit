#include "ui/CompareTab.h"

#include "core/FileSource.h"
#include "ui/CompareView.h"
#include "ui/HexCompareView.h"

#include <QAction>
#include <QBoxLayout>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <climits>

namespace aiedit::ui {

namespace {

constexpr qsizetype kMaxAiChars = 24'000;
constexpr int kDetailsBefore = 6; // instructions shown before and after a difference
constexpr int kDetailsAfter = 6;
constexpr std::uint64_t kDetailsBytes = 16;

QString key(const char* name)
{
    return QStringLiteral("compare/") + QLatin1String(name);
}

// The line ending most lines of a side use, by the first part of it; empty if it has none.
QString lineEnding(const CompareSide& side)
{
    const auto bytes = side.source->bytes();
    const core::LineEndings endings =
        core::countLineEndings(bytes.first(std::min<std::size_t>(bytes.size(), core::kSniffSize)), side.codec->scheme);
    if (endings.crlf == 0 && endings.lf == 0 && endings.cr == 0)
        return {};
    if (endings.crlf >= endings.lf && endings.crlf >= endings.cr)
        return QStringLiteral("CRLF");
    return endings.cr > endings.lf ? QStringLiteral("CR") : QStringLiteral("LF");
}

QString codecName(const CompareSide& side)
{
    return QString::fromLatin1(side.codec->name.data(), static_cast<qsizetype>(side.codec->name.size()));
}

QString hex(std::uint64_t value)
{
    return QStringLiteral("0x") + QString::number(value, 16).toUpper();
}

bool isBinaryFile(const CompareSide& side)
{
    return side.type.kind == core::ContentKind::Binary;
}

} // namespace

CompareTab::CompareTab(CompareSide left, CompareSide right, QWidget* parent)
    : QWidget(parent), m_left(std::move(left)), m_right(std::move(right))
{
    const QSettings settings;
    m_leftLabel = new QLabel(this);
    m_rightLabel = new QLabel(this);
    for (QLabel* label : {m_leftLabel, m_rightLabel}) {
        label->setTextFormat(Qt::RichText);
        label->setMinimumWidth(1);
    }
    m_swap = new QToolButton(this);
    m_swap->setText(QStringLiteral("⇄"));
    m_swap->setToolTip(tr("Swap the two sides"));
    m_swap->setAutoRaise(true);
    connect(m_swap, &QToolButton::clicked, this, &CompareTab::swapSides);

    const auto action = [this](const QString& text, const QString& tip, const QKeySequence& shortcut, auto slot) {
        auto* a = new QAction(text, this);
        a->setShortcut(shortcut);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        a->setToolTip(shortcut.isEmpty() ? tip
                                         : QStringLiteral("%1 (%2)").arg(tip, shortcut.toString(QKeySequence::NativeText)));
        addAction(a);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    const auto button = [this](QAction* a) {
        auto* tool = new QToolButton(this);
        tool->setDefaultAction(a);
        tool->setAutoRaise(true);
        return tool;
    };
    m_previous = action(tr("◀ Previous"), tr("The previous change"), QKeySequence(Qt::SHIFT | Qt::Key_F7), [this] {
        if (m_binary)
            m_hexView->goToPreviousDifference();
        else
            m_view->goToPreviousDifference();
    });
    m_next = action(tr("Next ▶"), tr("The next change"), QKeySequence(Qt::Key_F7), [this] {
        if (m_binary)
            m_hexView->goToNextDifference();
        else
            m_view->goToNextDifference();
    });
    m_refresh =
        action(tr("Refresh"), tr("Read both sides again and compare them"), QKeySequence(Qt::Key_F5), &CompareTab::refresh);
    m_ai = action(tr("Ask AI What Changed"),
                  tr("The AI gets the differences (a unified diff; for binaries, where they lie and the code around "
                     "them) and says what changed"),
                  QKeySequence(), &CompareTab::askAi);

    const auto option = [this, &settings](const QString& text, const QString& tip, const char* name, bool fallback) {
        auto* box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked(settings.value(key(name), fallback).toBool());
        return box;
    };
    m_onlyDifferences = option(tr("Only differences"),
                               tr("Fold the lines that are the same, but for a few around each change"), "onlyDifferences",
                               false);
    m_ignoreWhitespace = option(tr("Ignore whitespace"), tr("Spaces and tabs added, removed or changed do not count"),
                                "ignoreWhitespace", false);
    m_ignoreCase = option(tr("Ignore case"), tr("Capital and small letters count as the same"), "ignoreCase", false);
    m_ignoreLineEndings = option(tr("Ignore line endings"),
                                 tr("CRLF and LF count as the same, and so does a missing line break at the end"),
                                 "ignoreLineEndings", true);
    m_compareBytes = new QCheckBox(tr("Compare bytes"), this);
    m_compareBytes->setToolTip(tr("Compare byte by byte in hex, at the same offsets (binary files always are)"));
    connect(m_onlyDifferences, &QCheckBox::toggled, this, [this](bool on) {
        m_view->setOnlyDifferences(on);
        saveOptions();
    });
    for (QCheckBox* box : {m_ignoreWhitespace, m_ignoreCase, m_ignoreLineEndings, m_compareBytes})
        connect(box, &QCheckBox::toggled, this, [this] {
            saveOptions();
            compare();
        });

    m_view = new CompareView(this);
    connect(m_view, &CompareView::currentChanged, this, &CompareTab::updateButtons);
    connect(m_view, &CompareView::zoomRequested, this, &CompareTab::zoomRequested);
    connect(m_view, &CompareView::openRequested, this, [this](bool onLeft, std::uint64_t line) {
        const CompareSide& side = onLeft ? m_left : m_right;
        const std::shared_ptr<core::LineIndex>& index = onLeft ? m_comparison.left : m_comparison.right;
        if (side.path.isEmpty() || !index)
            return;
        const std::optional<std::uint64_t> row = index->rowIndexOfLine(line);
        emit openRequested(side.path, line, row ? index->row(*row).start : 0);
    });

    m_hexView = new HexCompareView(this);
    m_details = new QTextBrowser(this);
    m_details->setOpenLinks(false);
    connect(m_hexView, &HexCompareView::currentChanged, this, [this] {
        updateButtons();
        updateDetails();
    });
    connect(m_hexView, &HexCompareView::zoomRequested, this, &CompareTab::zoomRequested);
    connect(m_hexView, &HexCompareView::openRequested, this, [this](bool onLeft, std::uint64_t offset) {
        const CompareSide& side = onLeft ? m_left : m_right;
        if (!side.path.isEmpty())
            emit openOffsetRequested(side.path, offset);
    });
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_hexView);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    m_binaryPage = splitter;

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_view);
    m_pages->addWidget(m_binaryPage);

    auto* names = new QHBoxLayout;
    names->setContentsMargins(6, 4, 6, 0);
    names->addWidget(m_leftLabel, 1);
    names->addWidget(m_swap);
    names->addWidget(m_rightLabel, 1);
    auto* tools = new QHBoxLayout;
    tools->setContentsMargins(4, 0, 6, 0);
    tools->setSpacing(6);
    tools->addWidget(button(m_previous));
    tools->addWidget(button(m_next));
    tools->addSpacing(8);
    tools->addWidget(m_onlyDifferences);
    tools->addWidget(m_ignoreWhitespace);
    tools->addWidget(m_ignoreCase);
    tools->addWidget(m_ignoreLineEndings);
    tools->addWidget(m_compareBytes);
    tools->addSpacing(8);
    m_refreshButton = button(m_refresh);
    tools->addWidget(m_refreshButton);
    tools->addWidget(button(m_ai));
    tools->addStretch(1);
    // For a review: in or out with each change, and applying those that are in.
    m_toggle = action(tr("Reject This Change"), tr("Leave this change out, or take it back in"),
                      QKeySequence(Qt::Key_Space), &CompareTab::toggleCurrentChange);
    m_acceptAll = action(tr("Accept All"), tr("Take in every change"), QKeySequence(), &CompareTab::acceptAll);
    m_rejectAll = action(tr("Reject All"), tr("Leave out every change"), QKeySequence(), &CompareTab::rejectAll);
    m_apply = action(tr("Apply"), tr("Write the accepted changes, as one step to undo"),
                     QKeySequence(Qt::CTRL | Qt::Key_Return), [this] { applyAccepted(); });
    m_apply->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)});
    for (QAction* a : {m_toggle, m_acceptAll, m_rejectAll, m_apply})
        a->setEnabled(false); // their keys too, until there is a review
    m_reviewTools = new QWidget(this);
    auto* review = new QHBoxLayout(m_reviewTools);
    review->setContentsMargins(4, 0, 6, 0);
    review->setSpacing(6);
    review->addWidget(button(m_toggle));
    review->addWidget(button(m_acceptAll));
    review->addWidget(button(m_rejectAll));
    review->addStretch(1);
    auto* apply = new QToolButton(this);
    apply->setDefaultAction(m_apply);
    apply->setToolButtonStyle(Qt::ToolButtonTextOnly);
    review->addWidget(apply);
    m_reviewTools->hide();
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_note->setContentsMargins(6, 2, 6, 4);
    m_note->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(names);
    layout->addLayout(tools);
    layout->addWidget(m_reviewTools);
    layout->addWidget(m_pages, 1);
    layout->addWidget(m_note);

    connect(&m_textWatcher, &QFutureWatcher<TextComparison>::finished, this, &CompareTab::textFinished);
    connect(&m_binaryWatcher, &QFutureWatcher<BinaryResult>::finished, this, &CompareTab::binaryFinished);
    updateLabels();
    compare();
}

CompareTab::~CompareTab()
{
    m_textWatcher.cancel();
    m_binaryWatcher.cancel();
    m_textWatcher.waitForFinished();
    m_binaryWatcher.waitForFinished();
}

QString CompareTab::title() const
{
    QString left = m_left.label;
    QString right = m_right.label;
    // Two files of the same name: their folders tell them apart.
    if (left == right && !m_left.path.isEmpty() && !m_right.path.isEmpty()) {
        left = QFileInfo(m_left.path).dir().dirName() + QLatin1Char('/') + left;
        right = QFileInfo(m_right.path).dir().dirName() + QLatin1Char('/') + right;
    }
    return QStringLiteral("%1 ↔ %2").arg(left, right);
}

QString CompareTab::toolTip() const
{
    const auto where = [](const CompareSide& side) {
        return side.path.isEmpty() ? side.label : QDir::toNativeSeparators(side.path);
    };
    return QStringLiteral("%1\n↔ %2").arg(where(m_left), where(m_right));
}

bool CompareTab::isComparing() const
{
    return m_comparing;
}

QString CompareTab::detailsText() const
{
    return m_details->toPlainText();
}

CompareOptions CompareTab::options() const
{
    CompareOptions options;
    options.ignoreWhitespace = m_ignoreWhitespace->isChecked();
    options.ignoreCase = m_ignoreCase->isChecked();
    options.ignoreLineEndings = m_ignoreLineEndings->isChecked();
    return options;
}

void CompareTab::setOptions(const CompareOptions& options)
{
    const QSignalBlocker a(m_ignoreWhitespace);
    const QSignalBlocker b(m_ignoreCase);
    const QSignalBlocker c(m_ignoreLineEndings);
    m_ignoreWhitespace->setChecked(options.ignoreWhitespace);
    m_ignoreCase->setChecked(options.ignoreCase);
    m_ignoreLineEndings->setChecked(options.ignoreLineEndings);
    compare();
}

bool CompareTab::onlyDifferences() const
{
    return m_onlyDifferences->isChecked();
}

void CompareTab::setOnlyDifferences(bool on)
{
    m_onlyDifferences->setChecked(on);
}

void CompareTab::setCompareBytes(bool on)
{
    m_compareBytes->setChecked(on);
}

void CompareTab::refresh()
{
    for (CompareSide* side : {&m_left, &m_right}) {
        if (!side->reload)
            continue;
        if (std::shared_ptr<const core::FileSource> source = side->reload()) {
            side->source = std::move(source);
            side->type = core::detectFileType(side->source->bytes());
        }
    }
    compare();
}

void CompareTab::swapSides()
{
    std::swap(m_left, m_right);
    updateLabels();
    emit titleChanged();
    compare();
}

void CompareTab::compare()
{
    m_textWatcher.cancel();
    m_binaryWatcher.cancel();
    m_textWatcher.waitForFinished();
    m_binaryWatcher.waitForFinished();
    m_comparing = true;
    // Binary files are compared byte by byte whatever is asked.
    m_binary = m_compareBytes->isChecked() || isBinaryFile(m_left) || isBinaryFile(m_right);
    updateOptions();
    m_pages->setCurrentWidget(m_binary ? m_binaryPage : static_cast<QWidget*>(m_view));
    setStatus(tr("Comparing…"));
    updateButtons();
    if (m_binary) {
        m_binaryWatcher.setFuture(QtConcurrent::run([left = m_left, right = m_right](QPromise<BinaryResult>& promise) {
            BinaryResult result;
            result.bytes = core::compareBytes(left.source->bytes(), right.source->bytes(),
                                              [&promise] { return promise.isCanceled(); });
            result.left = core::readBinaryLayout(left.source->bytes());
            result.right = core::readBinaryLayout(right.source->bytes());
            promise.addResult(std::move(result));
        }));
    } else {
        m_textWatcher.setFuture(
            QtConcurrent::run([left = m_left, right = m_right, options = options()](QPromise<TextComparison>& promise) {
                promise.addResult(compareText(left, right, options, [&promise] { return promise.isCanceled(); }));
            }));
    }
}

void CompareTab::textFinished()
{
    if (m_textWatcher.isCanceled() || m_textWatcher.future().resultCount() == 0)
        return; // another comparison takes its place
    TextComparison comparison = m_textWatcher.result();
    if (comparison.cancelled)
        return;
    m_comparison = std::move(comparison);
    m_comparing = false;
    m_view->setComparison(m_left, m_right, m_comparison, m_onlyDifferences->isChecked());
    if (m_review)
        m_accepted.assign(m_comparison.hunks.size(), true);
    updateStatus();
    updateButtons();
    emit compared();
}

void CompareTab::binaryFinished()
{
    if (m_binaryWatcher.isCanceled() || m_binaryWatcher.future().resultCount() == 0)
        return;
    BinaryResult result = m_binaryWatcher.result();
    if (result.bytes.cancelled)
        return;
    m_bytes = std::move(result.bytes);
    m_leftLayout = std::move(result.left);
    m_rightLayout = std::move(result.right);
    m_comparing = false;
    m_hexView->setComparison(m_left, m_right, m_bytes);
    updateStatus();
    updateButtons();
    updateDetails();
    emit compared();
}

void CompareTab::updateLabels()
{
    // The name first, so that a narrow window still shows it; where it is, dimmed, after it.
    const QString dim = palette().color(QPalette::PlaceholderText).name();
    for (const auto& [label, side] : {std::pair{m_leftLabel, &m_left}, std::pair{m_rightLabel, &m_right}}) {
        const QString folder =
            side->path.isEmpty() ? QString() : QDir::toNativeSeparators(QFileInfo(side->path).absolutePath());
        const QString details =
            folder.isEmpty() ? codecName(*side) : QStringLiteral("%1 · %2").arg(folder, codecName(*side));
        label->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;<span style=\"color:%2\">%3</span>")
                           .arg(side->label.toHtmlEscaped(), dim, details.toHtmlEscaped()));
        label->setToolTip(side->path.isEmpty() ? side->label : QDir::toNativeSeparators(side->path));
    }
}

void CompareTab::updateOptions()
{
    // What only means something for lines.
    for (QCheckBox* box : {m_onlyDifferences, m_ignoreWhitespace, m_ignoreCase, m_ignoreLineEndings})
        box->setEnabled(!m_binary);
    const bool forced = isBinaryFile(m_left) || isBinaryFile(m_right);
    m_compareBytes->setEnabled(!forced);
    if (forced) {
        const QSignalBlocker blocker(m_compareBytes);
        m_compareBytes->setChecked(true);
    }
}

void CompareTab::updateStatus()
{
    const QLocale locale;
    QStringList parts;
    if (m_binary) {
        const auto bytes = [&locale](std::uint64_t n) { return locale.toString(static_cast<qulonglong>(n)); };
        if (m_bytes.ranges.empty())
            parts << tr("The two are identical");
        else
            parts << tr("%n difference(s), %1 bytes in all", "", static_cast<int>(std::min<std::size_t>(m_bytes.ranges.size(), INT_MAX)))
                         .arg(bytes(m_bytes.differentBytes));
        if (m_bytes.leftSize != m_bytes.rightSize)
            parts << tr("sizes %1 and %2 bytes").arg(bytes(m_bytes.leftSize), bytes(m_bytes.rightSize));
        if (!m_bytes.segments.empty())
            parts << tr("lined up again after bytes inserted or removed");
        else if (m_bytes.shiftAt)
            parts << tr("shifted by %1 bytes from %2").arg(QString::number(m_bytes.shift), hex(*m_bytes.shiftAt));
        if (m_leftLayout)
            parts << QStringLiteral("%1 %2").arg(QString::fromStdString(m_leftLayout->format),
                                                 QString::fromLatin1(core::architectureName(m_leftLayout->architecture)));
        setStatus(parts.join(QStringLiteral(" · ")));
        return;
    }
    const std::vector<core::DiffHunk>& hunks = m_comparison.hunks;
    if (hunks.empty()) {
        parts << (m_comparison.identicalBytes ? tr("The two are identical")
                                              : tr("No differences, apart from what is ignored"));
    } else {
        std::uint64_t removed = 0;
        std::uint64_t added = 0;
        for (const core::DiffHunk& hunk : hunks) {
            removed += hunk.leftCount;
            added += hunk.rightCount;
        }
        parts << tr("%n change(s): %1 line(s) removed, %2 added", "", static_cast<int>(hunks.size()))
                     .arg(locale.toString(static_cast<qulonglong>(removed)),
                          locale.toString(static_cast<qulonglong>(added)));
        if (m_review)
            parts << tr("%1 accepted").arg(std::count(m_accepted.begin(), m_accepted.end(), true));
    }
    if (m_left.codec->name != m_right.codec->name)
        parts << tr("read as %1 and %2, compared as text").arg(codecName(m_left), codecName(m_right));
    const QString leftEnding = lineEnding(m_left);
    const QString rightEnding = lineEnding(m_right);
    if (!leftEnding.isEmpty() && !rightEnding.isEmpty() && leftEnding != rightEnding)
        parts << tr("line endings %1 and %2").arg(leftEnding, rightEnding);
    setStatus(parts.join(QStringLiteral(" · ")));
}

QString CompareTab::where(const std::optional<core::BinaryLayout>& layout, std::uint64_t offset) const
{
    if (!layout)
        return {};
    const core::Section* section = layout->sectionAt(offset);
    if (!section)
        return tr("in the headers or between sections");
    const std::uint64_t address = section->address + (offset - section->fileOffset);
    const QString name = section->name.empty() ? tr("a section") : QString::fromStdString(section->name);
    QString text = layout->format.starts_with("PE")
                       ? tr("in %1, address %2 (RVA %3)").arg(name, hex(address), hex(address - layout->imageBase))
                       : tr("in %1, address %2").arg(name, hex(address));
    if (section->executable)
        text += tr(", code");
    return text;
}

void CompareTab::updateDetails()
{
    if (!m_binary || !m_hexView->hasComparison()) {
        m_details->clear();
        return;
    }
    const int index = m_hexView->currentRange();
    const std::uint64_t position = index >= 0 ? m_bytes.ranges[static_cast<std::size_t>(index)].start : m_hexView->cursorOffset();
    const QLocale locale;
    const QString font = m_hexView->font().family().toHtmlEscaped();
    QString html = QStringLiteral("<style>pre { font-family: '%1'; margin: 0; } td { padding: 0 12px 0 0; vertical-align: top; }"
                                  " .changed { font-weight: bold; }</style>")
                       .arg(font);
    const bool linedUp = !m_bytes.segments.empty();
    // Each side's bytes in the difference, or at the cursor: lined up, they lie at offsets of
    // their own.
    const core::ByteRange positions =
        index >= 0 ? m_bytes.ranges[static_cast<std::size_t>(index)] : core::ByteRange{position, 1};
    const core::SideRanges sides = core::sideRanges(m_bytes, positions);
    if (index >= 0 && linedUp) {
        const auto describe = [&](const core::ByteRange& r) {
            return r.length == 0 ? tr("none (at %1)").arg(hex(r.start))
                                 : tr("%1–%2, %n byte(s)", "", static_cast<int>(std::min<std::uint64_t>(r.length, INT_MAX)))
                                       .arg(hex(r.start), hex(r.end() - 1));
        };
        html += QStringLiteral("<p><b>%1</b></p>")
                    .arg(tr("Difference %1 of %2: left %3; right %4")
                             .arg(locale.toString(index + 1), locale.toString(static_cast<qulonglong>(m_bytes.ranges.size())),
                                  describe(sides.left), describe(sides.right))
                             .toHtmlEscaped());
    } else if (index >= 0) {
        const core::ByteRange& range = m_bytes.ranges[static_cast<std::size_t>(index)];
        html += QStringLiteral("<p><b>%1</b></p>")
                    .arg(tr("Difference %1 of %2: %3–%4, %n byte(s)", "", static_cast<int>(std::min<std::uint64_t>(range.length, INT_MAX)))
                             .arg(locale.toString(index + 1), locale.toString(static_cast<qulonglong>(m_bytes.ranges.size())),
                                  hex(range.start), hex(range.end() - 1))
                             .toHtmlEscaped());
    } else if (linedUp) {
        html += QStringLiteral("<p><b>%1</b></p>")
                    .arg(tr("The same on both sides: left %1, right %2").arg(hex(sides.left.start), hex(sides.right.start))
                             .toHtmlEscaped());
    } else {
        html += QStringLiteral("<p><b>%1</b></p>").arg(tr("Offset %1: the same on both sides").arg(hex(position)).toHtmlEscaped());
    }
    if (linedUp)
        html += QStringLiteral("<p>%1</p>")
                    .arg(tr("Bytes were inserted or removed: the two sides are lined up again after them, each with "
                            "its own offsets.")
                             .toHtmlEscaped());
    else if (m_bytes.shiftAt && position >= *m_bytes.shiftAt)
        html += QStringLiteral("<p style=\"color:#c62828\">%1</p>")
                    .arg(tr("From %1 on, the right side is shifted by %2 bytes (bytes were inserted or removed): "
                            "comparing at the same offsets means little here.")
                             .arg(hex(*m_bytes.shiftAt), QString::number(m_bytes.shift))
                             .toHtmlEscaped());

    // Each side: where it lies, its bytes, and in code its instructions around the difference.
    const auto side = [&](bool onLeft) {
        const CompareSide& s = onLeft ? m_left : m_right;
        const std::optional<core::BinaryLayout>& layout = onLeft ? m_leftLayout : m_rightLayout;
        const auto bytes = s.source->bytes();
        const core::ByteRange range = onLeft ? sides.left : sides.right;
        const std::uint64_t offset = range.start;
        QString cell = QStringLiteral("<p><b>%1</b>").arg(s.label.toHtmlEscaped());
        if (const QString place = where(layout, offset); !place.isEmpty())
            cell += QStringLiteral("<br>%1").arg(place.toHtmlEscaped());
        cell += QStringLiteral("</p>");
        if (offset >= bytes.size())
            return cell + QStringLiteral("<p>%1</p>").arg(tr("(past its end)").toHtmlEscaped());
        cell += QStringLiteral("<pre>%1</pre>").arg(hexBytes(bytes, offset, kDetailsBytes).toHtmlEscaped());
        if (layout) {
            const std::vector<core::Instruction> code = core::disassembleAround(bytes, *layout, offset, kDetailsBefore, kDetailsAfter);
            QString lines;
            for (const core::Instruction& instruction : code) {
                const bool changed =
                    instruction.offset < range.end() && instruction.offset + instruction.bytes.size() > range.start;
                QString codeBytes;
                for (const std::uint8_t byte : instruction.bytes)
                    codeBytes += QStringLiteral("%1 ").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
                const QString line = QStringLiteral("%1  %2 %3")
                                         .arg(hex(instruction.address), codeBytes.leftJustified(24),
                                              QString::fromStdString(instruction.text))
                                         .toHtmlEscaped();
                lines += changed ? QStringLiteral("<span class=\"changed\">▶ %1</span>\n").arg(line)
                                 : QStringLiteral("  %1\n").arg(line);
            }
            if (!lines.isEmpty())
                cell += QStringLiteral("<pre>%1</pre>").arg(lines);
        }
        return cell;
    };
    html += QStringLiteral("<table><tr><td>%1</td><td>%2</td></tr></table>").arg(side(true), side(false));
    if (m_leftLayout && !core::canDisassemble(m_leftLayout->architecture))
        html += QStringLiteral("<p>%1</p>").arg(tr("Built without Capstone: no disassembly.").toHtmlEscaped());
    m_details->setHtml(html);
}

void CompareTab::setStatus(const QString& text)
{
    if (text == m_statusText)
        return;
    m_statusText = text;
    emit statusChanged(text);
}

void CompareTab::updateButtons()
{
    if (m_binary) {
        const bool ready = !isComparing() && m_hexView->hasComparison();
        const auto& ranges = m_bytes.ranges;
        const std::uint64_t cursor = m_hexView->cursorOffset();
        m_next->setEnabled(ready && !ranges.empty() && cursor < ranges.back().start);
        m_previous->setEnabled(ready && !ranges.empty() && cursor > ranges.front().start);
        m_ai->setEnabled(ready && !ranges.empty());
        return;
    }
    const bool ready = !isComparing() && m_view->hasComparison();
    const int hunks = ready ? m_view->rows().hunkCount() : 0;
    const int current = m_view->currentHunk();
    const std::uint64_t row = m_view->currentRow();
    m_next->setEnabled(hunks > 0 && (current >= 0 ? current + 1 < hunks : m_view->rows().hunkAtOrAfter(row) >= 0));
    m_previous->setEnabled(hunks > 0 && (current >= 0 ? current > 0 : m_view->rows().hunkBefore(row) >= 0));
    m_ai->setEnabled(hunks > 0);
    if (m_review)
        updateReview();
}

void CompareTab::setReview(Review review)
{
    m_review = std::move(review);
    // Changes as they are: nothing ignored but how lines end, and nothing to swap or read again.
    {
        const QSignalBlocker blocker(m_compareBytes);
        m_compareBytes->setChecked(false);
    }
    for (QWidget* widget : {static_cast<QWidget*>(m_ignoreWhitespace), static_cast<QWidget*>(m_ignoreCase),
                            static_cast<QWidget*>(m_ignoreLineEndings), static_cast<QWidget*>(m_compareBytes),
                            static_cast<QWidget*>(m_swap), static_cast<QWidget*>(m_refreshButton)})
        widget->hide();
    m_refresh->setEnabled(false);
    m_apply->setText(m_review->applyLabel.isEmpty() ? tr("Apply") : m_review->applyLabel);
    m_reviewTools->show();
    m_note->setText(m_review->note);
    m_note->setVisible(!m_review->note.isEmpty());
    setOptions(CompareOptions{});
}

void CompareTab::setAccepted(int change, bool accepted)
{
    if (!m_review || change < 0 || static_cast<std::size_t>(change) >= m_accepted.size())
        return;
    m_accepted[static_cast<std::size_t>(change)] = accepted;
    updateReview();
}

void CompareTab::toggleCurrentChange()
{
    const int change = m_view->currentHunk();
    if (m_review && change >= 0 && static_cast<std::size_t>(change) < m_accepted.size())
        setAccepted(change, !m_accepted[static_cast<std::size_t>(change)]);
}

void CompareTab::acceptAll()
{
    std::fill(m_accepted.begin(), m_accepted.end(), true);
    updateReview();
}

void CompareTab::rejectAll()
{
    std::fill(m_accepted.begin(), m_accepted.end(), false);
    updateReview();
}

bool CompareTab::applyAccepted()
{
    if (!m_review || !m_review->apply || isComparing() || !m_view->hasComparison())
        return false;
    const int accepted = static_cast<int>(std::count(m_accepted.begin(), m_accepted.end(), true));
    if (accepted == 0)
        return false;
    const QByteArray bytes = acceptChanges(m_comparison, m_left.source->bytes(), m_right.source->bytes(), m_accepted);
    if (!m_review->apply(bytes, accepted, static_cast<int>(m_accepted.size())))
        return false;
    emit applied();
    return true;
}

void CompareTab::updateReview()
{
    std::vector<bool> rejected(m_accepted.size());
    for (std::size_t i = 0; i < m_accepted.size(); ++i)
        rejected[i] = !m_accepted[i];
    m_view->setRejected(std::move(rejected));
    const bool ready = !isComparing() && m_view->hasComparison();
    const int accepted = static_cast<int>(std::count(m_accepted.begin(), m_accepted.end(), true));
    const int changes = static_cast<int>(m_accepted.size());
    const int current = m_view->currentHunk();
    const bool currentAccepted = current >= 0 && static_cast<std::size_t>(current) < m_accepted.size() &&
                                 m_accepted[static_cast<std::size_t>(current)];
    m_toggle->setText(currentAccepted || current < 0 ? tr("Reject This Change") : tr("Accept This Change"));
    m_toggle->setEnabled(ready && current >= 0);
    m_acceptAll->setEnabled(ready && accepted < changes);
    m_rejectAll->setEnabled(ready && accepted > 0);
    m_apply->setEnabled(ready && accepted > 0);
    if (ready)
        updateStatus();
}

void CompareTab::saveOptions() const
{
    QSettings settings;
    settings.setValue(key("onlyDifferences"), m_onlyDifferences->isChecked());
    settings.setValue(key("ignoreWhitespace"), m_ignoreWhitespace->isChecked());
    settings.setValue(key("ignoreCase"), m_ignoreCase->isChecked());
    settings.setValue(key("ignoreLineEndings"), m_ignoreLineEndings->isChecked());
}

void CompareTab::askAi()
{
    bool cut = false;
    if (m_binary) {
        if (m_bytes.ranges.empty())
            return;
        const QString report = binaryReport(m_left, m_right, m_bytes, m_leftLayout, m_rightLayout, &cut);
        const QString question =
            tr("These are the differences between two binary files, probably two versions or builds of the same "
               "program. Say what each change most likely is (a patched instruction, a changed constant or string, a "
               "timestamp or checksum) and what it does. The disassembly was made by Capstone on this computer: rely on "
               "it rather than decoding bytes yourself.");
        const QString label = tr("the differences between %1 and %2, with the code around them").arg(m_left.label, m_right.label);
        emit aiRequested(question, cut ? tr("%1 (cut short)").arg(label) : label, report);
        return;
    }
    if (m_comparison.hunks.empty())
        return;
    const QString diff = unifiedDiff(m_left, m_right, m_comparison, kMaxAiChars, &cut);
    const QString question =
        tr("What changed between these two versions? Sum up the changes, the most important first, say what they do, "
           "and point out anything that looks like a mistake.");
    const QString label = cut ? tr("the start of the differences between %1 and %2 (a unified diff, cut short)")
                                    .arg(m_left.label, m_right.label)
                              : tr("the differences between %1 and %2 (a unified diff)").arg(m_left.label, m_right.label);
    emit aiRequested(question, label, diff);
}

} // namespace aiedit::ui
