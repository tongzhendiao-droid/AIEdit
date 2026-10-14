#pragma once

#include "core/FileType.h"
#include "core/TextCodec.h"

#include <QFileSystemWatcher>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

class QSplitter;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

struct AiContext;
class FindBar;
class HexView;
class TextDocument;
class TextView;

// One open file: its bytes, what kind of file it is, and the text and hex views showing it.
// Both views always share the caret and selection, so switching or splitting loses nothing.
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
    QString filePath() const;
    HexView* hexView() const { return m_hexView; }
    TextView* textView() const { return m_textView; }
    TextDocument* textDocument() const { return m_textDocument; }
    FindBar* findBar() const { return m_findBar; }

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    // The view the user works in: the visible one, or in split mode the one focused last.
    bool isTextActive() const;
    std::uint64_t cursorOffset() const;

    void setCodec(const core::TextCodec& codec);
    // Syntax highlighting definition name; empty for plain text.
    QString syntax() const;
    void setSyntax(const QString& name);

    // What the AI assistant may attach: the selection (text, or a hex dump) and a file summary.
    AiContext aiContext() const;

    // Follow mode (like tail -f): after every reload, jump to the end of the file.
    bool isFollowing() const { return m_following; }
    void setFollowing(bool following);

public slots:
    void copy();
    void selectAll();
    // Shows the find bar, seeded with a short selection.
    void showFindBar();
    // Finds the next or previous match of the find bar's query from the caret or selection.
    void findNext(bool forward = true);
    // Re-reads the file from disk, keeping the caret, view mode and codec. This also happens on
    // its own, shortly after the file changes.
    bool reload();

signals:
    // Caret, selection, view mode or codec changed: anything the status bar shows.
    void stateChanged();
    // The file was reloaded: size and contents may differ.
    void sourceChanged();
    // The text index grew or finished.
    void indexProgressed();
    void zoomRequested(int steps);
    void statusMessage(const QString& message);

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
    void fileChanged();
    void moveToEnd();

    std::shared_ptr<const core::FileSource> m_source;
    core::FileTypeInfo m_fileType;
    TextDocument* m_textDocument = nullptr;
    QSplitter* m_splitter = nullptr;
    TextView* m_textView = nullptr;
    HexView* m_hexView = nullptr;
    FindBar* m_findBar = nullptr;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> m_lastMatch;
    ViewMode m_viewMode = ViewMode::Hex;
    bool m_textFocusedLast = true;
    bool m_syncing = false;
    bool m_splitSizesPending = false; // sized once the splitter has its real width
    bool m_following = false;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer; // coalesces bursts of change notifications
};

} // namespace aiedit::ui
