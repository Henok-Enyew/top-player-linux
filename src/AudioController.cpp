#include "AudioController.h"
#include "ArtworkExtractor.h"
#include "AudioView.h"
#include "MediaFiles.h"
#include "MpvWidget.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>

#include <utility>

namespace {

// mpv's own audio visualization filters, in the skin's cyan.
const QString kWaveformFilter = QStringLiteral(
    "showwaves=s=1280x720:r=30:mode=cline:scale=sqrt:draw=full:colors=0x00D2FF,format=yuv420p");
const QString kSpectrumFilter = QStringLiteral(
    "showfreqs=s=1280x720:rate=30:mode=bar:ascale=log:fscale=log:win_size=1024:averaging=4:colors=0x00D2FF,"
    "format=yuv420p");

QImage readImage(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}

} // namespace

AudioController::AudioController(MpvWidget *mpv, QWidget *dialogParent)
    : QObject(dialogParent)
    , m_mpv(mpv)
    , m_dialogParent(dialogParent)
    , m_view(new AudioView(mpv))
    , m_extractor(new ArtworkExtractor(this))
    , m_visualization(AudioArtwork::visualization())
{
    connect(m_mpv, &MpvWidget::fileLoaded, this, &AudioController::onFileLoaded);
    connect(m_extractor, &ArtworkExtractor::artworkReady, this, &AudioController::onArtworkReady);
    connect(m_mpv, &MpvWidget::propertyUpdated, this, [this](const QString &name, const QVariant &value) {
        if (name == QLatin1String("metadata") && m_active) {
            updateTrackInfo();
        } else if (name == QLatin1String("idle-active") && value.toBool() && m_mpv->isIdle()) {
            // Stopped, or the playlist ran out: mpv dropped the file and its graph.
            m_active = false;
            m_graphFilter.clear();
            apply();
        }
    });
}

void AudioController::onFileLoaded()
{
    // mpv resets the file-local graph and video track for every new file.
    m_graphFilter.clear();
    m_active = m_mpv->isAudioOnly();
    m_artwork = {};
    m_source = ArtworkSource::None;
    m_extracting = false;
    m_customWins = false;
    if (!m_active) {
        apply();
        return;
    }

    m_path = m_mpv->mpvPropertyString(QStringLiteral("path"));
    const QString id = m_mpv->mpvPropertyString(QStringLiteral("current-tracks/audio/id"));
    m_audioId = id.isEmpty() ? QStringLiteral("no") : id;
    // The cover is drawn by AudioView; mpv need not decode it as video.
    m_mpv->setMpvProperty(QStringLiteral("file-local-options/vid"), QStringLiteral("no"));
    updateTrackInfo();

    const QString local = MediaFiles::localPath(m_path);
    const QString custom = local.isEmpty() ? QString() : AudioArtwork::customArtwork(local);
    if (const QImage image = custom.isEmpty() ? QImage() : readImage(custom); !image.isNull()) {
        m_artwork = image;
        m_source = ArtworkSource::Custom;
        m_customWins = true;
    } else if (!local.isEmpty()) {
        m_extracting = true;
        m_extractor->request(m_path);
    } else {
        // Streams and radio have no cover to look for.
        useDefaultArtwork();
    }
    apply();
}

void AudioController::useDefaultArtwork()
{
    if (!m_artwork.isNull())
        return;
    const QString image = AudioArtwork::defaultArtwork();
    m_artwork = image.isEmpty() ? QImage() : readImage(image);
    m_source = m_artwork.isNull() ? ArtworkSource::None : ArtworkSource::Default;
}

void AudioController::onArtworkReady(const QString &path, const QImage &image)
{
    if (!m_active || !m_extracting || path != m_path)
        return;
    m_extracting = false;
    if (!image.isNull()) {
        m_artwork = image;
        m_source = ArtworkSource::Embedded;
    } else if (const QString cover = AudioArtwork::folderCover(MediaFiles::localPath(m_path)); !cover.isEmpty()) {
        m_artwork = readImage(cover);
        m_source = m_artwork.isNull() ? ArtworkSource::None : ArtworkSource::Folder;
    }
    useDefaultArtwork();
    apply();
}

void AudioController::updateTrackInfo()
{
    const QString fallback = m_mpv->mpvPropertyString(QStringLiteral("media-title"));
    m_view->setTrackInfo(AudioArtwork::trackInfo(m_mpv->mpvProperty(QStringLiteral("metadata")).toMap(), fallback));
}

void AudioController::apply()
{
    using AudioArtwork::Visualization;
    Display display = Display::None;
    if (m_active) {
        switch (m_visualization) {
        case Visualization::AlbumArt:
            // Until the extractor answers, show a placeholder rather than
            // starting a visualization that may be gone a moment later.
            display = !m_artwork.isNull() || m_extracting ? Display::Artwork : Display::Spectrum;
            break;
        case Visualization::Waveform:
            display = Display::Waveform;
            break;
        case Visualization::Spectrum:
            display = Display::Spectrum;
            break;
        case Visualization::Off:
            // No visualizer: the song's own cover if it has one.
            display = !m_artwork.isNull() || m_extracting ? Display::Artwork : Display::Canvas;
            break;
        }
    }
    // The user's own cover for this track beats the visualization setting:
    // otherwise setting one while a visualizer runs would seem to do nothing.
    if (m_active && m_customWins && m_source == ArtworkSource::Custom && !m_artwork.isNull())
        display = Display::Artwork;
    m_display = display;
    Q_EMIT artworkChanged(m_artwork);

    QString filter;
    if (display == Display::Waveform)
        filter = kWaveformFilter;
    else if (display == Display::Spectrum)
        filter = kSpectrumFilter;
    // Without an audio track there is nothing to visualize.
    if (m_audioId == QLatin1String("no"))
        filter.clear();
    if (m_active)
        setGraph(filter);

    switch (display) {
    case Display::None:
        m_view->hide();
        return;
    case Display::Artwork:
        m_view->setMode(AudioView::Mode::Artwork);
        break;
    case Display::Canvas:
        m_view->setMode(AudioView::Mode::Canvas);
        break;
    case Display::Waveform:
    case Display::Spectrum:
        m_view->setMode(filter.isEmpty() ? AudioView::Mode::Canvas : AudioView::Mode::Visualizer);
        break;
    }
    m_view->setArtwork(m_artwork);
    m_view->show();
}

void AudioController::setGraph(const QString &filter)
{
    if (filter == m_graphFilter)
        return;
    const bool hadGraph = !m_graphFilter.isEmpty();
    m_graphFilter = filter;
    if (!filter.isEmpty()) {
        // The graph takes the place of the audio track selection: it decodes
        // the track, passes it on to the speakers and draws it as video.
        m_mpv->setMpvProperty(QStringLiteral("file-local-options/lavfi-complex"),
                              QStringLiteral("[aid%1]asplit[ao][v];[v]%2[vo]").arg(m_audioId, filter));
        return;
    }
    if (hadGraph) {
        // Leaving the graph deselects every track; select the audio again.
        m_mpv->setMpvProperty(QStringLiteral("file-local-options/lavfi-complex"), QString());
        m_mpv->setMpvProperty(QStringLiteral("file-local-options/aid"), m_audioId);
    }
}

void AudioController::setVisualization(AudioArtwork::Visualization mode)
{
    m_visualization = mode;
    m_customWins = false;
    AudioArtwork::setVisualization(mode);
    apply();
}

QString AudioController::audioTrack() const
{
    return m_graphFilter.isEmpty() ? m_mpv->mpvPropertyString(QStringLiteral("aid")) : m_audioId;
}

void AudioController::selectAudioTrack(const QString &id)
{
    if (m_graphFilter.isEmpty()) {
        m_mpv->setMpvProperty(QStringLiteral("aid"), id);
        if (m_active) {
            m_audioId = id;
            // Turning audio back on may make room for a visualization again.
            apply();
        }
        return;
    }
    m_audioId = id;
    if (id == QLatin1String("no"))
        setGraph({}); // drops the graph and leaves the audio off
    else
        m_mpv->setMpvProperty(QStringLiteral("file-local-options/lavfi-complex"),
                              QStringLiteral("[aid%1]asplit[ao][v];[v]%2[vo]").arg(id, m_graphFilter));
    apply();
}

void AudioController::setCustomArtworkDialog()
{
    if (!m_active || MediaFiles::localPath(m_path).isEmpty()) {
        Q_EMIT message(tr("Custom artwork needs a local audio file"));
        return;
    }
    const QString file = QFileDialog::getOpenFileName(m_dialogParent, tr("Set Custom Audio Artwork"),
                                                      QFileInfo(MediaFiles::localPath(m_path)).absolutePath(),
                                                      AudioArtwork::imageFileFilter());
    if (!file.isEmpty() && !setCustomArtwork(file))
        Q_EMIT message(tr("Not an image"), QFileInfo(file).fileName());
}

bool AudioController::setCustomArtwork(const QString &imagePath)
{
    const QString local = MediaFiles::localPath(m_path);
    if (!m_active || local.isEmpty())
        return false;
    const QImage image = readImage(imagePath);
    if (image.isNull())
        return false;
    AudioArtwork::setCustomArtwork(local, imagePath);
    m_artwork = image;
    m_source = ArtworkSource::Custom;
    m_extracting = false;
    m_customWins = true;
    apply();
    Q_EMIT message(tr("Artwork Set"), QFileInfo(imagePath).fileName());
    return true;
}

void AudioController::setDefaultArtworkDialog()
{
    const QString start = AudioArtwork::defaultArtwork().isEmpty()
                              ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                              : QString();
    const QString file = QFileDialog::getOpenFileName(m_dialogParent, tr("Default Artwork for Songs Without a Cover"),
                                                      start, AudioArtwork::imageFileFilter());
    if (!file.isEmpty() && !setDefaultArtwork(file))
        Q_EMIT message(tr("Not an image"), QFileInfo(file).fileName());
}

bool AudioController::setDefaultArtwork(const QString &imagePath)
{
    if (readImage(imagePath).isNull() || !AudioArtwork::setDefaultArtwork(imagePath))
        return false;
    // The playing song shows it at once if it has no cover of its own.
    if (m_active && !m_extracting && (m_source == ArtworkSource::None || m_source == ArtworkSource::Default)) {
        m_artwork = {};
        useDefaultArtwork();
        apply();
    }
    Q_EMIT message(tr("Default Artwork Set"), QFileInfo(imagePath).fileName());
    return true;
}

void AudioController::clearDefaultArtwork()
{
    AudioArtwork::clearDefaultArtwork();
    if (m_active && m_source == ArtworkSource::Default) {
        m_artwork = {};
        m_source = ArtworkSource::None;
        apply();
    }
    Q_EMIT message(tr("Default Artwork Cleared"));
}

void AudioController::clearCustomArtwork()
{
    const QString local = MediaFiles::localPath(m_path);
    if (!m_active || local.isEmpty() || m_source != ArtworkSource::Custom)
        return;
    AudioArtwork::clearCustomArtwork(local);
    // Look for the embedded or folder cover again.
    m_artwork = {};
    m_source = ArtworkSource::None;
    m_extracting = true;
    m_extractor->request(m_path);
    apply();
}
