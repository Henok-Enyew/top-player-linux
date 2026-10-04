#pragma once

#include <QObject>
#include <QString>

#include <optional>

class MpvWidget;
class ResumePrompt;
class QWidget;

// Remembers where each file was left (~/.config/top-player/resume.ini) and,
// when one of them is opened again, asks whether to resume or start over.
class ResumeManager : public QObject
{
    Q_OBJECT

public:
    // What happens when a file with a saved position opens.
    enum class Mode { Ask, Always, Never };

    struct Entry {
        double position = 0;
        double duration = 0;
    };

    ResumeManager(MpvWidget *mpv, QWidget *overlayParent);

    static Mode mode();
    static void setMode(Mode mode);

    // The saved position of `media` (a path or URL), if any.
    static std::optional<Entry> lookup(const QString &media);
    // Saves the position, or forgets it when it is too close to either end
    // to be worth resuming.
    static void remember(const QString &media, double position, double duration);
    static void forget(const QString &media);
    // Positions closer than this to the start or the end are not kept.
    static constexpr double kMinMargin = 10.0;

    ResumePrompt *prompt() const { return m_prompt; }
    // Saves the playing file's position now (on quit).
    void saveNow();

Q_SIGNALS:
    void message(const QString &label, const QString &value = QString());

private:
    void onFileStarted();
    void onFileLoaded();
    void resume();
    void startOver();
    QString currentEntry() const;

    MpvWidget *m_mpv;
    ResumePrompt *m_prompt;
    // The file that is playing, its last known position and length.
    QString m_media;
    double m_position = -1;
    double m_duration = 0;
    double m_lastSaved = -1;
    // Set between fileStarted() and fileLoaded() for a file with a saved position.
    std::optional<Entry> m_pending;
    QString m_pendingMedia;
    bool m_wasPaused = false;
    // While the prompt is open, positions are not recorded.
    bool m_asking = false;
};
