#pragma once

#include <QDialog>

class AudioController;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// Audio -> Artwork Gallery...: the built-in artwork shown for songs without
// a cover. A style for every song, one style for all, or none (the
// visualizer); a click applies it at once. A style can also be given to the
// current song alone, and the user's own default image still beats them all.
class ArtworkGalleryDialog : public QDialog
{
    Q_OBJECT

public:
    ArtworkGalleryDialog(AudioController *audio, QWidget *parent = nullptr);

private:
    void choose(QListWidgetItem *item);
    void updatePreview();
    void updateOwnImage();
    QString selectedChoice() const;

    AudioController *m_audio;
    QListWidget *m_list;
    QLabel *m_preview;
    QLabel *m_previewName;
    QLabel *m_ownImage;
    QPushButton *m_removeOwn;
    QPushButton *m_forSong;
};
