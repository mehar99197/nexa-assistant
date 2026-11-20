#include "core/mission/RoleClassifier.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

const QRegularExpression &pattern(AgentRole role) {
    constexpr auto options = QRegularExpression::CaseInsensitiveOption;
    static const QRegularExpression phone(
        uR"(\b(?:on|from|to|in) (?:my|the) (?:phone|mobile|android)\b|\bmy phone\b|\bandroid\b|\bsms\b|\btext messages?\b)"_s,
        options);
    static const QRegularExpression screen(
        uR"(\b(?:my|the|this) screen\b|\bscreenshot\b|\bsee this\b|\blook at (?:this|that)\b|\bclick (?:on |the |that )|)"
        uR"(\bbuttons?\b|\bdialog\b|\bwindows?\b(?! ?(?:10|11|pc|laptop))|\btype (?:into|in) )"_s,
        options);
    static const QRegularExpression web(
        uR"(https?://|\bwww\.|\b[a-z0-9-]+\.(?:com|org|net|io|pk|edu|gov|dev|ai|co|uk|in|me|app)\b|\bwebsites?\b|)"
        uR"(\bweb ?pages?\b|\bbrows(?:e|er|ing)\b|\bgoogle\b|\bsearch (?:the )?(?:web|online|internet)\b|\bonline\b|)"
        uR"(\byoutube\b|\bgithub\b|\bamazon\b|\blog ?in (?:to|on)\b|\bsign (?:up|in) (?:to|on|at)\b|)"
        uR"(\bfill (?:in |out )?(?:the |a |this |that )?forms?\b|\binbox\b|\be-?mails?\b|\bmail\b|\bgmail\b)"_s,
        options);
    static const QRegularExpression code(
        uR"(\bcode\b|\bcoding\b|\bfunctions?\b|\bbugs?\b|\bdebug|\bcompil(?:e|er|ing)\b|\brefactor|\bscripts?\b|)"
        uR"(\bpython\b|\bc\+\+|\brust\b|\bjavascript\b|\btypescript\b|\brepo(?:sitory)?\b|\bgit\b|)"
        uR"(\bunit tests?\b|\btest suite\b|\btests? for\b|\brun (?:the )?tests\b|\bcmake\b|\bstack ?trace\b|)"
        uR"(\bimplement\b|\bpull request\b|\bwrite a program\b)"_s,
        options);
    static const QRegularExpression none(u"(?!)"_s);
    switch (role) {
    case AgentRole::Phone: return phone;
    case AgentRole::Screen: return screen;
    case AgentRole::Web: return web;
    case AgentRole::Code: return code;
    case AgentRole::General: return none;
    }
    return none;
}

} // namespace

AgentRole RoleClassifier::classify(const QString &text) {
    for (const AgentRole role : {AgentRole::Phone, AgentRole::Screen, AgentRole::Web, AgentRole::Code}) {
        if (pattern(role).match(text).hasMatch()) return role;
    }
    return AgentRole::General;
}

AgentRole RoleClassifier::forTool(QStringView toolName) {
    if (toolName.startsWith(u"android_"_s)) return AgentRole::Phone;
    if (toolName.startsWith(u"browser_"_s)) return AgentRole::Web;
    if (toolName == u"take_screenshot"_s || toolName.startsWith(u"screen_"_s) || toolName.startsWith(u"mouse_"_s)
        || toolName == u"type_text"_s || toolName == u"press_keys"_s) {
        return AgentRole::Screen;
    }
    return AgentRole::General;
}

} // namespace nexa
