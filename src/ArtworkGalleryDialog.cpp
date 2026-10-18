#include "ArtworkGalleryDialog.h"
#include "ArtworkGallery.h"
#include "AudioArtwork.h"
#include "AudioController.h"
#include "MediaFiles.h"
#include "Theme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kThumb = 132;
constexpr int kPreview = 300;
constexpr int kChoiceRole = Qt::UserRole;
// Seed of the thumbnails: the same pictures every time the dialog opens.
constexpr quint32 kThumbSeed = 2026;

// A rounded thumbnail of `image`, sized for the device.
QIcon roundedIcon(const QImage &image, qreal dpr)
{
    QPixmap pixmap(QSize(kThumb, kThumb) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, kThumb, kThumb), 10, 10);
    p.setClipPath(clip);
    p.drawImage(QRectF(0, 0, kThumb, kThumb), image);
    return QIcon(pixmap);
}

// Auto: four styles in a 2x2 mosaic.
QImage mosaic(int size)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter p(&image);
    const QStringList ids{QStringLiteral("synthwave"), QStringLiteral("vinyl"), QStringLiteral("nebula"),
                          QStringLiteral("papercut")};
    const int half = size / 2;
    for (int i = 0; i < ids.size(); ++i)
        p.drawImage(QPoint((i % 2) * half, (i / 2) * half), ArtworkGallery::render(ids[i], half, kThumbSeed + i));
    return image;
}

// None: the visualizer's bars on the theme's surface.
QImage visualizerImage(int size)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Theme::Raised);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    const int bars = 9;
    const qreal width = size * 0.7 / bars;
    for (int i = 0; i < bars; ++i) {
        const qreal height = size * (0.15 + 0.5 * std::abs(std::sin(i * 0.9 + 0.4)));
        QLinearGradient fill(0, size * 0.75 - height, 0, size * 0.75);
        fill.setColorAt(0, Theme::Accent2);
        fill.setColorAt(1, Theme::Accent);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(size * 0.15 + i * width + 2, size * 0.75 - height, width - 4, height), 3, 3);
    }
    return image;
}

} // namespace

ArtworkGalleryDialog::ArtworkGalleryDialog(AudioController *audio, QWidget *parent)
    : QDialog(parent)
    , m_audio(audio)
    , m_list(new QListWidget(this))
    , m_preview(new QLabel(this))
    , m_previewName(new QLabel(this))
    , m_ownImage(new QLabel(this))
    , m_removeOwn(new QPushButton(tr("Use the Gallery Instead"), this))
    , m_forSong(new QPushButton(tr("Use for This Song Only"), this))
{
    setObjectName(QStringLiteral("ArtworkGalleryDialog"));
    setWindowTitle(tr("Artwork Gallery"));

    auto *heading = new QLabel(tr("Artwork for songs without a cover"), this);
    heading->setObjectName(QStringLiteral("ArtworkGalleryHeading"));
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.5);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    auto *hint = new QLabel(tr("Original generative art, sharp at any size. Pick one for every song, "
                               "or a different one for each song (the same each time it plays)."),
                            this);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("ArtworkGalleryHint"));

    m_list->setObjectName(QStringLiteral("ArtworkGalleryList"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kThumb, kThumb));
    m_list->setGridSize(QSize(kThumb + 22, kThumb + 40));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWrapping(true);
    m_list->setUniformItemSizes(true);
    m_list->setWordWrap(true);
    m_list->setSpacing(4);
    m_list->setMinimumSize(5 * (kThumb + 22) + 30, 2 * (kThumb + 40) + 20);

    const qreal dpr = devicePixelRatioF();
    const auto addItem = [this, dpr](const QString &choice, const QString &name, const QImage &image) {
        auto *item = new QListWidgetItem(roundedIcon(image, dpr), name, m_list);
        item->setData(kChoiceRole, choice);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(name);
    };
    const int pixels = int(kThumb * dpr);
    addItem(ArtworkGallery::autoChoice(), tr("Mix: One per Song"), mosaic(pixels));
    for (const ArtworkGallery::Style &style : ArtworkGallery::styles())
        addItem(style.id, style.name, ArtworkGallery::render(style.id, pixels, kThumbSeed));
    addItem(ArtworkGallery::noneChoice(), tr("None (Visualizer)"), visualizerImage(pixels));

    // The preview, for the song playing (or a sample) in the selected style.
    m_preview->setObjectName(QStringLiteral("ArtworkGalleryPreview"));
    m_preview->setFixedSize(kPreview, kPreview);
    m_preview->setAlignment(Qt::AlignCenter);
    m_previewName->setObjectName(QStringLiteral("ArtworkGalleryPreviewName"));
    m_previewName->setAlignment(Qt::AlignHCenter);
    m_previewName->setWordWrap(true);
    m_forSong->setObjectName(QStringLiteral("ArtworkGalleryForSongButton"));
    m_forSong->setToolTip(tr("Keep this picture for the playing song, whatever the gallery setting"));
    auto *ownButton = new QPushButton(tr("Your Own Image..."), this);
    ownButton->setObjectName(QStringLiteral("ArtworkGalleryOwnButton"));
    ownButton->setToolTip(tr("Use an image file for every song without a cover"));
    m_ownImage->setObjectName(QStringLiteral("ArtworkGalleryOwnImage"));
    m_ownImage->setWordWrap(true);
    m_removeOwn->setObjectName(QStringLiteral("ArtworkGalleryRemoveOwnButton"));

    auto *side = new QVBoxLayout;
    side->addWidget(m_preview, 0, Qt::AlignHCenter);
    side->addWidget(m_previewName);
    side->addSpacing(8);
    side->addWidget(m_forSong);
    side->addWidget(ownButton);
    side->addWidget(m_ownImage);
    side->addWidget(m_removeOwn);
    side->addStretch();

    auto *body = new QHBoxLayout;
    body->addWidget(m_list, 1);
    body->addSpacing(12);
    body->addLayout(side);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 16);
    layout->setSpacing(10);
    layout->addWidget(heading);
    layout->addWidget(hint);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    // The current choice starts selected.
    const QString current = ArtworkGallery::choice();
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(kChoiceRole).toString() == current)
            m_list->setCurrentRow(row);
    }
    connect(m_list, &QListWidget::itemClicked, this, &ArtworkGalleryDialog::choose);
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            choose(item);
    });
    connect(m_forSong, &QPushButton::clicked, this, [this] {
        const QString choice = selectedChoice();
        const QString style = choice == ArtworkGallery::autoChoice() ? ArtworkGallery::styleFor(m_audio->currentPath()) : choice;
        m_audio->setGalleryArtworkForSong(style);
    });
    connect(ownButton, &QPushButton::clicked, this, [this] {
        m_audio->setDefaultArtworkDialog();
        updateOwnImage();
    });
    connect(m_removeOwn, &QPushButton::clicked, this, [this] {
        m_audio->clearDefaultArtwork();
        updateOwnImage();
    });
    updateOwnImage();
    updatePreview();
}

QString ArtworkGalleryDialog::selectedChoice() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(kChoiceRole).toString() : ArtworkGallery::choice();
}

void ArtworkGalleryDialog::choose(QListWidgetItem *item)
{
    const QString choice = item->data(kChoiceRole).toString();
    if (choice != ArtworkGallery::choice())
        m_audio->setGalleryChoice(choice);
    updatePreview();
}

void ArtworkGalleryDialog::updatePreview()
{
    const QString choice = selectedChoice();
    const QString song = m_audio->currentPath();
    const QString key = song.isEmpty() ? QStringLiteral("sample") : song;
    const qreal dpr = devicePixelRatioF();
    const int pixels = int(kPreview * dpr);
    QImage image;
    QString name;
    if (choice == ArtworkGallery::noneChoice()) {
        image = visualizerImage(pixels);
        name = tr("Songs without a cover show the visualizer.");
    } else {
        const QString style = choice == ArtworkGallery::autoChoice() ? ArtworkGallery::styleFor(key) : choice;
        image = ArtworkGallery::render(style, pixels, ArtworkGallery::seedFor(key));
        for (const ArtworkGallery::Style &entry : ArtworkGallery::styles()) {
            if (entry.id == style)
                name = entry.name;
        }
        if (choice == ArtworkGallery::autoChoice())
            name = song.isEmpty() ? tr("Every song gets its own, like this %1.").arg(name)
                                  : tr("This song gets %1.").arg(name);
    }
    QPixmap pixmap(QSize(kPreview, kPreview) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, kPreview, kPreview), 14, 14);
        p.setClipPath(clip);
        p.drawImage(QRectF(0, 0, kPreview, kPreview), image);
    }
    m_preview->setPixmap(pixmap);
    m_previewName->setText(name);
    m_forSong->setEnabled(choice != ArtworkGallery::noneChoice() && !MediaFiles::localPath(song).isEmpty());
}

void ArtworkGalleryDialog::updateOwnImage()
{
    const bool own = !AudioArtwork::defaultArtwork().isEmpty();
    m_ownImage->setVisible(own);
    m_removeOwn->setVisible(own);
    m_ownImage->setText(tr("Your own default image is in use: it shows instead of the gallery."));
}
