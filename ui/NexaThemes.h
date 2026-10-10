#pragma once

#include "ui/NexaCoreSpec.h"
#include "ui/NexaTheme.h"

#include <QList>
#include <QString>

#include <optional>

namespace nexa {

// One theme: a colour family in a scheme, with the motion style it wears.
struct ThemePack {
    QString id;             // "ocean-dark"
    QString name;           // "Ocean · Dark"
    QString family;         // "Ocean"
    ThemeScheme scheme{ThemeScheme::Dark};
    CoreStyle style{CoreStyle::Classic};
    ThemeTokens tokens;
};

// The catalogue of themes (Registry): the design's own Nexa pair, and twenty-five more colour
// families in dark and light, each family with one of the eight motion styles — all from the
// same tokens and the same stylesheet, so nothing else in Nexa knows which one is on.
class NexaThemes {
public:
    static const QList<ThemePack> &all();                       // 52 of them, generated once
    static std::optional<ThemePack> find(const QString &id);
    // What config.ui.theme means: "system" (Nexa, as the desktop is), "dark", "light", or a pack id.
    static ThemePack resolve(const QString &setting);
    static QString settingFor(const ThemePack &pack);           // the id, or dark/light for Nexa itself
    static QString siblingId(const ThemePack &pack);            // the same family in the other scheme
    static QStringList families();
};

} // namespace nexa
