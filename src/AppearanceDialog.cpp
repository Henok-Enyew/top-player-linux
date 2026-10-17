#include "AppearanceDialog.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRadialGradient>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kThemeColumns = 5;
constexpr QSize kThemeCardSize(148, 106);
constexpr int kPreviewHeight = 80;
constexpr int kChipSize = 34;
constexpr QSize kStyleCardSize(236, 112);

// The current appearance with one part swapped.
Theme::Appearance withTheme(const QString &id)
{
    Theme::Appearance result = Theme::appearance();
    result.theme = id;
    return result;
}

Theme::Appearance withAccent(const QString &id)
{
    Theme::Appearance result = Theme::appearance();
    result.accent = id;
    return result;
}

Theme::Appearance withStyle(const QString &id)
{
    Theme::Appearance result = Theme::appearance();
    result.style = id;
    return result;
}

Theme::Style currentStyle()
{
    for (const Theme::Style &style : Theme::styles()) {
        if (style.id == Theme::appearance().style)
            return style;
    }
    return Theme::styles().constFirst();
}

QLinearGradient accentGradient(const QRectF &rect, const QColor &from, const QColor &to)
{
    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0, from);
    gradient.setColorAt(1, to);
    return gradient;
}

// The selection ring around a card: the accent gradient when checked, a
// hairline otherwise.
void drawRing(QPainter &p, const QRectF &rect, qreal radius, bool checked, bool hovered, bool focused)
{
    p.setBrush(Qt::NoBrush);
    if (checked) {
        p.setPen(QPen(QBrush(accentGradient(rect, Theme::Accent, Theme::Accent2)), 2));
        p.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius);
        return;
    }
    const QColor color = hovered || focused ? Theme::AccentHover : Theme::Border;
    p.setPen(QPen(color, 1));
    p.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

// A checkable card that repaints as the pointer comes and goes.
class Card : public QAbstractButton
{
public:
    Card(const QString &objectName, const QString &toolTip, QWidget *parent)
        : QAbstractButton(parent)
    {
        setObjectName(objectName);
        setToolTip(toolTip);
        setAccessibleName(toolTip);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
    }

protected:
    bool hovered() const { return underMouse(); }
    qreal cardRadius() const { return std::clamp(currentStyle().radiusLarge, 3, 14); }
};

// A theme as a tiny player window: title bar, drawer, cover, controls.
class ThemeCard : public Card
{
public:
    ThemeCard(const Theme::Scheme &scheme, QWidget *parent)
        : Card(QStringLiteral("Theme-") + scheme.id, scheme.name, parent)
        , m_scheme(scheme)
    {
        setFixedSize(kThemeCardSize);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Theme::Colors c = Theme::colors(withTheme(m_scheme.id));
        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        const qreal radius = cardRadius();
        QPainterPath shape;
        shape.addRoundedRect(card, radius, radius);

        p.save();
        p.setClipPath(shape);
        p.fillRect(card, Theme::Panel);
        const QRectF preview(card.left(), card.top(), card.width(), kPreviewHeight);
        p.fillRect(preview, c.surface);
        // Ambient accent glow, as in the app's empty state.
        QRadialGradient glow(preview.center() - QPointF(18, 4), preview.width() * 0.5);
        glow.setColorAt(0, Theme::withAlpha(c.accent, c.light ? 0.14 : 0.22));
        glow.setColorAt(1, Theme::withAlpha(c.accent, 0));
        p.fillRect(preview, glow);
        // Title bar with window dots.
        p.fillRect(QRectF(preview.left(), preview.top(), preview.width(), 12), c.panel);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            p.setBrush(i == 2 ? c.accent : c.textDim);
            p.drawEllipse(QPointF(preview.right() - 8 - i * 7, preview.top() + 6), 2, 2);
        }
        // Drawer with rows, the playing one marked.
        const QRectF drawer(preview.right() - 42, preview.top() + 12, 42, preview.height() - 26);
        p.fillRect(drawer, c.panel);
        p.fillRect(QRectF(drawer.left(), drawer.top(), 1, drawer.height()), c.border);
        for (int i = 0; i < 4; ++i) {
            const QRectF row(drawer.left() + 6, drawer.top() + 7 + i * 11, i == 1 ? 26 : 30 - i * 3, 3);
            if (i == 1) {
                p.fillRect(QRectF(drawer.left() + 1, row.top() - 3, drawer.width() - 1, 9), c.accentTint);
                p.fillRect(QRectF(drawer.left() + 1, row.top() - 3, 2, 9), c.accent);
            }
            p.setBrush(i == 1 ? c.accent : c.textSecondary);
            p.drawRoundedRect(row, 1.5, 1.5);
        }
        // Cover and song title.
        const QRectF cover(preview.left() + 10, preview.top() + 20, 30, 30);
        p.setBrush(accentGradient(cover, c.accent, c.accent2));
        p.drawRoundedRect(cover, 4, 4);
        p.setBrush(c.onAccent);
        p.drawPath([&cover] {
            QPainterPath play;
            play.addPolygon(QPolygonF({cover.center() + QPointF(-4, -6), cover.center() + QPointF(7, 0),
                                       cover.center() + QPointF(-4, 6)}));
            play.closeSubpath();
            return play;
        }());
        p.setBrush(c.text);
        p.drawRoundedRect(QRectF(cover.right() + 8, cover.top() + 7, 44, 4), 2, 2);
        p.setBrush(c.textSecondary);
        p.drawRoundedRect(QRectF(cover.right() + 8, cover.top() + 17, 30, 3), 1.5, 1.5);
        // Control bar with the progress.
        const QRectF controls(preview.left(), preview.bottom() - 14, preview.width(), 14);
        p.fillRect(controls, c.panel);
        p.fillRect(QRectF(controls.left(), controls.top(), controls.width(), 1), c.border);
        const QRectF groove(controls.left() + 8, controls.center().y() - 1.5, controls.width() - 16, 3);
        p.setBrush(c.border);
        p.drawRoundedRect(groove, 1.5, 1.5);
        const QRectF progress(groove.left(), groove.top(), groove.width() * 0.62, groove.height());
        p.setBrush(accentGradient(QRectF(groove.topLeft(), groove.bottomRight()), c.accent, c.accent2));
        p.drawRoundedRect(progress, 1.5, 1.5);
        p.restore();

        // The name, in the dialog's own colors.
        QFont font = this->font();
        font.setBold(isChecked());
        p.setFont(font);
        p.setPen(isChecked() ? Theme::TextPrimary : Theme::TextSecondary);
        const QRectF label(card.left() + 10, preview.bottom(), card.width() - 20, card.bottom() - preview.bottom());
        p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(font).elidedText(m_scheme.name, Qt::ElideRight, int(label.width())));
        drawRing(p, card, radius, isChecked(), hovered(), hasFocus());
    }

private:
    Theme::Scheme m_scheme;
};

// A round swatch of an accent palette, as it shows on the current theme.
class AccentChip : public Card
{
public:
    AccentChip(const Theme::AccentPalette &accent, QWidget *parent)
        : Card(QStringLiteral("Accent-") + accent.id, accent.name, parent)
        , m_id(accent.id)
    {
        setFixedSize(kChipSize, kChipSize);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Theme::Colors c = Theme::colors(withAccent(m_id));
        const QRectF outer = QRectF(rect()).adjusted(1, 1, -1, -1);
        const QRectF swatch = outer.adjusted(5, 5, -5, -5);
        if (isChecked() || hovered() || hasFocus()) {
            p.setPen(QPen(isChecked() ? Theme::TextPrimary : Theme::TextDim, isChecked() ? 2 : 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(outer.adjusted(1, 1, -1, -1));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(accentGradient(swatch, c.accent, c.accent2));
        p.drawEllipse(swatch);
        // The theme's own accent is marked with a small ring of the surface.
        if (m_id == QLatin1String("native")) {
            p.setPen(QPen(c.surface, 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(swatch.center(), 3.5, 3.5);
        }
    }

private:
    QString m_id;
};

// A visual style: a sample button and progress drawn in that style.
class StyleCard : public Card
{
public:
    StyleCard(const Theme::Style &style, QWidget *parent)
        : Card(QStringLiteral("Style-") + style.id, style.name, parent)
        , m_style(style)
    {
        setFixedSize(kStyleCardSize);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        const qreal radius = std::clamp(m_style.radiusLarge, 3, 14);
        QPainterPath shape;
        shape.addRoundedRect(card, radius, radius);
        if (m_style.gradients)
            p.fillPath(shape, accentGradient(card, Theme::mix(Theme::Panel, Theme::Accent, 0.08), Theme::Panel));
        else
            p.fillPath(shape, Theme::Panel);

        // The sample: a play button and a progress bar.
        const QRectF button(card.left() + 14, card.top() + 16, 30, 30);
        const qreal buttonRadius = m_style.mono ? m_style.radiusSmall : 15;
        p.setPen(m_style.mono ? QPen(Theme::Accent, 1) : Qt::NoPen);
        if (m_style.mono)
            p.setBrush(Qt::NoBrush);
        else if (m_style.gradients)
            p.setBrush(accentGradient(button, Theme::Accent, Theme::Accent2));
        else
            p.setBrush(Theme::Accent);
        p.drawRoundedRect(button, buttonRadius, buttonRadius);
        p.setPen(Qt::NoPen);
        p.setBrush(m_style.mono ? Theme::Accent : Theme::OnAccent);
        QPainterPath play;
        play.addPolygon(QPolygonF({button.center() + QPointF(-4, -6), button.center() + QPointF(7, 0),
                                   button.center() + QPointF(-4, 6)}));
        play.closeSubpath();
        p.drawPath(play);
        const QRectF groove(button.right() + 12, button.center().y() - 2, card.right() - button.right() - 26, 4);
        p.setBrush(Theme::Border);
        p.drawRoundedRect(groove, 2, 2);
        const QRectF progress(groove.left(), groove.top(), groove.width() * 0.58, groove.height());
        if (m_style.glow) {
            p.setBrush(Theme::withAlpha(Theme::Accent, 0.25));
            p.drawRoundedRect(progress.adjusted(-2, -2, 2, 2), 4, 4);
        }
        if (m_style.gradients)
            p.setBrush(accentGradient(QRectF(groove.topLeft(), groove.bottomRight()), Theme::Accent, Theme::Accent2));
        else
            p.setBrush(Theme::Accent);
        p.drawRoundedRect(progress, 2, 2);

        // Name and description.
        QFont name = font();
        name.setBold(true);
        if (m_style.mono)
            name.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("monospace")});
        p.setFont(name);
        p.setPen(isChecked() ? Theme::TextPrimary : Theme::TextSecondary);
        const QRectF text(card.left() + 14, button.bottom() + 8, card.width() - 28, card.bottom() - button.bottom() - 12);
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop, m_style.name);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.88);
        p.setFont(small);
        p.setPen(Theme::TextDim);
        const int nameHeight = QFontMetrics(name).height();
        p.drawText(text.adjusted(0, nameHeight, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   QFontMetrics(small).elidedText(m_style.description, Qt::ElideRight, int(text.width()) * 2));
        drawRing(p, card, radius, isChecked(), hovered(), hasFocus());
    }

private:
    Theme::Style m_style;
};

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("section"));
    return label;
}

} // namespace

AppearanceDialog::AppearanceDialog(QWidget *parent)
    : QDialog(parent)
    , m_themes(new QButtonGroup(this))
    , m_accents(new QButtonGroup(this))
    , m_styles(new QButtonGroup(this))
    , m_accentName(new QLabel(this))
{
    setObjectName(QStringLiteral("AppearanceDialog"));
    setWindowTitle(tr("Appearance"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 18);
    layout->setSpacing(8);

    auto *heading = new QLabel(tr("Appearance"), this);
    heading->setObjectName(QStringLiteral("AppearanceHeading"));
    auto *subheading = new QLabel(tr("Pick a theme, an accent and a style. Every choice previews live."), this);
    subheading->setObjectName(QStringLiteral("AppearanceSubheading"));
    layout->addWidget(heading);
    layout->addWidget(subheading);
    layout->addSpacing(10);

    layout->addWidget(sectionLabel(tr("THEME"), this));
    auto *themes = new QGridLayout;
    themes->setSpacing(10);
    for (int i = 0; i < Theme::schemes().size(); ++i) {
        const Theme::Scheme &scheme = Theme::schemes()[i];
        auto *card = new ThemeCard(scheme, this);
        m_themes->addButton(card, i);
        themes->addWidget(card, i / kThemeColumns, i % kThemeColumns);
        connect(card, &QAbstractButton::clicked, this, [this, id = scheme.id] { choose(withTheme(id)); });
    }
    layout->addLayout(themes);
    layout->addSpacing(10);

    layout->addWidget(sectionLabel(tr("ACCENT"), this));
    auto *accents = new QHBoxLayout;
    accents->setSpacing(4);
    for (int i = 0; i < Theme::accents().size(); ++i) {
        const Theme::AccentPalette &accent = Theme::accents()[i];
        auto *chip = new AccentChip(accent, this);
        m_accents->addButton(chip, i);
        accents->addWidget(chip);
        connect(chip, &QAbstractButton::clicked, this, [this, id = accent.id] { choose(withAccent(id)); });
    }
    accents->addSpacing(10);
    m_accentName->setObjectName(QStringLiteral("AccentName"));
    accents->addWidget(m_accentName);
    accents->addStretch();
    layout->addLayout(accents);
    layout->addSpacing(10);

    layout->addWidget(sectionLabel(tr("STYLE"), this));
    auto *styles = new QHBoxLayout;
    styles->setSpacing(10);
    for (int i = 0; i < Theme::styles().size(); ++i) {
        const Theme::Style &style = Theme::styles()[i];
        auto *card = new StyleCard(style, this);
        m_styles->addButton(card, i);
        styles->addWidget(card);
        connect(card, &QAbstractButton::clicked, this, [this, id = style.id] { choose(withStyle(id)); });
    }
    styles->addStretch();
    layout->addLayout(styles);
    layout->addSpacing(14);

    auto *buttons = new QHBoxLayout;
    auto *reset = new QPushButton(tr("Reset to Default"), this);
    reset->setObjectName(QStringLiteral("AppearanceReset"));
    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("AppearanceCancel"));
    auto *done = new QPushButton(tr("Done"), this);
    done->setObjectName(QStringLiteral("AppearanceDone"));
    done->setDefault(true);
    // Keyboard focus starts on Done rather than ringing the first theme card.
    done->setFocus();
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(done);
    layout->addLayout(buttons);

    connect(reset, &QPushButton::clicked, this, [this] { choose(Theme::Appearance()); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    // Also follows changes made elsewhere, e.g. from the menu.
    connect(Theme::notifier(), &Theme::Notifier::changed, this, &AppearanceDialog::syncChecks);
    syncChecks();
}

QIcon AppearanceDialog::themeSwatch(const Theme::Scheme &scheme)
{
    constexpr int kSize = 16;
    QPixmap pixmap(QSize(kSize, kSize) * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const Theme::Colors c = Theme::colors(withTheme(scheme.id));
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.surface);
    p.drawRoundedRect(QRectF(0.5, 0.5, kSize - 1, kSize - 1), 4, 4);
    p.setPen(Qt::NoPen);
    p.setBrush(accentGradient(QRectF(4, 4, 8, 8), c.accent, c.accent2));
    p.drawEllipse(QRectF(4, 4, 8, 8));
    return QIcon(pixmap);
}

QIcon AppearanceDialog::accentSwatch(const Theme::AccentPalette &accent)
{
    constexpr int kSize = 16;
    QPixmap pixmap(QSize(kSize, kSize) * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const Theme::Colors c = Theme::colors(withAccent(accent.id));
    p.setPen(Qt::NoPen);
    p.setBrush(accentGradient(QRectF(1, 1, kSize - 2, kSize - 2), c.accent, c.accent2));
    p.drawEllipse(QRectF(1, 1, kSize - 2, kSize - 2));
    return QIcon(pixmap);
}

void AppearanceDialog::accept()
{
    Theme::saveAppearance(Theme::appearance());
    QDialog::accept();
}

void AppearanceDialog::reject()
{
    Theme::setAppearance(m_original, true);
    QDialog::reject();
}

void AppearanceDialog::showEvent(QShowEvent *event)
{
    m_original = Theme::appearance();
    syncChecks();
    QDialog::showEvent(event);
}

void AppearanceDialog::choose(const Theme::Appearance &appearance)
{
    Theme::setAppearance(appearance, true);
}

void AppearanceDialog::syncChecks()
{
    const Theme::Appearance current = Theme::appearance();
    auto check = [](QButtonGroup *group, const QString &objectName) {
        for (QAbstractButton *button : group->buttons()) {
            if (button->objectName() == objectName)
                button->setChecked(true);
        }
    };
    check(m_themes, QStringLiteral("Theme-") + current.theme);
    check(m_accents, QStringLiteral("Accent-") + current.accent);
    check(m_styles, QStringLiteral("Style-") + current.style);
    for (const Theme::AccentPalette &accent : Theme::accents()) {
        if (accent.id == current.accent)
            m_accentName->setText(accent.name);
    }
}
