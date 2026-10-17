#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QApplication;
class QToolButton;
class QWidget;

// Top Player's appearance: a theme (the surfaces and text), an accent palette
// and a visual style, combined at runtime into the QSS skin
// (resources/skin/top-player.qss, a template with @token@ placeholders), the
// application palette and the colors below.
namespace Theme {

// The live colors of the current appearance, for widgets that paint
// themselves. They change when the appearance does: read them while
// painting, or refresh through bind(), instead of copying them once.
extern const QColor &Accent;        // brand violet by default
extern const QColor &AccentHover;
extern const QColor &AccentDeep;
extern const QColor &Accent2;       // the second color of accent gradients
extern const QColor &AccentTint;    // an opaque accent wash over Panel
extern const QColor &OnAccent;      // text and icons drawn on the accent
extern const QColor &Surface;       // window background
extern const QColor &Panel;         // drawer, menus, dialogs
extern const QColor &Raised;
extern const QColor &Border;
extern const QColor &TextPrimary;
extern const QColor &TextSecondary;
extern const QColor &TextDim;

// The fixed brand colors (logo, About), whatever the appearance.
inline constexpr QColor BrandViolet{0x7C, 0x5C, 0xFF};
inline constexpr QColor BrandMagenta{0xFF, 0x4F, 0xD8};

// "#rrggbb", for rich text.
inline QString hex(const QColor &color) { return color.name(QColor::HexRgb); }

// `color` with alpha `alpha` (0..1).
QColor withAlpha(const QColor &color, qreal alpha);
// `amount` (0..1) of the way from `a` to `b`.
QColor mix(const QColor &a, const QColor &b, qreal amount);

// A background scheme, after Omarchy's themes.
struct Scheme {
    QString id;
    QString name;
    bool light = false;
    QColor surface, panel, raised, border;
    QColor text, textSecondary, textDim;
    QColor accent, accent2; // the theme's own accent ("Theme Native")
};

// An accent palette; "native" takes the theme's own accent.
struct AccentPalette {
    QString id;
    QString name;
    QColor accent, accent2;
};

// A visual style: the shapes and effects laid over the colors.
struct Style {
    QString id;
    QString name;
    QString description;
    int radius = 6;       // buttons and inputs
    int radiusSmall = 4;  // chips, menu items, check boxes
    int radiusLarge = 10; // cards and frames
    bool gradients = true; // accent fills blend into Accent2
    bool glow = true;      // soft accent glow on the seekbar and focus
    bool mono = false;     // monospace headings
    bool softBorders = false; // borders fade into the surfaces
};

const QList<Scheme> &schemes();
const QList<AccentPalette> &accents();
const QList<Style> &styles();

// What the user picked; ids from the lists above. Unknown ids fall back to
// the defaults.
struct Appearance {
    QString theme = QStringLiteral("midnight");
    QString accent = QStringLiteral("violet");
    QString style = QStringLiteral("glass");

    bool operator==(const Appearance &other) const
    {
        return theme == other.theme && accent == other.accent && style == other.style;
    }
    bool operator!=(const Appearance &other) const { return !(*this == other); }
};

// The colors an appearance works out to: the theme's surfaces, and the accent
// made readable on them, with its shades.
struct Colors {
    bool light = false;
    QColor accent, accentHover, accentDeep, accent2, accentTint, onAccent;
    QColor surface, panel, raised, border, text, textSecondary, textDim;
};
Colors colors(const Appearance &appearance);

Appearance appearance();
// Whether the current theme is a light one.
bool isLight();
// Whether accent-filled controls (the play button) take the solid accent:
// on dark themes, when white reads on the accent. Else they take a wash of it.
bool solidAccentFill();
// Applies `appearance` to the whole app at once: colors, palette and skin.
// With `animate`, open windows cross-fade from their old look.
void setAppearance(const Appearance &appearance, bool animate = false);
// [appearance] in settings.ini.
Appearance savedAppearance();
void saveAppearance(const Appearance &appearance);

// The skin for `appearance`, with every @token@ of the template filled in.
QString styleSheet(const Appearance &appearance);
// The QSS template as shipped, tokens and all.
QString styleSheetTemplate();

// Emits changed() after the appearance changed and the colors above hold
// the new values.
class Notifier : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

Q_SIGNALS:
    void changed();
};
Notifier *notifier();
// Calls `refresh` now and after every appearance change, while `context`
// lives: for colors baked into rich text or a widget's own stylesheet.
void bind(QObject *context, const std::function<void()> &refresh);

// A text button for the bars along the bottom of the playlist and library
// ("ADD", "DEL", ...), styled as chips by the skin.
QToolButton *barButton(QWidget *parent, const QString &text, const QString &toolTip, const char *objectName);

} // namespace Theme

// Applies the Top Player skin with the saved appearance: Fusion style with a
// matching palette (so standard dialogs match) plus the QSS skin.
void applyDarkSkin(QApplication &app);
