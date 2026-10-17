// The appearance engine: the skin template filled in for every theme and
// style, live switching of colors, palette, skin and icons, the saved
// appearance, and the Appearance dialog and menu. Needs a display (run under
// xvfb-run).
//
// Set TOPPLAYER_SCREENSHOTS=<dir> to save the window and the dialog in each theme.

#include "AppearanceDialog.h"
#include "Icons.h"
#include "MainWindow.h"
#include "PlaylistSession.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <clocale>
#include <cmath>

namespace {

QStringList g_warnings;
QtMessageHandler g_previousHandler = nullptr;

void captureWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg)
        g_warnings.append(message);
    g_previousHandler(type, context, message);
}

qreal luminance(const QColor &color)
{
    auto channel = [](float v) { return v <= 0.03928f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

qreal contrast(const QColor &a, const QColor &b)
{
    const qreal la = luminance(a);
    const qreal lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

bool hasColor(const QImage &image, const QColor &color)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == color)
                return true;
        }
    }
    return false;
}

} // namespace

class ThemeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultColorsMatchTheEngine();
    void templateIsFilledIn_data();
    void templateIsFilledIn();
    void skinParsesForEveryAppearance();
    void accentsStayReadable();
    void switchingUpdatesTheApp();
    void unknownIdsFallBack();
    void iconsFollowTheAccent();
    void savedAppearanceRoundTrip();
    void crossFadeEndsByItself();
    void appearanceDialog();
    void appearanceMenu();
    void screenshots();
    void cleanupTestCase();
};

void ThemeTest::defaultColorsMatchTheEngine()
{
    // Before anything applies an appearance, the live colors hold the default one.
    const Theme::Colors c = Theme::colors(Theme::Appearance());
    QCOMPARE(Theme::Accent, c.accent);
    QCOMPARE(Theme::AccentHover, c.accentHover);
    QCOMPARE(Theme::AccentDeep, c.accentDeep);
    QCOMPARE(Theme::Accent2, c.accent2);
    QCOMPARE(Theme::AccentTint, c.accentTint);
    QCOMPARE(Theme::OnAccent, c.onAccent);
    QCOMPARE(Theme::Surface, c.surface);
    QCOMPARE(Theme::Panel, c.panel);
    QCOMPARE(Theme::Raised, c.raised);
    QCOMPARE(Theme::Border, c.border);
    QCOMPARE(Theme::TextPrimary, c.text);
    QCOMPARE(Theme::TextSecondary, c.textSecondary);
    QCOMPARE(Theme::TextDim, c.textDim);
    // The brand.
    QCOMPARE(Theme::Accent, Theme::BrandViolet);
    QCOMPARE(Theme::Accent2, Theme::BrandMagenta);
}

void ThemeTest::templateIsFilledIn_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QString>("style");
    for (const Theme::Scheme &scheme : Theme::schemes()) {
        for (const Theme::Style &style : Theme::styles())
            QTest::newRow(qPrintable(scheme.id + QLatin1Char('/') + style.id)) << scheme.id << style.id;
    }
}

void ThemeTest::templateIsFilledIn()
{
    QFETCH(QString, theme);
    QFETCH(QString, style);
    QVERIFY(Theme::styleSheetTemplate().contains(QLatin1String("@accent@")));
    for (const Theme::AccentPalette &accent : Theme::accents()) {
        g_warnings.clear();
        const QString qss = Theme::styleSheet({theme, accent.id, style});
        QVERIFY2(!qss.contains(QLatin1Char('@')), qPrintable(accent.id + QStringLiteral(": unfilled token")));
        QVERIFY2(g_warnings.isEmpty(), qPrintable(g_warnings.join(QLatin1Char('\n'))));
        // Every objectName the code relies on is still styled.
        for (const char *selector : {"#ControlBar", "#PlaylistDrawer", "#TitleBar", "SeekBar", "#PlayButton",
                                     "#PlaylistView", "#LibraryView", "#AboutDialog", "#AppearanceDialog"})
            QVERIFY2(qss.contains(QLatin1String(selector)), selector);
        QVERIFY(qss.contains(Theme::hex(Theme::colors({theme, accent.id, style}).accent)));
    }
}

void ThemeTest::skinParsesForEveryAppearance()
{
    // Qt warns when it cannot parse a stylesheet, once widgets are polished with it.
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *checkable = new QPushButton(QStringLiteral("Checked"));
    checkable->setCheckable(true);
    checkable->setChecked(true);
    layout->addWidget(checkable);
    layout->addWidget(new QLineEdit);
    layout->addWidget(new QComboBox);
    layout->addWidget(new QCheckBox(QStringLiteral("Check")));
    layout->addWidget(new QSlider(Qt::Horizontal));
    layout->addWidget(new QTreeWidget);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const Theme::Scheme &scheme : Theme::schemes()) {
        for (const Theme::Style &style : Theme::styles()) {
            g_warnings.clear();
            Theme::setAppearance({scheme.id, QStringLiteral("native"), style.id});
            window.repaint();
            QVERIFY2(g_warnings.isEmpty(), qPrintable(scheme.id + QLatin1Char('/') + style.id + QStringLiteral(": ")
                                                      + g_warnings.join(QLatin1Char('\n'))));
        }
    }
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::accentsStayReadable()
{
    for (const Theme::Scheme &scheme : Theme::schemes()) {
        for (const Theme::AccentPalette &accent : Theme::accents()) {
            const Theme::Colors c = Theme::colors({scheme.id, accent.id, QStringLiteral("glass")});
            QVERIFY2(contrast(c.accent, c.surface) >= 2.9, qPrintable(scheme.id + QLatin1Char('/') + accent.id));
            QVERIFY2(contrast(c.onAccent, c.accent) >= 3.0, qPrintable(scheme.id + QLatin1Char('/') + accent.id));
            QVERIFY2(contrast(c.text, c.surface) >= 4.5, qPrintable(scheme.id));
        }
    }
}

void ThemeTest::switchingUpdatesTheApp()
{
    QSignalSpy changed(Theme::notifier(), &Theme::Notifier::changed);
    const Theme::Appearance nord{QStringLiteral("nord"), QStringLiteral("cyan"), QStringLiteral("tiling")};
    Theme::setAppearance(nord);
    QCOMPARE(changed.count(), 1);
    QVERIFY(Theme::appearance() == nord);
    const Theme::Colors c = Theme::colors(nord);
    QCOMPARE(Theme::Accent, c.accent);
    QCOMPARE(Theme::Surface, QColor(0x24, 0x29, 0x33));
    QCOMPARE(QApplication::palette().color(QPalette::Window), c.surface);
    QCOMPARE(QApplication::palette().color(QPalette::Highlight), c.accent);
    QCOMPARE(qApp->styleSheet(), Theme::styleSheet(nord));
    QVERIFY(qApp->styleSheet().contains(Theme::hex(c.accent)));

    // The same appearance again changes nothing.
    Theme::setAppearance(nord);
    QCOMPARE(changed.count(), 1);

    // bind() refreshes now and on every change.
    QObject context;
    QStringList seen;
    Theme::bind(&context, [&seen] { seen.append(Theme::hex(Theme::Accent)); });
    Theme::setAppearance({QStringLiteral("catppuccin-latte"), QStringLiteral("violet"), QStringLiteral("soft")});
    QCOMPARE(seen.size(), 2);
    QCOMPARE(seen.last(), Theme::hex(Theme::Accent));
    QVERIFY(Theme::isLight());
    QVERIFY(luminance(Theme::Surface) > luminance(Theme::TextPrimary));

    Theme::setAppearance(Theme::Appearance());
    QCOMPARE(Theme::Accent, Theme::BrandViolet);
    QVERIFY(!Theme::isLight());
}

void ThemeTest::unknownIdsFallBack()
{
    Theme::setAppearance({QStringLiteral("no-such-theme"), QStringLiteral("gold"), QStringLiteral("nope")});
    QCOMPARE(Theme::appearance().theme, Theme::Appearance().theme);
    QCOMPARE(Theme::appearance().accent, QStringLiteral("gold"));
    QCOMPARE(Theme::appearance().style, Theme::Appearance().style);
    QVERIFY(!Theme::styleSheet({QStringLiteral("?"), QStringLiteral("?"), QStringLiteral("?")}).contains(QLatin1Char('@')));
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::iconsFollowTheAccent()
{
    // An icon made before a change draws in the new colors after it.
    const QIcon icon = skinIcon(IconType::Shuffle);
    auto on = [&icon] { return icon.pixmap(QSize(20, 20), QIcon::Normal, QIcon::On).toImage(); };
    QVERIFY(hasColor(on(), Theme::Accent));
    Theme::setAppearance({QStringLiteral("gruvbox"), QStringLiteral("mint"), QStringLiteral("glass")});
    QVERIFY(hasColor(on(), Theme::Accent));
    QVERIFY(!hasColor(on(), Theme::BrandViolet));
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::savedAppearanceRoundTrip()
{
    QCOMPARE(Theme::savedAppearance(), Theme::Appearance());
    const Theme::Appearance saved{QStringLiteral("rose-pine"), QStringLiteral("pink"), QStringLiteral("soft")};
    Theme::saveAppearance(saved);
    QVERIFY(Theme::savedAppearance() == saved);
    const QSettings settings(PlaylistSession::configDir() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("appearance/theme")).toString(), saved.theme);
    QCOMPARE(settings.value(QStringLiteral("appearance/accent")).toString(), saved.accent);
    QCOMPARE(settings.value(QStringLiteral("appearance/style")).toString(), saved.style);
    // Startup applies it.
    applyDarkSkin(*qApp);
    QVERIFY(Theme::appearance() == saved);
    QCOMPARE(QApplication::palette().color(QPalette::Window), Theme::colors(saved).surface);
    Theme::saveAppearance(Theme::Appearance());
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::crossFadeEndsByItself()
{
    QWidget window;
    window.resize(320, 200);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const int children = window.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly).size();
    Theme::setAppearance({QStringLiteral("kanagawa"), QStringLiteral("violet"), QStringLiteral("glass")}, true);
    // A snapshot of the old look fades out over the window, then goes.
    QCOMPARE(window.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly).size(), children + 1);
    QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly).size(), children, 2000);
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::appearanceDialog()
{
    AppearanceDialog dialog;
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QCOMPARE(dialog.findChildren<QAbstractButton *>(QRegularExpression(QStringLiteral("^Theme-"))).size(),
             Theme::schemes().size());
    QCOMPARE(dialog.findChildren<QAbstractButton *>(QRegularExpression(QStringLiteral("^Accent-"))).size(),
             Theme::accents().size());
    QCOMPARE(dialog.findChildren<QAbstractButton *>(QRegularExpression(QStringLiteral("^Style-"))).size(),
             Theme::styles().size());
    QVERIFY(dialog.findChild<QAbstractButton *>(QStringLiteral("Theme-midnight"))->isChecked());

    // A click previews at once; Cancel goes back.
    QTest::mouseClick(dialog.findChild<QAbstractButton *>(QStringLiteral("Theme-everforest")), Qt::LeftButton);
    QCOMPARE(Theme::appearance().theme, QStringLiteral("everforest"));
    QVERIFY(dialog.findChild<QAbstractButton *>(QStringLiteral("Theme-everforest"))->isChecked());
    QTest::mouseClick(dialog.findChild<QAbstractButton *>(QStringLiteral("Accent-sunset")), Qt::LeftButton);
    QCOMPARE(Theme::Accent, Theme::colors(Theme::appearance()).accent);
    QTest::mouseClick(dialog.findChild<QPushButton *>(QStringLiteral("AppearanceCancel")), Qt::LeftButton);
    QVERIFY(Theme::appearance() == Theme::Appearance());
    QVERIFY(!dialog.isVisible());

    // Done keeps the choice.
    dialog.show();
    QTest::mouseClick(dialog.findChild<QAbstractButton *>(QStringLiteral("Style-tiling")), Qt::LeftButton);
    QTest::mouseClick(dialog.findChild<QAbstractButton *>(QStringLiteral("Theme-matte-black")), Qt::LeftButton);
    QTest::mouseClick(dialog.findChild<QPushButton *>(QStringLiteral("AppearanceDone")), Qt::LeftButton);
    const Theme::Appearance kept{QStringLiteral("matte-black"), QStringLiteral("violet"), QStringLiteral("tiling")};
    QVERIFY(Theme::appearance() == kept);
    QVERIFY(Theme::savedAppearance() == kept);

    // Reset to Default.
    dialog.show();
    QTest::mouseClick(dialog.findChild<QPushButton *>(QStringLiteral("AppearanceReset")), Qt::LeftButton);
    QVERIFY(Theme::appearance() == Theme::Appearance());
    QTest::mouseClick(dialog.findChild<QPushButton *>(QStringLiteral("AppearanceDone")), Qt::LeftButton);
    QVERIFY(Theme::savedAppearance() == Theme::Appearance());
}

void ThemeTest::appearanceMenu()
{
    MainWindow window;
    window.resize(640, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *themes = window.findChild<QMenu *>(QStringLiteral("ThemeMenu"));
    QVERIFY(themes);
    Q_EMIT themes->aboutToShow();
    QAction *nord = nullptr;
    for (QAction *action : themes->actions()) {
        if (action->data().toString() == QLatin1String("nord"))
            nord = action;
        QCOMPARE(action->isChecked(), action->data().toString() == Theme::appearance().theme);
        QVERIFY(!action->icon().isNull());
    }
    QVERIFY(nord);
    nord->trigger();
    QCOMPARE(Theme::appearance().theme, QStringLiteral("nord"));
    QCOMPARE(Theme::savedAppearance().theme, QStringLiteral("nord"));
    // The title shown on the start screen follows.
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("EmptyStateTitle"))->text().contains(Theme::hex(Theme::Accent)));

    // Ctrl+Shift+T, a shortcut of its own, opens the dialog.
    QList<QAction *> bound;
    for (QAction *action : window.actions()) {
        if (action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T))
            bound.append(action);
    }
    QCOMPARE(bound.size(), 1);
    bound.first()->trigger();
    QTRY_VERIFY(window.appearanceDialog() && window.appearanceDialog()->isVisible());
    window.appearanceDialog()->reject();
    QCOMPARE(Theme::appearance().theme, QStringLiteral("nord"));
    Theme::saveAppearance(Theme::Appearance());
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::screenshots()
{
    const QString dir = qEnvironmentVariable("TOPPLAYER_SCREENSHOTS");
    if (dir.isEmpty())
        QSKIP("TOPPLAYER_SCREENSHOTS is not set");
    QDir().mkpath(dir);
    MainWindow window;
    window.resize(900, 520);
    window.show();
    window.setPlaylistVisible(true);
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.openAppearanceDialog();
    QWidget *dialog = window.appearanceDialog();
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    const QList<Theme::Appearance> looks{
        {QStringLiteral("midnight"), QStringLiteral("violet"), QStringLiteral("glass")},
        {QStringLiteral("obsidian"), QStringLiteral("cyan"), QStringLiteral("glass")},
        {QStringLiteral("catppuccin-mocha"), QStringLiteral("native"), QStringLiteral("soft")},
        {QStringLiteral("gruvbox"), QStringLiteral("native"), QStringLiteral("tiling")},
        {QStringLiteral("nord"), QStringLiteral("mint"), QStringLiteral("glass")},
        {QStringLiteral("catppuccin-latte"), QStringLiteral("violet"), QStringLiteral("glass")},
        {QStringLiteral("matte-black"), QStringLiteral("gold"), QStringLiteral("tiling")},
    };
    for (const Theme::Appearance &look : looks) {
        Theme::setAppearance(look);
        QTest::qWait(50);
        const QString name = look.theme + QLatin1Char('-') + look.accent + QLatin1Char('-') + look.style;
        window.grab().save(QStringLiteral("%1/window-%2.png").arg(dir, name));
        dialog->grab().save(QStringLiteral("%1/dialog-%2.png").arg(dir, name));
    }
    QPixmap logo = appLogo(256, 1.0);
    logo.save(dir + QStringLiteral("/logo.png"));
    dialog->hide();
    Theme::setAppearance(Theme::Appearance());
}

void ThemeTest::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
}

int main(int argc, char *argv[])
{
    // Keep the settings away from the user's own.
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    g_previousHandler = qInstallMessageHandler(captureWarnings);
    ThemeTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_theme.moc"
