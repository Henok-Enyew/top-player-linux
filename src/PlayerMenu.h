#pragma once

#include <QKeySequence>
#include <QList>
#include <QMenu>

#include <functional>

class MainWindow;
class MpvWidget;

// The right-click menu. Every playback item is bound to an mpv property or
// command; items with shortcuts are also registered on the main window so
// they work while the menu is closed.
class PlayerMenu : public QMenu
{
    Q_OBJECT

public:
    PlayerMenu(MpvWidget *mpv, MainWindow *window);

Q_SIGNALS:
    // Asks for an OSD message for changes the OSD cannot pick up from mpv by itself.
    void osdRequested(const QString &label, const QString &value);

private:
    struct Choice {
        QString text;
        QString value;
    };

    struct Toggle {
        QAction *action;
        QString property;
        QString offValue;
    };

    void buildVideoMenu();
    void buildAudioMenu();
    // Audio -> Visualizations, and the custom artwork items.
    void buildVisualizationMenu(QMenu *audio);
    void buildSubtitleMenu();
    void buildLyricsMenu();
    void buildPlaybackMenu();
    void buildToolsMenu();
    void buildWindowMenu();
    void buildHelpMenu();

    QAction *addItem(QMenu *menu, const QString &text, std::function<void()> handler,
                     const QKeySequence &shortcut = {});
    QAction *addCommand(QMenu *menu, const QString &text, const QStringList &command,
                        const QKeySequence &shortcut = {});
    // A checkable item that flips `property` between `onValue` and `offValue`.
    QAction *addToggle(QMenu *menu, const QString &text, const QString &property,
                       const QKeySequence &shortcut = {}, bool announce = true,
                       const QString &onValue = QStringLiteral("yes"),
                       const QString &offValue = QStringLiteral("no"));
    // A submenu of exclusive values for `property`; the current value is checked on open.
    QMenu *addChoices(QMenu *menu, const QString &title, const QString &property,
                      const QList<Choice> &choices);
    // A submenu listing tracks of `type` ("video", "audio", "sub") bound to `property`.
    QMenu *addTrackMenu(QMenu *menu, const QString &title, const QString &type, const QString &property);

    // Selects the next subtitle track for `property` ("sid" or "secondary-sid").
    void cycleTrack(const QString &title, const QString &property);
    // The track id selected in the other subtitle slot, or empty for non-subtitle properties.
    QString otherSubtitleSlot(const QString &property) const;
    void bindShortcut(QAction *action, const QKeySequence &shortcut);
    void syncState();

    MpvWidget *m_mpv;
    MainWindow *m_window;
    QList<Toggle> m_toggles;
    QAction *m_fullScreenAction = nullptr;
    QAction *m_onTopAction = nullptr;
    QAction *m_miniPlayerAction = nullptr;
    QAction *m_playlistAction = nullptr;
};
