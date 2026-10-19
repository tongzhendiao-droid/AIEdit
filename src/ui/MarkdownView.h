#pragma once

#include <QFont>
#include <QList>
#include <QPointer>
#include <QTextBrowser>

class QAction;

namespace aiedit::ui {

// A Markdown document drawn as it reads: headings, lists and task lists, tables, quotes, and code
// coloured by its language, in a column in the middle, in the colours of the scheme. What Qt
// reads of Markdown (CommonMark, with GitHub's tables, task lists and strikethrough), styled
// afterwards. Images come from disk only: nothing is fetched from the network.
class MarkdownView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit MarkdownView(QWidget* parent = nullptr);

    // `baseDirectory`: where relative links and images point. Shown again from where it was, so
    // that drawing the text anew as it is typed keeps the place.
    void setMarkdown(const QString& text, const QString& baseDirectory);
    QString markdown() const { return m_text; }

    // The headings as drawn, in order.
    int headingCount() const { return static_cast<int>(m_headings.size()); }
    // A place in the document by its headings: `fraction` of the way from heading `heading`
    // (-1: the start of the document) to the next one, or to the end.
    struct Place
    {
        int heading = -1;
        double fraction = 0;
    };
    // Where the top of the view is; bringing a place to the top.
    Place topPlace() const;
    void scrollToPlace(const Place& place);
    // Brings `fraction` of the document to the top (where there are no headings to go by).
    void scrollToFraction(double fraction);
    // Selects `text` where it shows nearest to `place`, and shows it; false, showing the place,
    // if it does not show as it is written (marked up, say).
    bool selectText(const QString& text, const Place& place);

    // Extra actions for the right-click menu (the AI actions), after Copy.
    void setContextActions(const QList<QAction*>& actions);

    // The document drawn as on paper, written out.
    static bool exportHtml(const QString& markdown, const QString& baseDirectory, const QFont& font,
                           const QString& path, QString* error = nullptr);
    static bool exportPdf(const QString& markdown, const QString& baseDirectory, const QFont& font,
                          const QString& path, QString* error = nullptr);

signals:
    // A link to a file on disk (another note, say), made absolute.
    void fileLinkActivated(const QString& path);
    // Scrolled by the user (wheel, keys, the scroll bar), not by the program.
    void scrolledByUser();
    // Ctrl+wheel; positive steps mean larger text. The owner zooms all its views alike.
    void zoomRequested(int steps);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    QVariant loadResource(int type, const QUrl& name) override;

private:
    void render();
    void placeColumn(QTextDocument* doc);
    // Lays the whole document out now and sizes the scroll bar for it: Qt lays long documents
    // out a piece at a time, and the scroll range grows as it goes.
    void layOutNow();
    // Where heading `index` starts (-1: the top; past the last one: the end).
    qreal headingTop(int index) const;
    qreal placeY(const Place& place) const;
    void followLink(const QUrl& url);

    QString m_text;
    QString m_baseDirectory;
    QList<int> m_headings; // their blocks' numbers
    QList<QPointer<QAction>> m_contextActions;
    bool m_restyling = false;
    int m_zoomRemainder = 0;
};

} // namespace aiedit::ui
