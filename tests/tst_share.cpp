// Show in File Manager and Share: which entries get which actions, the
// links handed to the sharing sites, the clipboard, and the programs started
// for Telegram and email (fakes on $PATH that note their arguments).

#include "FileShare.h"
#include "HostTools.h"

#include <QAction>
#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QMenu>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

class ShareTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void entries();
    void shareLinks();
    void menuForFile();
    void menuForLink();
    void copyFiles();
    void telegram();
    void telegramMissing();
    void email();

private:
    // A program named `name` on $PATH that writes its arguments, one per line, to `name`.args.
    void fakeProgram(const QString &name);
    QStringList argumentsOf(const QString &name) const;
    QStringList actionTexts(QMenu *menu) const;

    QTemporaryDir m_dir;
    QString m_bin;
    QString m_song;
};

void ShareTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_bin = m_dir.filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(m_bin));
    // Only the fakes (and the shell's basics) are found.
    qputenv("PATH", (m_bin + QStringLiteral(":/usr/bin:/bin")).toUtf8());
    m_song = m_dir.filePath(QStringLiteral("My Song (live).mp3"));
    QFile song(m_song);
    QVERIFY(song.open(QIODevice::WriteOnly));
    song.write("ID3");
}

void ShareTest::fakeProgram(const QString &name)
{
    const QString path = m_bin + QLatin1Char('/') + name;
    QFile script(path);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1.args'\n").arg(m_dir.filePath(name)).toUtf8());
    script.close();
    script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QStringList ShareTest::argumentsOf(const QString &name) const
{
    QFile file(m_dir.filePath(name + QStringLiteral(".args")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

QStringList ShareTest::actionTexts(QMenu *menu) const
{
    QStringList texts;
    for (QAction *action : menu->actions()) {
        if (!action->isSeparator())
            texts.append(action->text());
    }
    return texts;
}

void ShareTest::entries()
{
    QCOMPARE(FileShare::localFile(m_song), m_song);
    QCOMPARE(FileShare::localFile(QUrl::fromLocalFile(m_song).toString()), m_song);
    QVERIFY(FileShare::localFile(m_dir.filePath(QStringLiteral("missing.mp3"))).isEmpty());
    QVERIFY(FileShare::localFile(QStringLiteral("https://youtu.be/abc")).isEmpty());

    QCOMPARE(FileShare::webLink(QStringLiteral("https://www.youtube.com/watch?v=abc")),
             QStringLiteral("https://www.youtube.com/watch?v=abc"));
    QCOMPARE(FileShare::webLink(QStringLiteral("ytdl://https://youtu.be/abc")), QStringLiteral("https://youtu.be/abc"));
    // A song found by name is shared as the same search on YouTube.
    QCOMPARE(FileShare::webLink(QStringLiteral("ytdl://ytsearch1:Artist - Song & Co")),
             QStringLiteral("https://www.youtube.com/results?search_query=Artist%20-%20Song%20%26%20Co"));
    QVERIFY(FileShare::webLink(m_song).isEmpty());
    QVERIFY(FileShare::webLink(QStringLiteral("rtsp://camera/stream")).isEmpty());

    // Paths that aren't the Flatpak portal's come back as they are, and so
    // do portal paths it can't resolve.
    QCOMPARE(HostTools::realPath(m_song), m_song);
    QCOMPARE(HostTools::realPath(QStringLiteral("/run/user/1000/doc/77eab690/Music")),
             QStringLiteral("/run/user/1000/doc/77eab690/Music"));
}

void ShareTest::shareLinks()
{
    const QString link = QStringLiteral("https://youtu.be/abc?t=1&x=2");
    QCOMPARE(FileShare::shareLinkUrl(QStringLiteral("telegram"), link, QStringLiteral("A & B")),
             QStringLiteral("https://t.me/share/url?url=https%3A%2F%2Fyoutu.be%2Fabc%3Ft%3D1%26x%3D2&text=A%20%26%20B"));
    QCOMPARE(FileShare::shareLinkUrl(QStringLiteral("whatsapp"), link, QStringLiteral("Song")),
             QStringLiteral("https://wa.me/?text=Song%20https%3A%2F%2Fyoutu.be%2Fabc%3Ft%3D1%26x%3D2"));
    QVERIFY(FileShare::shareLinkUrl(QStringLiteral("x"), link, {}).startsWith(QLatin1String("https://x.com/intent/tweet?url=")));
    QVERIFY(FileShare::shareLinkUrl(QStringLiteral("facebook"), link, {}).startsWith(QLatin1String("https://www.facebook.com/sharer/")));
    QVERIFY(FileShare::shareLinkUrl(QStringLiteral("reddit"), link, {}).startsWith(QLatin1String("https://www.reddit.com/submit?")));
    QVERIFY(FileShare::shareLinkUrl(QStringLiteral("email"), link, {}).startsWith(QLatin1String("mailto:?")));
    QVERIFY(FileShare::shareLinkUrl(QStringLiteral("nowhere"), link, {}).isEmpty());
}

void ShareTest::menuForFile()
{
    QMenu menu;
    FileShare::addActions(&menu, {m_song}, {QStringLiteral("My Song")}, {});
    QCOMPARE(actionTexts(&menu), (QStringList{QStringLiteral("Show in File Manager"), QStringLiteral("Share")}));
    auto *share = menu.findChild<QMenu *>(QStringLiteral("ShareMenu"));
    QVERIFY(share);
    const QStringList items = actionTexts(share);
    QVERIFY(items.contains(QStringLiteral("Telegram")));
    QVERIFY(items.contains(QStringLiteral("Email...")));
    QVERIFY(items.contains(QStringLiteral("Copy File (Paste in Any Chat or Upload)")));
    QVERIFY(!items.contains(QStringLiteral("WhatsApp (Link)")));

    // Nothing for entries that are neither files nor web pages.
    QMenu empty;
    FileShare::addActions(&empty, {QStringLiteral("rtsp://camera/stream")}, {QString()}, {});
    QVERIFY(empty.actions().isEmpty());
}

void ShareTest::menuForLink()
{
    QMenu menu;
    FileShare::addActions(&menu, {QStringLiteral("https://youtu.be/abc")}, {QStringLiteral("Video")}, {});
    // No file to show; the link goes to the sites.
    QCOMPARE(actionTexts(&menu), QStringList{QStringLiteral("Share")});
    const QStringList items = actionTexts(menu.findChild<QMenu *>(QStringLiteral("ShareMenu")));
    for (const char *site : {"Telegram (Link)", "WhatsApp (Link)", "X (Twitter) (Link)", "Facebook (Link)", "Reddit (Link)",
                             "Email (Link)", "Copy Link"})
        QVERIFY2(items.contains(QString::fromLatin1(site)), site);
}

void ShareTest::copyFiles()
{
    FileShare::copyFiles({m_song});
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QVERIFY(mime);
    QCOMPARE(mime->urls(), QList<QUrl>{QUrl::fromLocalFile(m_song)});
    QCOMPARE(QString::fromUtf8(mime->data(QStringLiteral("x-special/gnome-copied-files"))),
             QStringLiteral("copy\n") + QUrl::fromLocalFile(m_song).toString(QUrl::FullyEncoded));
}

void ShareTest::telegram()
{
    fakeProgram(QStringLiteral("telegram-desktop"));
    QStringList messages;
    FileShare::sendToTelegram(m_song, [&](const QString &label, const QString &) { messages.append(label); });
    QTRY_COMPARE(argumentsOf(QStringLiteral("telegram-desktop")), (QStringList{QStringLiteral("-sendpath"), m_song}));
    QTRY_COMPARE(messages, QStringList{QStringLiteral("Sharing on Telegram:")});
    QFile::remove(m_bin + QStringLiteral("/telegram-desktop"));
}

void ShareTest::telegramMissing()
{
    QStringList messages;
    FileShare::sendToTelegram(m_song, [&](const QString &label, const QString &) { messages.append(label); });
    QTRY_COMPARE(messages, QStringList{QStringLiteral("Telegram Desktop is not installed")});
}

void ShareTest::email()
{
    fakeProgram(QStringLiteral("xdg-email"));
    const QString other = m_dir.filePath(QStringLiteral("other.flac"));
    FileShare::email({m_song, other}, {});
    QTRY_COMPARE(argumentsOf(QStringLiteral("xdg-email")),
                 (QStringList{QStringLiteral("--attach"), m_song, QStringLiteral("--attach"), other}));
}

QTEST_MAIN(ShareTest)
#include "tst_share.moc"
