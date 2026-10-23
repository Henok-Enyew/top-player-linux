#include "MemoryUsage.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QTimer>

#include <atomic>

#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

// Enough for the decoder threads to rarely wait on each other, few enough
// that freed memory is found again instead of piling up per thread.
constexpr int kArenas = 2;
// Several releases in a row (a helper player and a file's caches) are one trim.
constexpr int kTrimDelayMs = 400;

std::atomic<bool> s_trimScheduled{false};

void trimNow()
{
    s_trimScheduled = false;
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

// A field of /proc/self/smaps_rollup in bytes, or -1.
qint64 rollupField(const char *name)
{
    QFile file(QStringLiteral("/proc/self/smaps_rollup"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    const QByteArray prefix = QByteArray(name) + ':';
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (line.startsWith(prefix)) {
            bool ok = false;
            const qint64 kib = line.mid(prefix.size()).trimmed().split(' ').value(0).toLongLong(&ok);
            return ok ? kib * 1024 : -1;
        }
    }
    return -1;
}

} // namespace

void MemoryUsage::tuneAllocator()
{
#ifdef __GLIBC__
    if (qEnvironmentVariableIsEmpty("MALLOC_ARENA_MAX"))
        mallopt(M_ARENA_MAX, kArenas);
#endif
}

void MemoryUsage::releaseFreed()
{
    if (s_trimScheduled.exchange(true))
        return;
    QCoreApplication *app = QCoreApplication::instance();
    if (!app) {
        trimNow();
        return;
    }
    // From any thread: the timer runs in the application's.
    QMetaObject::invokeMethod(app, [] { QTimer::singleShot(kTrimDelayMs, &trimNow); }, Qt::QueuedConnection);
}

qint64 MemoryUsage::residentBytes()
{
    if (const qint64 rss = rollupField("Rss"); rss >= 0)
        return rss;
    // Kernels before 4.14 have no smaps_rollup.
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return -1;
    bool ok = false;
    const qint64 pages = statm.readAll().split(' ').value(1).toLongLong(&ok);
    return ok ? pages * sysconf(_SC_PAGESIZE) : -1;
}

qint64 MemoryUsage::anonymousBytes()
{
    return rollupField("Anonymous");
}
