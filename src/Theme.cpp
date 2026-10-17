#include "Theme.h"
#include "PlaylistSession.h"

#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStyleFactory>
#include <QToolButton>
#include <QVariantAnimation>

#include <cmath>

namespace {

struct LiveColors {
    QColor accent, accentHover, accentDeep, accent2, accentTint, onAccent;
    QColor surface, panel, raised, border, text, textSecondary, textDim;
};

// Midnight with Brand Violet in Neon Glass, the default appearance, as
// Theme::colors() works it out (tst_theme checks they agree). Literals, so
// the colors are right before anything runs.
LiveColors g_live{
    {0x7C, 0x5C, 0xFF}, {0x96, 0x7D, 0xFF}, {0x57, 0x40, 0xB3}, {0xFF, 0x4F, 0xD8}, {0x2E, 0x28, 0x51}, {0xFF, 0xFF, 0xFF},
    {0x15, 0x16, 0x1E}, {0x1A, 0x1B, 0x26}, {0x23, 0x25, 0x36}, {0x2C, 0x2F, 0x45},
    {0xDD, 0xE3, 0xFB}, {0xA9, 0xB1, 0xD6}, {0x64, 0x6B, 0x92},
};

Theme::Appearance g_appearance;
bool g_applied = false;

constexpr int kCrossFadeMs = 260;
const QString kCrossFadeName = QStringLiteral("ThemeCrossFade");

QString settingsFile()
{
    return PlaylistSession::configDir() + QStringLiteral("/settings.ini");
}

QColor rgb(QRgb value)
{
    return QColor::fromRgb(value);
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

// `color`, lightened (or darkened on a light background) until it stands out
// from `background` by `minimum` (WCAG contrast ratio).
QColor readableOn(const QColor &color, const QColor &background, qreal minimum)
{
    const QColor target = luminance(background) > 0.4 ? QColor(Qt::black) : QColor(Qt::white);
    QColor result = color;
    for (int step = 1; step <= 12 && contrast(result, background) < minimum; ++step)
        result = Theme::mix(color, target, step * 0.06);
    return result;
}

const Theme::Scheme &schemeFor(const QString &id)
{
    for (const Theme::Scheme &scheme : Theme::schemes()) {
        if (scheme.id == id)
            return scheme;
    }
    return Theme::schemes().constFirst();
}

const Theme::AccentPalette &accentFor(const QString &id)
{
    for (const Theme::AccentPalette &accent : Theme::accents()) {
        if (accent.id == id)
            return accent;
    }
    return Theme::accents().constFirst();
}

const Theme::Style &styleFor(const QString &id)
{
    for (const Theme::Style &style : Theme::styles()) {
        if (style.id == id)
            return style;
    }
    return Theme::styles().constFirst();
}

// Known ids only, so what is saved and reported always exists.
Theme::Appearance normalized(const Theme::Appearance &appearance)
{
    return {schemeFor(appearance.theme).id, accentFor(appearance.accent).id, styleFor(appearance.style).id};
}

QString px(int value)
{
    return QString::number(value) + QStringLiteral("px");
}

QString rgba(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QString gradient(const QColor &from, const QColor &to, bool vertical = false)
{
    return QStringLiteral("qlineargradient(x1: 0, y1: 0, x2: %1, y2: %2, stop: 0 %3, stop: 1 %4)")
        .arg(vertical ? 0 : 1)
        .arg(vertical ? 1 : 0)
        .arg(rgba(from), rgba(to));
}

// The QSS template with its comments taken out, read once.
const QString &strippedTemplate()
{
    static const QString text = [] {
        QString qss = Theme::styleSheetTemplate();
        qss.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));
        return qss;
    }();
    return text;
}

QHash<QString, QString> tokens(const Theme::Appearance &appearance)
{
    using Theme::mix;
    using Theme::withAlpha;
    const Theme::Scheme &scheme = schemeFor(appearance.theme);
    const Theme::Style &style = styleFor(appearance.style);
    const Theme::Colors c = Theme::colors(appearance);
    const QColor white(Qt::white);
    const QColor black(Qt::black);
    const QColor end = style.gradients ? (style.softBorders ? mix(c.accent, c.accent2, 0.55) : c.accent2) : c.accent;
    const QColor endHover = style.gradients ? mix(end, c.light ? black : white, 0.15) : c.accentHover;
    const QColor tintStrong = mix(c.panel, c.accent, c.light ? 0.24 : 0.3);
    const QColor hover = mix(c.raised, c.text, c.light ? 0.08 : 0.06);
    const QColor pressed = mix(c.raised, c.text, c.light ? 0.14 : 0.12);
    const QColor selection = mix(c.panel, c.accent, c.light ? 0.16 : 0.24);
    // The cutter's clip stays apart from the accent: sky blue, or violet next to a blue accent.
    const float hue = c.accent.hslHueF() * 360;
    const QColor clip = hue >= 170 && hue <= 235 ? rgb(0xA78BFA) : rgb(0x38BDF8);

    QHash<QString, QString> t;
    auto color = [&t](const char *name, const QColor &value) {
        t.insert(QString::fromLatin1(name), value.alpha() == 255 ? Theme::hex(value) : rgba(value));
    };
    color("surface", c.surface);
    color("panel", c.panel);
    color("raised", c.raised);
    color("border", c.border);
    color("borderStrong", style.softBorders ? c.border : mix(c.border, c.text, 0.08));
    color("field", c.light ? mix(c.raised, c.surface, 0.3) : mix(c.surface, c.panel, 0.3));
    color("text", c.text);
    color("text2", c.textSecondary);
    color("textDim", c.textDim);
    color("rowText", mix(c.text, c.textSecondary, 0.25));
    color("accent", c.accent);
    color("accentHover", c.accentHover);
    color("accentDeep", c.accentDeep);
    color("accent2", c.accent2);
    color("accentTint", c.accentTint);
    color("accentTintStrong", tintStrong);
    color("onAccent", c.onAccent);
    color("hover", hover);
    color("pressed", pressed);
    color("titleLine", mix(c.surface, c.border, 0.75));
    color("controlBackground", c.panel);
    color("actionBar", mix(c.surface, c.panel, 0.55));
    color("chip", mix(c.raised, c.border, 0.25));
    color("chipBorder", mix(c.border, c.text, 0.05));
    color("chipText", mix(c.text, c.textSecondary, 0.5));
    color("chipHover", hover);
    color("chipDisabled", mix(c.panel, c.raised, 0.4));
    color("chipDisabledText", c.textDim);
    color("altRow", mix(c.surface, c.panel, 0.5));
    color("overlay", withAlpha(c.surface, style.gradients && !style.softBorders ? 0.84 : 0.9));
    color("overlayLine", withAlpha(c.text, 0.09));
    color("miniBg", withAlpha(c.surface, 0.78));
    color("miniLine", withAlpha(c.text, 0.12));
    color("accentGlow", withAlpha(c.accent, 0.35));
    color("progressHover", withAlpha(c.accentHover, 0.24));
    color("progressEnd", end);
    color("seekGlow", style.glow ? withAlpha(c.accent, c.light ? 0.22 : 0.4) : QColor(0, 0, 0, 0));
    color("groove", c.light ? mix(c.border, c.raised, 0.2) : mix(c.border, c.raised, 0.3));
    color("handle", c.light ? c.accent : rgb(0xFFFFFF));
    color("scrollHandle", style.softBorders ? mix(c.raised, c.border, 0.6) : c.border);
    color("clip", clip);
    color("menuSelectedText", style.mono ? c.onAccent : (c.light ? c.accentDeep : c.accentHover));

    auto fill = [&t](const char *name, const QString &value) { t.insert(QString::fromLatin1(name), value); };
    fill("accentFill", style.gradients ? gradient(c.accent, end) : Theme::hex(c.accent));
    fill("accentFillHover", style.gradients ? gradient(c.accentHover, endHover) : Theme::hex(c.accentHover));
    fill("accentFillV", style.gradients ? gradient(end, c.accent, true) : Theme::hex(c.accent));
    fill("selection", style.gradients ? gradient(selection, mix(c.panel, end, c.light ? 0.06 : 0.1)) : Theme::hex(selection));
    // Tiling Minimal marks the menu item as a solid block, like a tiling bar.
    fill("menuSelection", style.mono ? Theme::hex(c.accent)
                                     : style.gradients ? gradient(selection, mix(c.panel, end, 0.12))
                                                       : Theme::hex(c.accentTint));
    fill("titleBackground", style.gradients ? gradient(mix(c.surface, c.panel, 0.7), c.surface, true) : Theme::hex(c.surface));
    fill("cardFill", style.gradients ? gradient(c.panel, mix(c.panel, c.raised, 0.5), true) : Theme::hex(c.panel));
    // The play button: a solid accent where white reads on it, else a wash.
    const bool solidPlay = !scheme.light && c.onAccent == white;
    fill("playFill", solidPlay ? (style.gradients ? gradient(c.accent, end) : Theme::hex(c.accent)) : Theme::hex(c.accentTint));
    fill("playFillHover", solidPlay ? (style.gradients ? gradient(c.accentHover, endHover) : Theme::hex(c.accentHover))
                                    : Theme::hex(tintStrong));
    fill("playBorder", solidPlay ? QStringLiteral("none") : QStringLiteral("1px solid ") + Theme::hex(c.accent));

    fill("radius", px(style.radius));
    fill("radiusSmall", px(style.radiusSmall));
    fill("radiusLarge", px(style.radiusLarge));
    fill("radiusCheck", px(std::min(style.radiusSmall, 4)));
    fill("radiusScroll", px(style.radius == 0 ? 0 : 4));
    fill("radiusPlay", px(style.mono ? style.radiusSmall : 15));
    fill("line", style.softBorders ? QStringLiteral("0px") : QStringLiteral("1px"));

    // Whole declarations, where the styles differ in more than a value.
    fill("toolBorder", style.mono ? QStringLiteral("1px solid transparent") : QStringLiteral("none"));
    if (style.mono)
        fill("checkedRule", QStringLiteral("background: transparent; border: 1px solid %1;").arg(Theme::hex(c.accent)));
    else if (style.softBorders)
        fill("checkedRule", QStringLiteral("background: %1;").arg(Theme::hex(c.accentTint)));
    else
        fill("checkedRule", QStringLiteral("background: %1; border-bottom: 2px solid %2;")
                                .arg(Theme::hex(c.accentTint), Theme::hex(c.accent)));
    fill("focusRule", style.mono ? QStringLiteral("border: 2px solid %1; padding: 2px 4px;").arg(Theme::hex(c.accent))
                                 : QStringLiteral("border-color: %1;").arg(Theme::hex(c.accent)));
    fill("headingFont", style.mono ? QStringLiteral("font-family: \"JetBrains Mono\", \"DejaVu Sans Mono\", "
                                                    "\"Noto Sans Mono\", monospace;")
                                   : QString());
    return t;
}

QPalette paletteFor(const Theme::Colors &c)
{
    QPalette palette;
    palette.setColor(QPalette::Window, c.surface);
    palette.setColor(QPalette::WindowText, c.text);
    palette.setColor(QPalette::Base, c.surface);
    palette.setColor(QPalette::AlternateBase, c.panel);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.raised);
    palette.setColor(QPalette::ButtonText, c.text);
    palette.setColor(QPalette::Light, Theme::mix(c.raised, c.text, 0.12));
    palette.setColor(QPalette::Midlight, Theme::mix(c.raised, c.text, 0.06));
    palette.setColor(QPalette::Mid, c.border);
    palette.setColor(QPalette::Dark, Theme::mix(c.surface, QColor(Qt::black), 0.3));
    palette.setColor(QPalette::Highlight, c.accent);
    palette.setColor(QPalette::HighlightedText, c.onAccent);
    palette.setColor(QPalette::ToolTipBase, c.panel);
    palette.setColor(QPalette::ToolTipText, c.text);
    palette.setColor(QPalette::PlaceholderText, c.textDim);
    palette.setColor(QPalette::Link, c.accent);
    palette.setColor(QPalette::LinkVisited, c.accentHover);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, c.textDim);
    return palette;
}

// A snapshot of a window in its old look, fading out over the new one.
class CrossFade : public QWidget
{
public:
    CrossFade(QWidget *window, const QPixmap &snapshot)
        : QWidget(window)
        , m_snapshot(snapshot)
    {
        setObjectName(kCrossFadeName);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(window->rect());
        auto *fade = new QVariantAnimation(this);
        fade->setDuration(kCrossFadeMs);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setEasingCurve(QEasingCurve::InOutQuad);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_opacity = value.toReal();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        raise();
        show();
        fade->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setOpacity(m_opacity);
        p.drawPixmap(0, 0, m_snapshot);
    }

private:
    QPixmap m_snapshot;
    qreal m_opacity = 1.0;
};

struct Snapshot {
    QPointer<QWidget> window;
    QPixmap pixmap;
};

QList<Snapshot> snapshotWindows()
{
    QList<Snapshot> result;
    for (QWidget *window : QApplication::topLevelWidgets()) {
        const Qt::WindowType type = window->windowType();
        if (!window->isVisible() || window->isMinimized() || (type != Qt::Window && type != Qt::Dialog))
            continue;
        result.append({window, window->grab()});
        // A fade still running is part of the snapshot now.
        for (QWidget *old : window->findChildren<QWidget *>(kCrossFadeName, Qt::FindDirectChildrenOnly))
            delete old;
    }
    return result;
}

} // namespace

const QColor &Theme::Accent = g_live.accent;
const QColor &Theme::AccentHover = g_live.accentHover;
const QColor &Theme::AccentDeep = g_live.accentDeep;
const QColor &Theme::Accent2 = g_live.accent2;
const QColor &Theme::AccentTint = g_live.accentTint;
const QColor &Theme::OnAccent = g_live.onAccent;
const QColor &Theme::Surface = g_live.surface;
const QColor &Theme::Panel = g_live.panel;
const QColor &Theme::Raised = g_live.raised;
const QColor &Theme::Border = g_live.border;
const QColor &Theme::TextPrimary = g_live.text;
const QColor &Theme::TextSecondary = g_live.textSecondary;
const QColor &Theme::TextDim = g_live.textDim;

QColor Theme::withAlpha(const QColor &color, qreal alpha)
{
    QColor result = color;
    result.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
    return result;
}

QColor Theme::mix(const QColor &a, const QColor &b, qreal amount)
{
    auto channel = [amount](int from, int to) { return qRound(from + (to - from) * amount); };
    return QColor(channel(a.red(), b.red()), channel(a.green(), b.green()), channel(a.blue(), b.blue()),
                  channel(a.alpha(), b.alpha()));
}

const QList<Theme::Scheme> &Theme::schemes()
{
    // surface, panel, raised, border; text, secondary, dim; accent, accent 2.
    static const QList<Scheme> list{
        {QStringLiteral("midnight"), QStringLiteral("Midnight"), false,
         rgb(0x15161E), rgb(0x1A1B26), rgb(0x232536), rgb(0x2C2F45),
         rgb(0xDDE3FB), rgb(0xA9B1D6), rgb(0x646B92), rgb(0x7AA2F7), rgb(0xBB9AF7)},
        {QStringLiteral("obsidian"), QStringLiteral("Obsidian"), false,
         rgb(0x121316), rgb(0x1A1C22), rgb(0x22252C), rgb(0x2A2D35),
         rgb(0xFFFFFF), rgb(0xA0A5B5), rgb(0x6B7080), rgb(0x00D2FF), rgb(0x3A7BFF)},
        {QStringLiteral("matte-black"), QStringLiteral("Matte Black"), false,
         rgb(0x0B0B0B), rgb(0x121212), rgb(0x1B1B1B), rgb(0x262626),
         rgb(0xEAEAEA), rgb(0xA3A3A3), rgb(0x606060), rgb(0xF0A030), rgb(0xE0582F)},
        {QStringLiteral("catppuccin-mocha"), QStringLiteral("Catppuccin Mocha"), false,
         rgb(0x181825), rgb(0x1E1E2E), rgb(0x292A3E), rgb(0x313244),
         rgb(0xCDD6F4), rgb(0xA6ADC8), rgb(0x6C7086), rgb(0xCBA6F7), rgb(0xF5C2E7)},
        {QStringLiteral("nord"), QStringLiteral("Nord"), false,
         rgb(0x242933), rgb(0x2E3440), rgb(0x3B4252), rgb(0x434C5E),
         rgb(0xECEFF4), rgb(0xB8C0D0), rgb(0x6F7A8F), rgb(0x88C0D0), rgb(0xB48EAD)},
        {QStringLiteral("gruvbox"), QStringLiteral("Gruvbox"), false,
         rgb(0x1D2021), rgb(0x282828), rgb(0x32302F), rgb(0x3C3836),
         rgb(0xEBDBB2), rgb(0xBDAE93), rgb(0x7C6F64), rgb(0xFABD2F), rgb(0xFE8019)},
        {QStringLiteral("rose-pine"), QStringLiteral("Rosé Pine"), false,
         rgb(0x191724), rgb(0x1F1D2E), rgb(0x26233A), rgb(0x312E48),
         rgb(0xE0DEF4), rgb(0x908CAA), rgb(0x6E6A86), rgb(0xEBBCBA), rgb(0xC4A7E7)},
        {QStringLiteral("everforest"), QStringLiteral("Everforest"), false,
         rgb(0x232A2E), rgb(0x2D353B), rgb(0x343F44), rgb(0x3D484D),
         rgb(0xD3C6AA), rgb(0x9DA9A0), rgb(0x7A8478), rgb(0xA7C080), rgb(0x83C092)},
        {QStringLiteral("kanagawa"), QStringLiteral("Kanagawa"), false,
         rgb(0x16161D), rgb(0x1F1F28), rgb(0x2A2A37), rgb(0x363646),
         rgb(0xDCD7BA), rgb(0xA6A69C), rgb(0x727169), rgb(0x7E9CD8), rgb(0x957FB8)},
        {QStringLiteral("catppuccin-latte"), QStringLiteral("Catppuccin Latte"), true,
         rgb(0xE6E9EF), rgb(0xEFF1F5), rgb(0xFBFCFD), rgb(0xCCD0DA),
         rgb(0x4C4F69), rgb(0x6C6F85), rgb(0x8C8FA1), rgb(0x8839EF), rgb(0xEA76CB)},
    };
    return list;
}

const QList<Theme::AccentPalette> &Theme::accents()
{
    static const QList<AccentPalette> list{
        {QStringLiteral("violet"), QStringLiteral("Brand Violet"), BrandViolet, BrandMagenta},
        {QStringLiteral("native"), QStringLiteral("Theme Native"), {}, {}},
        {QStringLiteral("cyan"), QStringLiteral("Electric Cyan"), rgb(0x00D2FF), rgb(0x3A7BFF)},
        {QStringLiteral("mint"), QStringLiteral("Neon Mint"), rgb(0x2EF2B0), rgb(0x00B8FF)},
        {QStringLiteral("sunset"), QStringLiteral("Sunset Orange"), rgb(0xFF7A3D), rgb(0xFF3D77)},
        {QStringLiteral("pink"), QStringLiteral("Hot Pink"), rgb(0xFF4FA3), rgb(0xFF9A4F)},
        {QStringLiteral("lime"), QStringLiteral("Acid Lime"), rgb(0xB6FF3B), rgb(0x2EF2B0)},
        {QStringLiteral("gold"), QStringLiteral("Gold"), rgb(0xFFC83D), rgb(0xFF7A3D)},
        {QStringLiteral("crimson"), QStringLiteral("Crimson"), rgb(0xFF2E63), rgb(0xFF8A5C)},
        {QStringLiteral("ocean"), QStringLiteral("Ocean Blue"), rgb(0x3D8BFF), rgb(0x2EE6FF)},
    };
    return list;
}

const QList<Theme::Style> &Theme::styles()
{
    // radius, small, large; gradients, glow, mono, soft borders.
    static const QList<Style> list{
        {QStringLiteral("glass"), QStringLiteral("Neon Glass"),
         QStringLiteral("Soft gradients, translucent overlays and an accent glow."), 7, 5, 12, true, true, false, false},
        {QStringLiteral("tiling"), QStringLiteral("Tiling Minimal"),
         QStringLiteral("Crisp accent borders, tight corners, flat fills, monospace headings."), 2, 0, 3, false, false, true, false},
        {QStringLiteral("soft"), QStringLiteral("Soft"),
         QStringLiteral("Large radii, pill buttons and quiet, low-contrast edges."), 14, 9, 18, true, false, false, true},
    };
    return list;
}

Theme::Colors Theme::colors(const Appearance &appearance)
{
    const Scheme &scheme = schemeFor(appearance.theme);
    const AccentPalette &palette = accentFor(appearance.accent);
    const Style &style = styleFor(appearance.style);
    const bool native = !palette.accent.isValid();
    const QColor white(Qt::white);
    const QColor black(Qt::black);

    Colors c;
    c.light = scheme.light;
    c.surface = scheme.surface;
    c.panel = scheme.panel;
    c.raised = scheme.raised;
    c.border = style.softBorders ? mix(scheme.border, scheme.panel, 0.5) : scheme.border;
    c.text = scheme.text;
    c.textSecondary = scheme.textSecondary;
    c.textDim = scheme.textDim;
    c.accent = readableOn(native ? scheme.accent : palette.accent, scheme.surface, 3.0);
    c.accent2 = readableOn(native ? scheme.accent2 : palette.accent2, scheme.surface, 2.0);
    c.accentHover = scheme.light ? mix(c.accent, black, 0.14) : mix(c.accent, white, 0.2);
    c.accentDeep = mix(c.accent, black, 0.3);
    c.accentTint = mix(scheme.panel, c.accent, scheme.light ? 0.14 : 0.2);
    // White on the accent while it reads well, as on the brand violet; else near-black.
    c.onAccent = contrast(white, c.accent) >= 3.0 ? white : rgb(0x0B0C10);
    return c;
}

bool Theme::isLight()
{
    return schemeFor(g_appearance.theme).light;
}

bool Theme::solidAccentFill()
{
    return !isLight() && OnAccent == QColor(Qt::white);
}

Theme::Appearance Theme::appearance()
{
    return g_appearance;
}

void Theme::setAppearance(const Appearance &requested, bool animate)
{
    const Appearance next = normalized(requested);
    if (g_applied && next == g_appearance)
        return;
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    const QList<Snapshot> snapshots = animate && app && g_applied ? snapshotWindows() : QList<Snapshot>();

    g_appearance = next;
    const Colors c = colors(next);
    g_live = {c.accent, c.accentHover, c.accentDeep, c.accent2, c.accentTint, c.onAccent,
              c.surface, c.panel, c.raised, c.border, c.text, c.textSecondary, c.textDim};
    if (app) {
        g_applied = true;
        QApplication::setPalette(paletteFor(c));
        app->setStyleSheet(styleSheet(next));
    }
    Q_EMIT notifier()->changed();

    for (const Snapshot &snapshot : snapshots) {
        if (snapshot.window)
            new CrossFade(snapshot.window, snapshot.pixmap);
    }
}

Theme::Appearance Theme::savedAppearance()
{
    const QSettings settings(settingsFile(), QSettings::IniFormat);
    const Appearance defaults;
    return normalized({settings.value(QStringLiteral("appearance/theme"), defaults.theme).toString(),
                       settings.value(QStringLiteral("appearance/accent"), defaults.accent).toString(),
                       settings.value(QStringLiteral("appearance/style"), defaults.style).toString()});
}

void Theme::saveAppearance(const Appearance &appearance)
{
    const Appearance known = normalized(appearance);
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("appearance/theme"), known.theme);
    settings.setValue(QStringLiteral("appearance/accent"), known.accent);
    settings.setValue(QStringLiteral("appearance/style"), known.style);
}

QString Theme::styleSheetTemplate()
{
    QFile qss(QStringLiteral(":/skin/top-player.qss"));
    if (!qss.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(qss.readAll());
}

QString Theme::styleSheet(const Appearance &appearance)
{
    const QString &source = strippedTemplate();
    const QHash<QString, QString> values = tokens(normalized(appearance));
    QString result;
    result.reserve(source.size() + source.size() / 4);
    qsizetype from = 0;
    while (true) {
        const qsizetype open = source.indexOf(QLatin1Char('@'), from);
        const qsizetype close = open < 0 ? -1 : source.indexOf(QLatin1Char('@'), open + 1);
        if (close < 0) {
            result += QStringView(source).mid(from);
            break;
        }
        const QString name = source.mid(open + 1, close - open - 1);
        const auto value = values.constFind(name);
        result += QStringView(source).mid(from, open - from);
        if (value != values.constEnd()) {
            result += *value;
            from = close + 1;
        } else {
            qWarning("Theme: unknown skin token @%s@", qPrintable(name));
            result += QLatin1Char('@');
            from = open + 1;
        }
    }
    return result;
}

Theme::Notifier *Theme::notifier()
{
    static Notifier *instance = new Notifier;
    return instance;
}

void Theme::bind(QObject *context, const std::function<void()> &refresh)
{
    refresh();
    QObject::connect(notifier(), &Notifier::changed, context, refresh);
}

void applyDarkSkin(QApplication &app)
{
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    Q_UNUSED(app);
    // Applied even when the saved appearance is the default one.
    g_applied = false;
    Theme::setAppearance(Theme::savedAppearance());
}

QToolButton *Theme::barButton(QWidget *parent, const QString &text, const QString &toolTip, const char *objectName)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setProperty("barButton", true);
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    // Set here rather than in the skin, so the size hint fits the text.
    QFont font = button->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 0.85);
    button->setFont(font);
    // The skin's padding is not part of a text button's size hint, and the
    // labels must stay readable however narrow the drawer is.
    button->setMinimumWidth(QFontMetrics(font).horizontalAdvance(text) + 16);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return button;
}
