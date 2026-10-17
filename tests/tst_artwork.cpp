// The built-in artwork gallery: every style paints something, a song always
// gets the same picture, and the choice is kept in the settings.

#include "ArtworkGallery.h"
#include "PlaylistSession.h"

#include <QDir>
#include <QPainter>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

namespace {

// How many different colors a 32x32 thumbnail has: a blank or flat image has few.
int colorCount(const QImage &image)
{
    const QImage small = image.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QSet<QRgb> colors;
    for (int y = 0; y < small.height(); ++y)
        for (int x = 0; x < small.width(); ++x)
            colors.insert(small.pixel(x, y) & 0xFCFCFC);
    return int(colors.size());
}

} // namespace

class ArtworkTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void everyStylePaints();
    void sameSongSamePicture();
    void choiceIsKept();

private:
    QTemporaryDir m_config;
};

void ArtworkTest::initTestCase()
{
    QVERIFY(m_config.isValid());
    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
}

void ArtworkTest::everyStylePaints()
{
    const QList<ArtworkGallery::Style> styles = ArtworkGallery::styles();
    QVERIFY(styles.size() >= 16);
    const QString dir = qEnvironmentVariable("TOPPLAYER_SCREENSHOTS");
    QImage sheet;
    if (!dir.isEmpty()) {
        sheet = QImage(6 * 260, ((styles.size() + 5) / 6) * 260, QImage::Format_RGB32);
        sheet.fill(Qt::black);
    }
    for (int i = 0; i < styles.size(); ++i) {
        const QImage image = ArtworkGallery::render(styles[i].id, 256, 42);
        QCOMPARE(image.size(), QSize(256, 256));
        QVERIFY2(colorCount(image) > 8, qPrintable(styles[i].id));
        QVERIFY(!styles[i].name.isEmpty());
        if (!sheet.isNull())
            QPainter(&sheet).drawImage(QPoint((i % 6) * 260 + 2, (i / 6) * 260 + 2), image);
    }
    if (!sheet.isNull())
        sheet.save(dir + QStringLiteral("/artwork-gallery.png"));
}

void ArtworkTest::sameSongSamePicture()
{
    const QString song = QStringLiteral("/music/Some Artist - Song.flac");
    QCOMPARE(ArtworkGallery::styleFor(song), ArtworkGallery::styleFor(song));
    QCOMPARE(ArtworkGallery::seedFor(song), ArtworkGallery::seedFor(song));
    QVERIFY(ArtworkGallery::isStyle(ArtworkGallery::styleFor(song)));
    // Another seed, another picture of the same style.
    QVERIFY(ArtworkGallery::render(QStringLiteral("nebula"), 128, 1) != ArtworkGallery::render(QStringLiteral("nebula"), 128, 2));
    QCOMPARE(ArtworkGallery::render(QStringLiteral("nebula"), 128, 1), ArtworkGallery::render(QStringLiteral("nebula"), 128, 1));
    // Songs spread over the styles.
    QSet<QString> used;
    for (int i = 0; i < 200; ++i)
        used.insert(ArtworkGallery::styleFor(QStringLiteral("/music/track %1.mp3").arg(i)));
    QVERIFY(used.size() >= 12);
}

void ArtworkTest::choiceIsKept()
{
    QCOMPARE(ArtworkGallery::choice(), ArtworkGallery::autoChoice());
    const QString song = QStringLiteral("/music/a.mp3");
    QVERIFY(!ArtworkGallery::artworkFor(song, 64).isNull());
    ArtworkGallery::setChoice(QStringLiteral("vinyl"));
    QCOMPARE(ArtworkGallery::choice(), QStringLiteral("vinyl"));
    QCOMPARE(ArtworkGallery::artworkFor(song, 64), ArtworkGallery::render(QStringLiteral("vinyl"), 64, ArtworkGallery::seedFor(song)));
    ArtworkGallery::setChoice(ArtworkGallery::noneChoice());
    QVERIFY(ArtworkGallery::artworkFor(song, 64).isNull());
    // Unknown values read as Auto.
    ArtworkGallery::setChoice(QStringLiteral("no-such-style"));
    QCOMPARE(ArtworkGallery::choice(), ArtworkGallery::autoChoice());
}

QTEST_GUILESS_MAIN(ArtworkTest)
#include "tst_artwork.moc"
