#pragma once

#include <QList>
#include <QUrl>
#include <QWidget>

enum class IconType;
class QGraphicsOpacityEffect;
class QGridLayout;
class QLabel;
class QPropertyAnimation;
class QPushButton;

// Start screen shown over the video while nothing is loaded: the logo, open
// actions and a drop zone. It covers its parent and follows its size.
class EmptyStateWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EmptyStateWidget(QWidget *parent);

    bool isActive() const { return m_active; }
    // Fades the screen in or out; without `animate` it switches at once.
    void setActive(bool active, bool animate = true);

Q_SIGNALS:
    void openFileRequested();
    void openFolderRequested();
    void openUrlRequested();
    void openPlaylistRequested();
    void searchYouTubeRequested();
    void searchSpotifyRequested();
    void urlsDropped(const QList<QUrl> &urls);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QPushButton *addButton(const QString &objectName, const QString &text, IconType icon);
    // Picks the largest arrangement of the content that fits the current size.
    void fitContent();
    void setColumns(int columns);
    // Toggles the "compact" style property: a smaller title, icon-only buttons.
    void setCompact(QWidget *widget, bool compact);
    void setDropHighlight(bool highlight);

    QWidget *m_content;
    QLabel *m_logo;
    QLabel *m_title;
    QLabel *m_hint;
    QGridLayout *m_buttonGrid;
    QList<QPushButton *> m_buttons;
    QGraphicsOpacityEffect *m_opacity;
    QPropertyAnimation *m_fade;
    int m_columns = 0;
    bool m_active = true;
    bool m_dropHighlight = false;
};
