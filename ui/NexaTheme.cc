#include "ui/NexaTheme.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStyleFactory>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QColor withAlpha(QColor colour, double alpha) {
    colour.setAlphaF(alpha);
    return colour;
}

QString css(const QColor &colour) {
    return colour.alpha() == 255 ? colour.name(QColor::HexRgb) : colour.name(QColor::HexArgb);
}

} // namespace

QColor ThemeTokens::stateColor(CoreMode mode) const {
    switch (mode) {
    case CoreMode::Idle: return idle;
    case CoreMode::WakeWord: return wake;
    case CoreMode::Listening: return listen;
    case CoreMode::Transcribing: return stt;
    case CoreMode::Thinking: return think;
    case CoreMode::Acting: return act;
    case CoreMode::Speaking: return speak;
    case CoreMode::FastPath: return fast;
    case CoreMode::Offline: return error;
    }
    return idle;
}

ThemeTokens NexaTheme::tokens(ThemeScheme scheme) {
    if (scheme == ThemeScheme::Light) {
        return ThemeTokens{.ground = QColor(u"#F6F8FA"_s),
                           .panel = QColor(u"#FFFFFF"_s),
                           .panelRaised = QColor(u"#EEF2F5"_s),
                           .line = QColor(u"#D9E0E7"_s),
                           .text = QColor(u"#141A21"_s),
                           .muted = QColor(u"#5F6B78"_s),
                           .idle = QColor(u"#2A9C95"_s),
                           .wake = QColor(u"#2E9E57"_s),
                           .listen = QColor(u"#1AA99B"_s),
                           .stt = QColor(u"#4F6FD8"_s),
                           .think = QColor(u"#7A5FD1"_s),
                           .act = QColor(u"#C4860F"_s),
                           .speak = QColor(u"#1F9E70"_s),
                           .fast = QColor(u"#0E8F84"_s),
                           .error = QColor(u"#D9483B"_s),
                           .onAccent = QColor(u"#06201C"_s)};
    }
    return ThemeTokens{.ground = QColor(u"#0D1014"_s),
                       .panel = QColor(u"#151A21"_s),
                       .panelRaised = QColor(u"#1B222B"_s),
                       .line = QColor(u"#2A323D"_s),
                       .text = QColor(u"#E7ECF2"_s),
                       .muted = QColor(u"#98A4B3"_s),
                       .idle = QColor(u"#46B7B0"_s),
                       .wake = QColor(u"#6FD38A"_s),
                       .listen = QColor(u"#5BE3D6"_s),
                       .stt = QColor(u"#9DB4FF"_s),
                       .think = QColor(u"#B39DFF"_s),
                       .act = QColor(u"#F0B24B"_s),
                       .speak = QColor(u"#7BE8C0"_s),
                       .fast = QColor(u"#E6FFFB"_s),
                       .error = QColor(u"#FF7A66"_s),
                       .onAccent = QColor(u"#06201C"_s)};
}

ThemeScheme NexaTheme::schemeFor(const QString &setting) {
    if (setting == u"dark"_s) return ThemeScheme::Dark;
    if (setting == u"light"_s) return ThemeScheme::Light;
    const QStyleHints *hints = QGuiApplication::styleHints();
    return hints && hints->colorScheme() == Qt::ColorScheme::Light ? ThemeScheme::Light : ThemeScheme::Dark;
}

QString NexaTheme::schemeKey(ThemeScheme scheme) {
    return scheme == ThemeScheme::Light ? u"light"_s : u"dark"_s;
}

QString NexaTheme::monoFamily() {
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

QFont NexaTheme::scaled(const QFont &base, double factor, const QString &family) {
    QFont font = base;
    if (!family.isEmpty()) font.setFamily(family);
    if (base.pointSizeF() > 0) {
        font.setPointSizeF(base.pointSizeF() * factor);
    } else if (base.pixelSize() > 0) {
        font.setPixelSize(std::max(6, int(std::lround(base.pixelSize() * factor))));
    }
    return font;
}

QString NexaTheme::styleSheet(ThemeScheme scheme) {
    return styleSheet(tokens(scheme), CoreStyle::Classic);
}

QString NexaTheme::styleSheet(const ThemeTokens &t, CoreStyle style) {
    return styleSheet(t, style, NexaDesigns::resolve(u"studio"_s));
}

QString NexaTheme::styleSheet(const ThemeTokens &t, CoreStyle style, const DesignSpec &design,
                              const ThemeChrome &chrome) {
    QFile file(u":/nexa/ui/theme.qss"_s);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QString sheet = QString::fromUtf8(file.readAll());
    const int basePoints = QFontDatabase::systemFont(QFontDatabase::GeneralFont).pointSize();
    const QString chatFont = design.font == FontFace::Mono ? monoFamily() : NexaDesigns::fontFamily(design.font);
    // In the Flat chrome the sheet draws the panels; in any other the panel paints itself (ChromeFrame), so the
    // sheet leaves it bare.
    const bool flat = chrome.panels == PanelChrome::Flat;
    const std::pair<QString, QString> values[] = {
        {u"coreStyle"_s, QString::number(static_cast<int>(style))},
        {u"coreShape"_s, QString::number(static_cast<int>(design.shape))},
        {u"bubbleStyle"_s, QString::number(static_cast<int>(design.bubbles))},
        {u"panelChrome"_s, QString::number(static_cast<int>(chrome.panels))},
        {u"backdrop"_s, QString::number(static_cast<int>(chrome.backdrop))},
        {u"panelFill"_s, flat ? css(t.panel) : u"transparent"_s},
        {u"panelBorder"_s, flat ? u"1px solid %1"_s.arg(css(t.line)) : u"0"_s},
        {u"radius"_s, QString::number(design.radius)},
        {u"radiusSmall"_s, QString::number(std::max(0, int(std::lround(design.radius * 0.75))))},
        {u"font"_s, NexaDesigns::fontFamily(design.font)},
        {u"chatFont"_s, chatFont},
        {u"fontSize"_s, QString::number(std::max(8, (basePoints > 0 ? basePoints : 10) + design.fontPoints))},
        {u"ground"_s, css(t.ground)},
        {u"panel"_s, css(t.panel)},
        {u"panel2"_s, css(t.panelRaised)},
        {u"line"_s, css(t.line)},
        {u"text"_s, css(t.text)},
        {u"muted"_s, css(t.muted)},
        {u"idle"_s, css(t.idle)},
        {u"wake"_s, css(t.wake)},
        {u"listen"_s, css(t.listen)},
        {u"stt"_s, css(t.stt)},
        {u"think"_s, css(t.think)},
        {u"act"_s, css(t.act)},
        {u"speak"_s, css(t.speak)},
        {u"fast"_s, css(t.fast)},
        {u"error"_s, css(t.error)},
        {u"onAccent"_s, css(t.onAccent)},
        {u"accentSoft"_s, css(withAlpha(t.idle, 0.18))},
        {u"accentLine"_s, css(withAlpha(t.idle, 0.55))},
        {u"errorSoft"_s, css(withAlpha(t.error, 0.16))},
        {u"errorSoft2"_s, css(withAlpha(t.error, 0.28))},
        {u"errorLine"_s, css(withAlpha(t.error, 0.5))},
        {u"mono"_s, monoFamily()},
    };
    for (const auto &[name, value] : values) sheet.replace(u'@' + name + u'@', value);
    return sheet;
}

QPalette NexaTheme::palette(ThemeScheme scheme) {
    return palette(tokens(scheme));
}

QPalette NexaTheme::palette(const ThemeTokens &t) {
    QPalette p;
    p.setColor(QPalette::Window, t.ground);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.panel);
    p.setColor(QPalette::AlternateBase, t.ground);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.text);
    p.setColor(QPalette::Highlight, t.idle);
    p.setColor(QPalette::HighlightedText, t.onAccent);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::Link, t.idle);
    p.setColor(QPalette::LinkVisited, t.think);
    p.setColor(QPalette::Light, t.panelRaised);
    p.setColor(QPalette::Midlight, t.line);
    p.setColor(QPalette::Mid, t.line);
    p.setColor(QPalette::Dark, t.line);
    p.setColor(QPalette::Shadow, t.ground);
    p.setColor(QPalette::Disabled, QPalette::Text, t.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.muted);
    return p;
}

void NexaTheme::apply(QApplication *app, ThemeScheme scheme) {
    apply(app, tokens(scheme), CoreStyle::Classic);
}

void NexaTheme::apply(QApplication *app, const ThemeTokens &tokens, CoreStyle style) {
    apply(app, tokens, style, NexaDesigns::resolve(u"studio"_s));
}

void NexaTheme::apply(QApplication *app, const ThemeTokens &tokens, CoreStyle style, const DesignSpec &design,
                      const ThemeChrome &chrome) {
    // Fusion draws the same everywhere and leaves the look to the sheet and the palette; the
    // platform style would fight both.
    static bool styled = false;
    if (!styled) {
        if (QStyle *fusion = QStyleFactory::create(u"Fusion"_s)) app->setStyle(fusion);
        styled = true;
    }
    app->setPalette(palette(tokens));
    app->setStyleSheet(styleSheet(tokens, style, design, chrome));
}

} // namespace nexa
