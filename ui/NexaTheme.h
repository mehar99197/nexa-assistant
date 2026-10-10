#pragma once

#include "ui/NexaCoreSpec.h"

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

class QApplication;

namespace nexa {

enum class ThemeScheme { Dark, Light };

// The design's tokens: five neutrals and the nine state colours, for one scheme.
struct ThemeTokens {
    QColor ground;          // the window
    QColor panel;           // pills, bubbles, the input line, the status bar
    QColor panelRaised;     // a hovered panel
    QColor line;            // borders
    QColor text;
    QColor muted;
    QColor idle;
    QColor wake;
    QColor listen;
    QColor stt;
    QColor think;
    QColor act;
    QColor speak;
    QColor fast;
    QColor error;
    QColor onAccent;        // text on a teal button

    QColor stateColor(CoreMode mode) const;
};

// Nexa's look (Facade over the stylesheet and the palette). theme.qss names which widget wears
// which token; this fills the tokens in for the scheme and applies both the sheet and a matching
// QPalette, so widgets the sheet does not mention (dialog buttons, check boxes) agree with it.
class NexaTheme {
public:
    static ThemeTokens tokens(ThemeScheme scheme);
    // "dark" or "light" as set; anything else follows the desktop's own setting.
    static ThemeScheme schemeFor(const QString &setting);
    static QString schemeKey(ThemeScheme scheme);          // "dark" | "light"
    static QString monoFamily();                           // the system's fixed-pitch font
    // `base` scaled by `factor`, in points or in pixels — Android's fonts carry no point size.
    static QFont scaled(const QFont &base, double factor, const QString &family = {});
    static QString styleSheet(ThemeScheme scheme);         // theme.qss with every token filled in
    static QPalette palette(ThemeScheme scheme);
    static void apply(QApplication *app, ThemeScheme scheme);
};

} // namespace nexa
