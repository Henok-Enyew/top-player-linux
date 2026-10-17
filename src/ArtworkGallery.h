#pragma once

#include <QImage>
#include <QList>
#include <QString>

// Built-in artwork for songs without a cover: generative pieces painted in
// code (aurora, synthwave sunset, vinyl, nebula, ...), so they are sharp at
// any size and cost nothing to ship. Every style varies with a seed: a song
// gets its own picture, the same each time it plays.
namespace ArtworkGallery {

struct Style {
    QString id;   // stored in the settings, e.g. "aurora"
    QString name; // shown, e.g. "Aurora"
};

QList<Style> styles();
bool isStyle(const QString &id);

// The style a song gets in Auto mode, and its seed: both follow from its path.
QString styleFor(const QString &trackPath);
quint32 seedFor(const QString &trackPath);

// Paints style `id` (falling back to the first) as a square image of `size` pixels.
QImage render(const QString &id, int size, quint32 seed = 0);

// What songs without a cover show: Auto (a style per song), a style's id,
// or None (the visualizer). Kept in settings.ini as [audio] builtinArtwork.
QString autoChoice();
QString noneChoice();
QString choice();
void setChoice(const QString &choice);
// The artwork the choice gives `trackPath`, or a null image for None.
QImage artworkFor(const QString &trackPath, int size = 1024);

} // namespace ArtworkGallery
