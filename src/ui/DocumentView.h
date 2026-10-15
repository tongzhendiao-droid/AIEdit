#pragma once

#include "core/FileType.h"
#include "core/TextCodec.h"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

class QAction;
class QSplitter;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

struct AiContext;
struct FieldMark;
struct SearchContext;
class FilterPanel;
class FindBar;
class HexView;
class TextDocument;
class TextView;

// One open file: its bytes, what kind of file it is, and the text and hex views showing it.
// Both views always share the caret and selection, so switching or splitting loses nothing.
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
    // Writes the document to its file, or to another (which it then shows). False, with the
    // reason in `error`, if that failed.
    bool save(QString* error = nullptr);
    bool saveAs(const QString& path, QString* error = nullptr);
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
    };
    State state() const;
    // Puts it back; positions the index has not reached yet are applied once it has.
    void restoreState(const State& state);

    // Follow mode (like tail -f): after every reload, jump to the end of the file.
    bool isFollowing() const { return m_following; }
    void setFollowing(bool following);

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
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
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer; // coalesces bursts of change notifications
};

} // namespace aiedit::ui
