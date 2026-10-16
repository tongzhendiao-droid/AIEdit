#pragma once

#include "ui/Compare.h"

#include <QFutureWatcher>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QCheckBox;
class QLabel;
class QStackedWidget;
class QTextBrowser;
class QToolButton;

namespace aiedit::ui {

class CompareView;
class HexCompareView;

// Two versions compared in a tab: side by side with the changes marked, from change to change
// (F7, Shift+F7), with what to ignore, and the AI asked what changed. Binary files (or any two,
// with "Compare bytes") are compared byte by byte in hex instead, with where each difference
// lies in an executable and the code around it, disassembled. The comparison runs in the
// background, and again when an option changes or on Refresh (F5), which reads both sides again.
class CompareTab : public QWidget
{
    Q_OBJECT

public:
    CompareTab(CompareSide left, CompareSide right, QWidget* parent = nullptr);
    ~CompareTab() override;

    // For the tab: "a.txt ↔ b.txt", with the folders when the names are the same.
    QString title() const;
    QString toolTip() const;
    CompareView* view() const { return m_view; }
    HexCompareView* hexView() const { return m_hexView; }
    const CompareSide& left() const { return m_left; }
    const CompareSide& right() const { return m_right; }
    const TextComparison& comparison() const { return m_comparison; }
    const core::ByteComparison& byteComparison() const { return m_bytes; }
    // Byte by byte (binary files, or "Compare bytes"), rather than line by line.
    bool isBinary() const { return m_binary; }
    // From starting until the result is shown.
    bool isComparing() const;
    // What the comparison found, for the status bar: "2 changes: 5 lines removed, 1 added".
    QString statusText() const { return m_statusText; }
    // About the current byte difference: where it lies, and the code around it.
    QString detailsText() const;

    CompareOptions options() const;
    void setOptions(const CompareOptions& options);
    bool onlyDifferences() const;
    void setOnlyDifferences(bool on);
    // Compares even text files byte by byte.
    void setCompareBytes(bool on);

    // Reviewing changes proposed for the left side (the AI's version, on the right): each change
    // can be accepted or rejected, and those accepted are applied. The sides are compared as
    // they are, nothing ignored but how lines end.
    struct Review
    {
        QString note;       // about the changes, shown under them
        QString applyLabel; // "Apply to main.cpp"
        // Writes the left side with the accepted changes made; false if that could not be done,
        // and the tab stays.
        std::function<bool(const QByteArray& bytes, int accepted, int changes)> apply;
    };
    void setReview(Review review);
    bool isReview() const { return m_review.has_value(); }
    // Which changes are accepted: all of them at first.
    const std::vector<bool>& accepted() const { return m_accepted; }
    void setAccepted(int change, bool accepted);

public slots:
    // Reads both sides again (the files from disk, a document as it is now) and compares them.
    void refresh();
    void swapSides();
    // Hands the AI the differences: a unified diff, or for binaries a report with the code.
    void askAi();
    // In a review: the change at the current row in or out (Space), all of them, and applying
    // those accepted (Ctrl+Enter); true if they were written.
    void toggleCurrentChange();
    void acceptAll();
    void rejectAll();
    bool applyAccepted();

signals:
    // A line, or a byte, of a side that is a file: open it there.
    void openRequested(const QString& path, std::uint64_t line, std::uint64_t offset);
    void openOffsetRequested(const QString& path, std::uint64_t offset);
    void aiRequested(const QString& question, const QString& label, const QString& text);
    void titleChanged();
    void zoomRequested(int steps);
    // A comparison finished and is shown.
    void compared();
    // A review's accepted changes were applied.
    void applied();
    void statusChanged(const QString& text);

private:
    struct BinaryResult
    {
        core::ByteComparison bytes;
        std::optional<core::BinaryLayout> left;
        std::optional<core::BinaryLayout> right;
    };

    void compare();
    void textFinished();
    void binaryFinished();
    void updateLabels();
    void updateStatus();
    void updateDetails();
    void setStatus(const QString& text);
    void updateButtons();
    void updateOptions();
    void saveOptions() const;
    void updateReview();
    // Where an offset lies in an executable, for the details: section, address.
    QString where(const std::optional<core::BinaryLayout>& layout, std::uint64_t offset) const;

    CompareSide m_left;
    CompareSide m_right;
    bool m_binary = false;
    bool m_comparing = false; // until the result is shown, which is after the worker is done
    TextComparison m_comparison;
    core::ByteComparison m_bytes;
    std::optional<core::BinaryLayout> m_leftLayout;
    std::optional<core::BinaryLayout> m_rightLayout;
    QFutureWatcher<TextComparison> m_textWatcher;
    QFutureWatcher<BinaryResult> m_binaryWatcher;

    QLabel* m_leftLabel = nullptr;
    QLabel* m_rightLabel = nullptr;
    // Actions, not their buttons, are enabled and disabled: that goes for their shortcuts too.
    QAction* m_previous = nullptr;
    QAction* m_next = nullptr;
    QAction* m_ai = nullptr;
    QAction* m_refresh = nullptr;
    QToolButton* m_refreshButton = nullptr;
    QToolButton* m_swap = nullptr;
    std::optional<Review> m_review;
    std::vector<bool> m_accepted;
    QWidget* m_reviewTools = nullptr;
    QAction* m_toggle = nullptr;
    QAction* m_acceptAll = nullptr;
    QAction* m_rejectAll = nullptr;
    QAction* m_apply = nullptr;
    QLabel* m_note = nullptr;
    QCheckBox* m_onlyDifferences = nullptr;
    QCheckBox* m_ignoreWhitespace = nullptr;
    QCheckBox* m_ignoreCase = nullptr;
    QCheckBox* m_ignoreLineEndings = nullptr;
    QCheckBox* m_compareBytes = nullptr;
    QString m_statusText;
    QStackedWidget* m_pages = nullptr;
    CompareView* m_view = nullptr;
    QWidget* m_binaryPage = nullptr;
    HexCompareView* m_hexView = nullptr;
    QTextBrowser* m_details = nullptr;
};

} // namespace aiedit::ui
