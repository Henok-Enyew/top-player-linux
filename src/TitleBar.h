#pragma once

#include <QFrame>

class QLabel;
class QToolButton;

// Slim skin title bar for the frameless window: logo, title and the window
// buttons (mini player, pin on top, minimize, maximize, fullscreen, close), as in
// PotPlayer's corner. Dragging it moves the window (the press falls through
// to MainWindow).
class TitleBar : public QFrame
{
    Q_OBJECT

public:
    explicit TitleBar(QWidget *window);

    void setTitle(const QString &title);
    // Mirrors "Always on Top" on the pin button.
    void setPinned(bool pinned);

Q_SIGNALS:
    void pinToggled(bool pinned);
    void fullScreenRequested();
    void miniPlayerRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateMaximizeButton();
    void updateElidedTitle();

    QWidget *m_window;
    QLabel *m_title;
    QString m_fullTitle;
    QToolButton *m_pinButton;
    QToolButton *m_maximizeButton;
};
