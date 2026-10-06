#include "Icons.h"
#include "MainWindow.h"
#include "PlaylistSession.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QNetworkProxyFactory>
#include <QSurfaceFormat>

#include "MpvWidget.h"

#include <cstring>

#include <clocale>

namespace {

bool hasArgument(int argc, char *argv[], const char *name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    // X11 mode: in a Wayland session, run through XWayland so the mini player
    // can stay above other apps and on every workspace (Wayland allows
    // neither). An explicit $QT_QPA_PLATFORM, or --native-wayland, wins.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && !qEnvironmentVariableIsEmpty("DISPLAY") && !hasArgument(argc, argv, "--native-wayland")
        && PlaylistSession::x11Mode()) {
        qputenv("QT_QPA_PLATFORM", "xcb;wayland");
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("top-player"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.github.topplayer"));
    QApplication::setApplicationDisplayName(QStringLiteral("Top Player"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    // The installed icon theme wins; the built-in logo covers running from the build tree.
    QIcon icon = QIcon::fromTheme(QStringLiteral("org.github.topplayer"));
    if (icon.isNull()) {
        for (int size : {16, 24, 32, 48, 64, 128, 256})
            icon.addPixmap(appLogo(size, 1.0));
    }
    QApplication::setWindowIcon(icon);

    // libmpv requires the C numeric locale; QApplication may have changed it.
    std::setlocale(LC_NUMERIC, "C");
    PlaylistSession::migrateLegacyConfig();
    applyDarkSkin(app);
    // Subtitle downloads go through the desktop's proxy settings ($https_proxy, ...).
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Top Player: a high-performance, lightweight media player for Linux"));
    parser.addHelpOption();
    parser.addVersionOption();
    // Used when the player restarts itself (X11 mode): reopen the queue where
    // it was, playing or not, in the mini player or not.
    QCommandLineOption handoff(QStringLiteral("handoff"), QStringLiteral("Reopen the saved queue and position."));
    QCommandLineOption play(QStringLiteral("play"), QStringLiteral("Start playing the reopened queue."));
    QCommandLineOption mini(QStringLiteral("mini"), QStringLiteral("Start in the mini player."));
    QCommandLineOption nativeWayland(QStringLiteral("native-wayland"),
                                     QStringLiteral("Run as a Wayland app even if X11 mode is on."));
    for (QCommandLineOption *option : {&handoff, &play})
        option->setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({handoff, play, mini, nativeWayland});
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Media files or URLs to play; extra files are queued."),
                                 QStringLiteral("[files...]"));
    parser.process(app);

    MainWindow window;
    window.resize(960, 540);
    window.show();

    // Files on the command line replace the queue from the last run.
    const QStringList args = parser.positionalArguments();
    const bool handedOver = parser.isSet(handoff) && args.isEmpty();
    window.startSession(args.isEmpty(), handedOver);
    if (!args.isEmpty())
        window.openFiles(args);
    if (handedOver && parser.isSet(play))
        window.findChild<MpvWidget *>()->setMpvProperty(QStringLiteral("pause"), QStringLiteral("no"));
    if (parser.isSet(mini))
        window.setMiniPlayer(true);

    return app.exec();
}
