#include "core/intent/PcIntents.h"

#include <QJsonArray>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QRegularExpressionMatch matchWhole(const QString &pattern, const QString &text, bool caseInsensitive = false) {
    const QRegularExpression expression(u"^(?:"_s + pattern + u")$"_s,
                                        caseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                                        : QRegularExpression::NoPatternOption);
    return expression.match(text);
}

IntentMatch toolMatch(const QString &tool, const QJsonObject &arguments) {
    return IntentMatch{.toolName = tool, .arguments = arguments};
}

} // namespace

std::optional<IntentMatch> ControlIntent::match(const IntentInput &input) const {
    // Everything: every task, every tool, every child process.
    static const QRegularExpression everything(
        u"^(?:stop (?:everything|all(?: (?:tasks|agents))?|it all)|panic|halt|emergency stop|kill everything)$"_s);
    // The voice only: the work goes on.
    static const QRegularExpression talking(
        u"^(?:stop (?:talking|speaking|reading)|be quiet|quiet|shut up|hush|silence|(?:that'?s )?enough|"
        "(?:ok(?:ay)?,? )?(?:you can )?stop talking)$"_s);
    // The task in view.
    static const QRegularExpression task(u"^(?:stop|cancel|abort|stop (?:it|that)|cancel (?:it|that)|never ?mind|forget it)$"_s);
    if (everything.match(input.normalized).hasMatch()) return IntentMatch{.panic = true};
    if (talking.match(input.normalized).hasMatch()) return IntentMatch{.control = Control::StopSpeaking};
    if (task.match(input.normalized).hasMatch()) return IntentMatch{.control = Control::StopTask};
    return std::nullopt;
}

std::optional<IntentMatch> MediaIntent::match(const IntentInput &input) const {
    // Bare "next", "back" or "continue" are ordinary chat words, so those need a noun.
    static const QString nouns = u"(?:music|song|track|media|video|playback|player)"_s;
    static const QString optionalNoun = u"(?:\\s+(?:the\\s+)?(?:"_s + nouns + u"|it))?"_s;
    static const QString requiredNoun = u"\\s+(?:the\\s+)?"_s + nouns;
    const QString &text = input.normalized;
    QString action;
    if (matchWhole(u"(?:play|resume|unpause)"_s + optionalNoun, text).hasMatch()) {
        action = u"play"_s;
    } else if (matchWhole(u"pause"_s + optionalNoun, text).hasMatch()) {
        action = u"pause"_s;
    } else if (matchWhole(u"play ?/ ?pause|toggle\\s+(?:the\\s+)?(?:music|playback)"_s, text).hasMatch()) {
        action = u"toggle"_s;
    } else if (matchWhole(u"stop"_s + requiredNoun, text).hasMatch()) {
        action = u"stop"_s;  // bare "stop" is a panic (ControlIntent runs first)
    } else if (matchWhole(u"skip"_s + optionalNoun + u"|(?:play\\s+)?(?:the\\s+)?next"_s + requiredNoun, text)
                   .hasMatch()) {
        action = u"next"_s;
    } else if (matchWhole(u"(?:play\\s+)?(?:the\\s+)?(?:previous|prev|last)"_s + requiredNoun
                              + u"|(?:go\\s+)?back"_s + requiredNoun + u"|go back a (?:song|track)"_s,
                          text)
                   .hasMatch()) {
        action = u"previous"_s;
    } else {
        return std::nullopt;
    }
    return toolMatch(u"media_control"_s, {{u"action"_s, action}});
}

std::optional<IntentMatch> AudioIntent::match(const IntentInput &input) const {
    const QString &text = input.normalized;
    static const QString percent = u"(\\d{1,3})\\s*(?:%|percent)?"_s;

    if (QRegularExpressionMatch m = matchWhole(
            u"(?:set\\s+)?(?:the\\s+)?(?:volume|sound)(?:\\s+level)?\\s+(?:to\\s+|at\\s+)?"_s + percent, text);
        m.hasMatch()) {
        return toolMatch(u"set_volume"_s, {{u"level"_s, qMin(m.captured(1).toInt(), 100)}});
    }
    if (matchWhole(u"(?:max|full|maximum)\\s+volume|volume\\s+(?:max|full|maximum)"_s, text).hasMatch()) {
        return toolMatch(u"set_volume"_s, {{u"level"_s, 100}});
    }
    if (QRegularExpressionMatch m = matchWhole(
            u"(?:turn\\s+|crank\\s+)?(?:the\\s+)?(?:volume|sound|it)\\s+(up|down)(?:\\s+(?:by\\s+)?"_s + percent + u")?"_s
                + u"|(?:turn|crank)\\s+(up|down)\\s+(?:the\\s+)?(?:volume|sound)(?:\\s+(?:by\\s+)?"_s + percent + u")?"_s,
            text);
        m.hasMatch()) {
        const bool up = (m.captured(1) == u"up") || (m.captured(3) == u"up");
        const QString amount = m.captured(2).isEmpty() ? m.captured(4) : m.captured(2);
        const int step = amount.isEmpty() ? 10 : qMin(amount.toInt(), 100);
        return toolMatch(u"set_volume"_s, {{u"change"_s, up ? step : -step}});
    }
    if (QRegularExpressionMatch m = matchWhole(u"(louder|quieter|softer)"_s, text); m.hasMatch()) {
        return toolMatch(u"set_volume"_s, {{u"change"_s, m.captured(1) == u"louder" ? 10 : -10}});
    }
    if (QRegularExpressionMatch m = matchWhole(
            u"(mute|unmute)(?:\\s+(?:the\\s+)?(?:sound|audio|volume|speakers?|computer|pc))?"_s, text);
        m.hasMatch()) {
        return toolMatch(u"mute"_s, {{u"muted"_s, m.captured(1) == u"mute"}});
    }
    return std::nullopt;
}

std::optional<IntentMatch> ScreenIntent::match(const IntentInput &input) const {
    static const QRegularExpression describe(
        u"^(?:(?:(?:take|grab|capture)\\s+(?:a\\s+)?)?screenshot|(?:see|look at)\\s+(?:this|that|my screen|the screen)"
        "|(?:describe|read|check)\\s+(?:my|the)\\s+screen)$"_s);
    static const QRegularExpression question(
        u"^(?:what(?:'s| is)\\s+on\\s+(?:my|the)\\s+screen|what do you see|what am i (?:looking at|seeing))$"_s);

    // Switching the sharing itself: no tool, the controller does it.
    static const QRegularExpression shareOn(
        u"^(?:(?:start|begin)\\s+)?shar(?:e|ing)\\s+(?:my|the)\\s+screen(?:\\s+with\\s+you)?$"
        "|^(?:turn|switch)\\s+(?:on\\s+screen\\s+shar(?:e|ing)|screen\\s+shar(?:e|ing)\\s+on)$"
        "|^screen\\s+shar(?:e|ing)\\s+on$|^share\\s+screen$"_s);
    static const QRegularExpression shareOff(
        u"^(?:stop|end|quit)\\s+sharing\\s+(?:my|the)\\s+screen$|^unshare\\s+(?:my|the)\\s+screen$"
        "|^(?:turn|switch)\\s+(?:off\\s+screen\\s+shar(?:e|ing)|screen\\s+shar(?:e|ing)\\s+off)$"
        "|^screen\\s+shar(?:e|ing)\\s+off$|^stop\\s+(?:screen\\s+)?shar(?:e|ing)$"_s);

    const QString &text = input.normalized;
    if (shareOn.match(text).hasMatch()) return IntentMatch{.shareScreen = true};
    if (shareOff.match(text).hasMatch()) return IntentMatch{.shareScreen = false};
    if (question.match(text).hasMatch()) {
        return IntentMatch{.toolName = u"take_screenshot"_s, .forwardToAgent = true, .agentPrompt = input.text};
    }
    if (describe.match(text).hasMatch()) {
        return IntentMatch{.toolName = u"take_screenshot"_s, .forwardToAgent = true,
                           .agentPrompt = u"Describe what is on my screen."_s};
    }
    return std::nullopt;
}

std::optional<IntentMatch> AppIntent::match(const IntentInput &input) const {
    if (!catalog_) return std::nullopt;
    static const QString app = u"(?:the\\s+|my\\s+|a\\s+|an\\s+)?(.+?)(?:\\s+(?:app|application|program|window))?"_s;

    if (QRegularExpressionMatch m =
            matchWhole(u"(?:open|launch|start|fire up|bring up|show me)\\s+(?:up\\s+)?"_s + app, input.normalized);
        m.hasMatch()) {
        const QString name = m.captured(1);
        if (catalog_->score(name) >= kMinScore) return toolMatch(u"open_app"_s, {{u"name"_s, name}});
        return std::nullopt;
    }
    if (QRegularExpressionMatch m = matchWhole(u"(?:close|quit|exit|kill)\\s+"_s + app, input.normalized);
        m.hasMatch()) {
        const QString name = m.captured(1);
        if (catalog_->score(name) >= kMinScore) return toolMatch(u"close_app"_s, {{u"name"_s, name}});
    }
    return std::nullopt;
}

std::optional<IntentMatch> CommandIntent::match(const IntentInput &input) const {
    // Matched on the original text: commands are case-sensitive.
    QRegularExpressionMatch m = matchWhole(u"(?:run|execute|exec)\\s+(?:the\\s+)?(?:command\\s+)?(.+)|\\$\\s*(.+)"_s,
                                           input.text, true);
    if (!m.hasMatch()) return std::nullopt;
    const bool explicitShell = m.captured(1).isEmpty();  // "$ cmd" always means a shell command
    QString command = (explicitShell ? m.captured(2) : m.captured(1)).trimmed();

    static const QRegularExpression asRoot(u"\\s+(?:as|with)\\s+(?:root|sudo|admin)$"_s,
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sudoPrefix(u"^sudo\\s+(?:-\\S+\\s+)*"_s);
    bool root = false;
    if (QRegularExpressionMatch suffix = asRoot.match(command); suffix.hasMatch()) {
        command.chop(suffix.capturedLength());
        root = true;
    } else if (QRegularExpressionMatch prefix = sudoPrefix.match(command); prefix.hasMatch()) {
        command.remove(0, prefix.capturedLength());
        root = true;
    }
    command = command.trimmed();
    if (command.isEmpty()) return std::nullopt;

    // "run firefox": a bare application name means launch it (a GUI app would block run_command).
    if (!root && !command.contains(u' ') && catalog_ && catalog_->score(command.toLower()) >= 0.95) {
        return toolMatch(u"open_app"_s, {{u"name"_s, command.toLower()}});
    }
    // "run me through the steps" is a chat request, not a command: the program must exist.
    const QString program = command.section(u' ', 0, 0, QString::SectionSkipEmpty);
    if (!explicitShell && commandExists_ && !commandExists_(program)) return std::nullopt;
    IntentMatch match = toolMatch(root ? u"run_root_command"_s : u"run_command"_s, {{u"command"_s, command}});
    match.verbatimPayload = true;
    return match;
}

std::optional<IntentMatch> InputIntent::match(const IntentInput &input) const {
    // Dictated words are typed as said, a final "now" or "thanks" included.
    if (QRegularExpressionMatch m = matchWhole(u"type\\s+(?:out\\s+)?(.+)"_s, input.raw, true); m.hasMatch()) {
        QString text = m.captured(1);
        // "type of thing", "type of error" — a noun phrase, not an instruction to type.
        static const QRegularExpression nounPhrase(u"^of\\b"_s, QRegularExpression::CaseInsensitiveOption);
        if (nounPhrase.match(text).hasMatch()) return std::nullopt;
        // Quotes around the payload are delimiters, not text to type.
        if (text.size() >= 2 && (text.front() == u'"' || text.front() == u'\'') && text.back() == text.front()) {
            text = text.mid(1, text.size() - 2);
        }
        IntentMatch match = toolMatch(u"type_text"_s, {{u"text"_s, text}});
        match.verbatimPayload = true;
        return match;
    }
    if (QRegularExpressionMatch m = matchWhole(u"(?:press|hit)\\s+(?:the\\s+)?(.+?)(?:\\s+keys?)?"_s, input.normalized);
        m.hasMatch()) {
        const QStringList keys = m.captured(1).split(QRegularExpression(u"\\s*\\+\\s*|\\s+"_s),
                                                     Qt::SkipEmptyParts);
        if (keys.isEmpty() || keys.size() > 5) return std::nullopt;
        // One chord, written the way press_keys takes it.
        IntentMatch match = toolMatch(u"press_keys"_s, {{u"keys"_s, keys.join(u'+')}});
        match.verbatimPayload = true;  // "ctrl+shift+t" is one chord, not a compound request
        return match;
    }
    return std::nullopt;
}

} // namespace nexa
