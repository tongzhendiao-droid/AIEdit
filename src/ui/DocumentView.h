#pragma once

#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/LineOperations.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFutureWatcher>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

class QAction;
class QSplitter;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

struct AiContext;
struct AiEditRequest;
struct FieldMark;
struct SearchContext;
class AiEditBar;
class AiEditor;
class FilterPanel;
class FindBar;
class HexView;
class MarkdownView;
class TextDocument;
class TextView;

// One open file: its bytes, what kind of file it is, and the text and hex views showing it.
// Both views always share the caret and selection, so switching or splitting loses nothing.
// Markdown is drawn too: beside the text as it is written, and in its place in reading mode.
//
// Text files up to core::EditBuffer::kMaxEditableSize can be edited in the text view. With
// unsaved edits, changes on disk no longer reload the file; saving writes over them.
class DocumentView : public QWidget
{
    Q_OBJECT

public:
    enum class ViewMode
    {
        Text,
        Hex,
        Split,
        Preview, // the text, and the Markdown drawn beside it (Markdown documents only)
    };

    DocumentView(std::shared_ptr<const core::FileSource> source, const core::FileTypeInfo& fileType,
                 const core::TextCodec& codec, QWidget* parent = nullptr);
    ~DocumentView() override;

    const std::shared_ptr<const core::FileSource>& source() const { return m_source; }
    const core::FileTypeInfo& fileType() const { return m_fileType; }
    // Empty for a new document that has not been saved yet.
    QString filePath() const;
    bool isUntitled() const;
    // What tabs and questions call it: the file name, or a new document's title until it is saved.
    QString title() const;
    void setUntitledTitle(const QString& title) { m_untitledTitle = title; }
    HexView* hexView() const { return m_hexView; }
    TextView* textView() const { return m_textView; }
    TextDocument* textDocument() const { return m_textDocument; }
    FindBar* findBar() const { return m_findBar; }
    // The log filter under the document; created when first shown.
    FilterPanel* filterPanel() const { return m_filterPanel; }
    bool isFilterPanelVisible() const;

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    // Reading mode (see TextView::setReadingMode), in the text view. Remembered with the file.
    bool isReadingMode() const;
    void setReadingMode(bool on);
    // Whether the window shows the contents beside this document: as it is in reading mode or
    // not, unless shown or closed by hand while this document was in front.
    bool showsContents() const { return m_showsContents; }
    void setShowsContents(bool on) { m_showsContents = on; }
    // Shows the text from `offset` on at the top, with the caret there: where a chapter starts.
    void showFromOffset(std::uint64_t offset);

    // Markdown, by the file's name or the syntax chosen for it.
    bool isMarkdown() const;
    // Reading mode on Markdown not too large to draw: it is read drawn, not as text.
    bool showsDrawnMarkdown() const;
    // Where the Markdown is drawn; made when first shown.
    MarkdownView* markdownView() const { return m_markdownView; }
    // The font it is drawn in.
    void setMarkdownFont(const QFont& font);
    enum class ExportFormat
    {
        Html,
        Pdf,
    };
    // Writes the document as it is now, drawn as on paper.
    bool exportMarkdown(const QString& path, ExportFormat format, QString* error = nullptr) const;
    // Writing Markdown: on the selection, or else the word or the line at the caret.
    enum class MarkdownEdit
    {
        Bold,          // **text**, or back to text
        Italic,        // *text*
        Strikethrough, // ~~text~~
        Code,          // `text`
        Link,          // [text](address)
        Heading1,      // the lines' marks, as markdown::toggleLinePrefix() gives and takes them
        Heading2,
        Heading3,
        BulletList,
        NumberedList,
        TaskList,
        Quote,
        CodeBlock,   // ``` above the lines and below them
        Table,       // a table to fill in, under the caret's line
        FormatTable, // the table at the caret, its columns lined up
    };
    // False if nothing changed; the status bar then says why.
    bool applyMarkdownEdit(MarkdownEdit edit);
    // The view the user works in: the visible one, or in split mode the one focused last.
    bool isTextActive() const;
    std::uint64_t cursorOffset() const;
    // Whether the active view has a selection worth handing to the AI.
    bool hasSelection() const;
    // Right-click actions for the text and the hex view (the AI actions).
    void setContextActions(const QList<QAction*>& textActions, const QList<QAction*>& hexActions);

    // The user's choice: it is remembered with the file (see State).
    void setCodec(const core::TextCodec& codec);
    // Syntax highlighting definition name; empty for plain text.
    QString syntax() const;
    void setSyntax(const QString& name);

    // What the AI assistant may attach: the selection (text, or a hex dump) and a file summary,
    // plus a target for its tools, which can mark fields in this view.
    AiContext aiContext();
    // Colours the fields in the hex view (and shows it, if only the text view was visible).
    void showFieldMarks(const QList<FieldMark>& marks);

    bool isEditable() const;
    bool isModified() const;
    // Replaces the whole text with bytes kept from an earlier session, as one edit: undoing it
    // goes back to the file as it is on disk.
    void restoreContent(std::span<const std::uint8_t> bytes);
    // Sorts, cleans, moves or copies the selected lines as one edit. Without a selection, sorting
    // and cleaning take the whole document, moving and copying the caret's line. False if it
    // changed nothing; either way the status bar hears what happened.
    bool applyLineOperation(LineOperation operation);
    // Comments the selected lines (or the caret's) out, or in again, as the document's
    // language writes comments (Ctrl+/). False if nothing changed; the status bar says why.
    bool toggleComment();
    // The selection, or the word at the caret, in upper or lower case.
    bool convertCase(bool upper);
    // Writes the document to its file, or to another (which it then shows). False, with the
    // reason in `error`, if that failed.
    bool save(QString* error = nullptr);
    bool saveAs(const QString& path, QString* error = nullptr);
    // The file was renamed or moved to `path` (from the folder panel): show it from there, where
    // it was left. Only for a document without unsaved changes; false if it cannot be opened.
    bool followMove(const QString& path);
    // Ends every line with `terminator` ("\n", "\r\n" or "\r"), as one edit to undo. False if
    // every line ended so already, or the document cannot be edited.
    bool convertLineEndings(const QString& terminator);

    enum class Conversion
    {
        Saved,
        Unreadable, // bytes that are not valid in the current codec would become U+FFFD
        Unwritable, // the new codec cannot hold some of the characters
        Failed,     // writing the file failed
    };
    // Writes the text in another codec (UTF-16 and UTF-32 with a byte order mark) and shows the
    // file in that codec from then on. Unreadable bytes stop it unless `replaceUnreadable`.
    Conversion saveWithCodec(const core::TextCodec& codec, bool replaceUnreadable, QString* error = nullptr);

    // What is remembered of a file between sessions, to come back to where it was left.
    struct State
    {
        QString path;
        std::uint64_t cursor = 0;
        std::uint64_t top = 0; // where the first line on screen started: the reading position
        ViewMode mode = ViewMode::Text;
        QString codec; // empty unless chosen by the user; otherwise the file's is detected again
        bool reading = false;
        std::vector<std::uint64_t> bookmarks; // where the bookmarked lines start
    };
    State state() const;
    // Puts it back; positions the index has not reached yet are applied once it has.
    void restoreState(const State& state);

    // Follow mode (like tail -f): after every reload, jump to the end of the file.
    bool isFollowing() const { return m_following; }
    void setFollowing(bool following);

    // An edit the AI proposed for [start, end) of the bytes, which held `before` when it was
    // asked; nothing is written until it is reviewed (see CompareTab).
    struct AiEditProposal
    {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::uint64_t firstLine = 0; // of the range, from 0
        QByteArray before;
        QByteArray after;
        QString instruction;
        QString note; // what the AI said of it
        QString rule; // a rule it wrote and that ran here, as JSON
    };
    // The bar that asks the AI to change the selection, or else the whole document (Ctrl+I).
    void showAiEditBar();
    AiEditBar* aiEditBar() const { return m_aiEditBar; }
    // Made when first needed; tests point it at a local server.
    AiEditor* aiEditor();
    // Asks the AI to change the selection, or the whole document, as the instruction says.
    void requestAiEdit(const QString& instruction);
    // Replaces [start, end) with `bytes` as one step to undo, if it still holds `before`; false,
    // changing nothing, if the document changed since.
    bool replaceIfUnchanged(std::uint64_t start, std::uint64_t end, const QByteArray& before, const QByteArray& bytes);

public slots:
    void copy();
    void selectAll();
    void cut();
    void paste();
    void undo();
    void redo();
    // Shows the find bar, seeded with a short selection; with `replace`, its replace row too.
    void showFindBar(bool replace = false);
    // Finds the next or previous match of the find bar's query from the caret or selection.
    void findNext(bool forward = true);
    // Finds the next or previous place of the selection, or of the word at the caret, which it
    // selects first (Ctrl+F3, as in Notepad++ and VS Code).
    void findSelection(bool forward = true);
    // Replaces the match found last, if it is still selected, then finds the next one.
    void replaceCurrent();
    // Replaces every match in the document, as one step to undo.
    void replaceAll();
    // Shows the panel that filters the lines of the document, and puts the cursor in it.
    void showFilterPanel();
    // Re-reads the file from disk, keeping the caret, view mode and codec. This also happens on
    // its own, shortly after the file changes.
    bool reload();

signals:
    // Caret, selection, view mode or codec changed: anything the status bar shows.
    void stateChanged();
    // The file was reloaded or edited: size and contents may differ.
    void sourceChanged();
    void modificationChanged(bool modified);
    // The document shows another file now (after Save As).
    void pathChanged();
    // The text index grew or finished.
    void indexProgressed();
    void zoomRequested(int steps);
    void statusMessage(const QString& message);
    void aiSettingsRequested();
    void readingModeChanged(bool on);
    // A link in the drawn Markdown to a file on disk was clicked.
    void fileLinkActivated(const QString& path);
    void aiEditProposed(const aiedit::ui::DocumentView::AiEditProposal& proposal);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Shows the views the mode asks for, and focuses the one worked in.
    void updateViews();
    QWidget* activeView() const;
    // The drawn Markdown is what the user works in: in reading mode, or clicked in the preview.
    bool isMarkdownActive() const;
    MarkdownView* ensureMarkdownView();
    // The text changed: drawn again after a pause, if it is shown.
    void markdownChanged();
    void drawMarkdown();
    void updateMarkdownEditing();
    // The text and the drawn Markdown scroll together, by the headings.
    void syncMarkdownFromText();
    void syncTextFromMarkdown();
    std::uint64_t lineAt(std::uint64_t offset) const;
    // Marks around the selection (or the word at the caret) within a line, or taken away.
    bool toggleInlineMark(const QString& mark);
    bool insertMarkdownLink();
    bool formatMarkdownTable();
    // The text of the lines `first` to `last`; nothing, with a word in the status bar, if it
    // would not be written back as the same bytes.
    std::optional<QStringList> markdownLines(std::uint64_t first, std::uint64_t last);
    // A place in lines about to be written: the line, and the character in it.
    struct TextPosition
    {
        qsizetype line = 0;
        qsizetype column = 0;
    };
    // Replaces the lines `first` to `last` with `lines`, as one step to undo, and selects from
    // `from` to `to` in them.
    bool replaceMarkdownLines(std::uint64_t first, std::uint64_t last, const QStringList& lines, TextPosition from,
                              TextPosition to);
    void syncFromText();
    void syncFromHex();
    void applySplitSizes();
    void selectMatch(std::uint64_t start, std::uint64_t end);
    void closeFindBar();
    void closeFilterPanel();
    void goToFilteredLine(std::uint64_t line, bool focus);
    void fileChanged();
    void moveToEnd();
    void updateEditable();
    void documentEdited();
    // The lines the selection takes in: the caret's line without one.
    std::pair<std::uint64_t, std::uint64_t> selectedLines() const;
    // What the AI is asked, for the selection or the whole document; the range it changes.
    AiEditRequest aiEditRequest(const QString& instruction, std::uint64_t* start, std::uint64_t* end) const;
    void updateAiEditScope();
    void aiTextWritten(const QString& text, const QString& note);
    void aiRuleRan();
    void aiEditFailed(const QString& message);
    void applyLineEdit(const LineEdit& edit);
    bool writeTo(const QString& path, QString* error);
    bool writeBytes(const QString& path, std::span<const std::uint8_t> bytes, QString* error);
    SearchContext searchContext() const;

    std::shared_ptr<const core::FileSource> m_source;
    core::FileTypeInfo m_fileType;
    TextDocument* m_textDocument = nullptr;
    QSplitter* m_documentSplitter = nullptr; // the views above, the filter panel below
    QSplitter* m_splitter = nullptr;
    TextView* m_textView = nullptr;
    HexView* m_hexView = nullptr;
    FindBar* m_findBar = nullptr;
    FilterPanel* m_filterPanel = nullptr;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> m_lastMatch;
    ViewMode m_viewMode = ViewMode::Hex;
    bool m_textFocusedLast = true;
    bool m_syncing = false;
    bool m_splitSizesPending = false; // sized once the splitter has its real width
    bool m_following = false;
    bool m_codecChosen = false; // by the user, rather than detected
    std::optional<State> m_pendingState; // waiting for the index to get there
    QString m_untitledTitle;
    AiEditBar* m_aiEditBar = nullptr;
    AiEditor* m_aiEditor = nullptr;
    AiEditProposal m_aiEdit; // what is being asked: the range, and what it held
    QString m_aiEditText;    // that text, decoded
    std::shared_ptr<const SearchContext> m_aiContext; // the text as it was asked about, for a rule
    struct RuleRun
    {
        std::optional<QByteArray> bytes;
        QString error;
        QString rule;
        QString note;
    };
    QFutureWatcher<RuleRun> m_ruleWatcher;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer; // coalesces bursts of change notifications
    bool m_showsContents = false;
    MarkdownView* m_markdownView = nullptr;
    QList<QAction*> m_textContextActions; // for the drawn Markdown too
    QFont m_markdownFont;
    QTimer m_markdownTimer;        // draws after a pause in typing
    bool m_markdownStale = true;   // the text changed since it was drawn
    qint64 m_markdownDrawMs = 0;   // how long drawing it took last
    QList<int> m_markdownHeadings; // the lines of the headings drawn
    bool m_markdownFocusedLast = false;
    bool m_syncingScroll = false;
    // The file's size and modification time as last seen, looked at every second for changes the
    // watcher misses: Windows reports a file that grows late, if at all, while its writer keeps
    // it open, and network folders may report nothing.
    // The look happens in the background: on a network drive that has gone away it can take long.
    struct DiskState
    {
        QString path;
        qint64 size = -1; // -1: no file
        QDateTime modified;

        bool operator==(const DiskState&) const = default;
    };
    static DiskState diskState(const QString& path);
    DiskState diskState() const;
    QTimer m_pollTimer;
    QFutureWatcher<DiskState> m_pollWatcher;
    bool m_polling = false; // a look is under way
    DiskState m_diskState;
};

} // namespace aiedit::ui
