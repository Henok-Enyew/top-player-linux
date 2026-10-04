#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QPushButton;

// A small card over the video asking whether to continue a file from where
// it was left or to start it over. Resumes by itself after a countdown.
// Enter/R resume, Escape/S start over.
class ResumePrompt : public QWidget
{
    Q_OBJECT

public:
    explicit ResumePrompt(QWidget *parent);

    // Shows the card for `title`, last left at `position` of `duration`
    // seconds; `audio` words it for a song instead of a video.
    void ask(const QString &title, double position, double duration, bool audio = false);
    void dismiss();
    int secondsLeft() const { return m_secondsLeft; }
    QPushButton *resumeButton() const { return m_resume; }
    QPushButton *startOverButton() const { return m_startOver; }

    static constexpr int kCountdownSeconds = 10;

Q_SIGNALS:
    void resumeChosen();
    void startOverChosen();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void place();
    void updateCountdown();

    QLabel *m_heading;
    QLabel *m_title;
    QLabel *m_detail;
    QLabel *m_countdown;
    QPushButton *m_resume;
    QPushButton *m_startOver;
    QTimer m_tick;
    int m_secondsLeft = 0;
    double m_progress = 0;
};
