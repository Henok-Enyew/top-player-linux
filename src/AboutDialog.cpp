#include "AboutDialog.h"
#include "Icons.h"
#include "MemoryUsage.h"
#include "MpvWidget.h"
#include "Theme.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <mpv/client.h>

#include <algorithm>

namespace {

constexpr auto kGitHubUrl = "https://github.com/Henok-Enyew/pot-player-linux";
constexpr auto kTelegramUrl = "https://t.me/enoch90s";
// Replace with the portfolio address once it is published.
constexpr auto kPortfolioUrl = "https://github.com/Henok-Enyew";

QString link(const char *url, const QString &text)
{
    return QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
        .arg(QString::fromLatin1(url), Theme::hex(Theme::Accent), text.toHtmlEscaped());
}

QLabel *label(const QString &text, const char *objectName, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QLatin1String(objectName));
    result->setAlignment(Qt::AlignHCenter);
    result->setWordWrap(true);
    return result;
}

constexpr int kMemoryRefreshMs = 2000;

QString megabytes(qint64 bytes)
{
    return QString::number(std::max<qint64>(1, (bytes + 512 * 1024) / (1024 * 1024)));
}

// "182 MB (330 MB with shared libraries)".
QString memoryText()
{
    const qint64 own = MemoryUsage::anonymousBytes();
    const qint64 resident = MemoryUsage::residentBytes();
    if (own < 0)
        return resident < 0 ? AboutDialog::tr("Unknown") : AboutDialog::tr("%1 MB").arg(megabytes(resident));
    return AboutDialog::tr("%1 MB (%2 MB with shared libraries)").arg(megabytes(own), megabytes(resident));
}

} // namespace

QString AboutDialog::hwdecName(const QString &hwdec)
{
    if (hwdec.isEmpty() || hwdec == QLatin1String("no"))
        return {};
    QString base = hwdec;
    const bool copy = base.endsWith(QLatin1String("-copy"));
    if (copy)
        base.chop(5);
    static const QList<QPair<QString, QString>> names{
        {QStringLiteral("vaapi"), QStringLiteral("VA-API")},
        {QStringLiteral("nvdec"), QStringLiteral("NVDEC")},
        {QStringLiteral("cuda"), QStringLiteral("CUDA")},
        {QStringLiteral("vdpau"), QStringLiteral("VDPAU")},
        {QStringLiteral("vulkan"), QStringLiteral("Vulkan Video")},
        {QStringLiteral("drm"), QStringLiteral("DRM (V4L2)")},
        {QStringLiteral("v4l2m2m"), QStringLiteral("V4L2 M2M")},
    };
    QString name = base;
    for (const auto &entry : names) {
        if (entry.first == base)
            name = entry.second;
    }
    return copy ? tr("%1 (copy-back)").arg(name) : name;
}

AboutDialog::AboutDialog(MpvWidget *mpv, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("AboutDialog"));
    setWindowTitle(tr("About Top Player"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 18);
    layout->setSpacing(6);

    auto *logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("AboutLogo"));
    logo->setAlignment(Qt::AlignHCenter);
    logo->setPixmap(appLogo(96, devicePixelRatioF()));
    layout->addWidget(logo);
    layout->addSpacing(4);

    QLabel *title = label(QString(), "AboutTitle", this);
    Theme::bind(title, [title] {
        title->setText(QStringLiteral("%1 <span style=\"color:%2; font-weight:normal;\">— %3</span>")
                           .arg(tr("Top Player"), Theme::hex(Theme::Accent), tr("Version %1").arg(QStringLiteral(APP_VERSION))));
    });
    title->setTextFormat(Qt::RichText);
    title->setAccessibleName(tr("Top Player — Version %1").arg(QStringLiteral(APP_VERSION)));
    layout->addWidget(title);
    layout->addWidget(label(tr("High-performance, lightweight native media player for Linux powered by Qt6 & libmpv."),
                            "AboutTagline", this));
    layout->addSpacing(8);
    layout->addWidget(label(tr("Developed by Henok Enyew Andargie"), "AboutByline", this));

    QLabel *links = label(QString(), "AboutLinks", this);
    Theme::bind(links, [links] {
        links->setText(QStringLiteral("%1 &nbsp;·&nbsp; %2 &nbsp;·&nbsp; %3")
                           .arg(link(kGitHubUrl, tr("GitHub")), link(kTelegramUrl, tr("Telegram")),
                                link(kPortfolioUrl, tr("Portfolio"))));
    });
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);
    layout->addWidget(links);
    layout->addSpacing(10);

    // System details.
    auto *system = new QFrame(this);
    system->setObjectName(QStringLiteral("AboutSystem"));
    auto *grid = new QGridLayout(system);
    grid->setContentsMargins(14, 10, 14, 12);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);
    auto *heading = new QLabel(tr("System"), system);
    heading->setObjectName(QStringLiteral("AboutSystemHeading"));
    grid->addWidget(heading, 0, 0, 1, 2);
    const auto addRow = [system, grid](const QString &key, const QString &value, const char *name) {
        const int row = grid->rowCount();
        auto *keyLabel = new QLabel(key, system);
        keyLabel->setProperty("role", QStringLiteral("key"));
        auto *valueLabel = new QLabel(value, system);
        valueLabel->setObjectName(QLatin1String(name));
        valueLabel->setProperty("role", QStringLiteral("value"));
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valueLabel->setWordWrap(true);
        grid->addWidget(keyLabel, row, 0, Qt::AlignTop);
        grid->addWidget(valueLabel, row, 1);
    };

    addRow(tr("Qt"), QString::fromLatin1(qVersion()), "AboutQtVersion");

    const unsigned long api = mpv_client_api_version();
    const QString apiVersion = QStringLiteral("%1.%2").arg(api >> 16).arg(api & 0xFFFF);
    QString mpvVersion = mpv ? mpv->mpvPropertyString(QStringLiteral("mpv-version")) : QString();
    mpvVersion = mpvVersion.isEmpty() ? tr("API %1").arg(apiVersion)
                                      : tr("%1 (API %2)").arg(mpvVersion, apiVersion);
    addRow(tr("libmpv"), mpvVersion, "AboutMpvVersion");

    QString acceleration;
    if (mpv) {
        const QString active = hwdecName(mpv->mpvPropertyString(QStringLiteral("hwdec-current")));
        const QString requested = mpv->mpvPropertyString(QStringLiteral("hwdec"));
        if (!active.isEmpty())
            acceleration = tr("%1 (active)").arg(active);
        else if (mpv->isIdle())
            acceleration = tr("Idle (hwdec: %1)").arg(requested);
        else
            acceleration = tr("Software decoding (hwdec: %1)").arg(requested);
    }
    addRow(tr("Video acceleration"), acceleration, "AboutHwdec");
    if (mpv && !mpv->glRenderer().isEmpty())
        addRow(tr("Renderer"), mpv->glRenderer(), "AboutRenderer");
    // The player's own memory, kept up to date while the dialog is open.
    addRow(tr("Memory"), memoryText(), "AboutMemory");
    auto *memory = system->findChild<QLabel *>(QStringLiteral("AboutMemory"));
    auto *memoryTimer = new QTimer(this);
    memoryTimer->setInterval(kMemoryRefreshMs);
    connect(memoryTimer, &QTimer::timeout, memory, [memory] { memory->setText(memoryText()); });
    memoryTimer->start();
    layout->addWidget(system);
    layout->addSpacing(8);

    layout->addWidget(label(tr("MIT License · Copyright © 2026 Henok Enyew Andargie and Top Player contributors"),
                            "AboutLicense", this));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setCenterButtons(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(6);
    layout->addWidget(buttons);

    setMinimumWidth(480);
}
