#pragma once

#include "ui/NexaCoreSpec.h"
#include "ui/NexaTheme.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace nexa {

// One theme pack: a palette (the tokens theme.qss takes) with the core's motion style that suits it, named,
// in a category. Every pack is dark — the owner wants no light look — so a pack differs from the next in
// its accent, how its ground is tinted and how deep it is, how far the state colours wander from the accent
// and how bright they glow, and the motion of its core.
struct ThemePack {
    QString id;          // stable, kebab-case: "neon-cyan"; config.ui.theme holds it
    QString name;        // "Neon Cyan"
    QString category;    // "Neon", "Nature", … — the gallery's chips
    CoreStyle style;
    ThemeTokens tokens;
};

// The catalogue of theme packs (Factory): sixty-odd dark packs in ten categories, each written as a recipe
// (an accent, a ground, a spread, a glow, a motion style) and generated with the design's own shape of
// palette and its contrast guaranteed — text 7:1 on the panels, muted text 3.5:1 and every state colour 3:1
// on the ground. Nexa's own pack and Mission Control's are hand-tuned. Old settings keep working: the
// catalogue's former `<family>-dark` and `<family>-light` ids, "system", "dark" and "light" all resolve to a
// pack, and so does anything unknown.
class NexaThemes {
public:
    static const QList<ThemePack> &all();
    static std::optional<ThemePack> find(const QString &id);
    // The pack for a setting: an id, an old id, "system" / "dark" / "light" (Nexa's own look), or anything.
    static ThemePack resolve(const QString &setting);
    // What config.ui.theme holds for a pack.
    static QString settingFor(const ThemePack &pack);
    // The categories in the catalogue's order, and the packs of one.
    static QStringList categories();
    static QList<ThemePack> inCategory(const QString &category);
    // The pack after this one in its category, round and round: the toolbar button steps through them.
    static ThemePack nextInCategory(const ThemePack &pack);
    // The id Nexa falls back to.
    static QString defaultId();
};

} // namespace nexa
