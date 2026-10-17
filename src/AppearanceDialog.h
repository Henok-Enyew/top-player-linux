#pragma once

#include "Theme.h"

#include <QDialog>
#include <QIcon>

class QButtonGroup;
class QLabel;

// Appearance settings: the themes as painted previews, the accent palettes
// as swatches and the visual styles. A choice previews live on the whole
// app; Done keeps it, Cancel goes back to the appearance the dialog opened with.
class AppearanceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AppearanceDialog(QWidget *parent = nullptr);

    // Small icons for menus: a theme's surface with its accent, an accent's gradient.
    static QIcon themeSwatch(const Theme::Scheme &scheme);
    static QIcon accentSwatch(const Theme::AccentPalette &accent);

    void accept() override;
    void reject() override;

protected:
    void showEvent(QShowEvent *event) override;

private:
    void choose(const Theme::Appearance &appearance);
    // Checks the buttons of the current appearance.
    void syncChecks();

    Theme::Appearance m_original;
    QButtonGroup *m_themes;
    QButtonGroup *m_accents;
    QButtonGroup *m_styles;
    QLabel *m_accentName;
};
