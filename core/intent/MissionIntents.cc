#include "core/intent/MissionIntents.h"

#include <QRegularExpression>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr auto kCase = QRegularExpression::CaseInsensitiveOption;

QRegularExpressionMatch whole(const QString &pattern, const QString &text) {
    const QRegularExpression expression(u"^(?:"_s + pattern + u")$"_s, kCase);
    return expression.match(text);
}

// Which task: "task two", "the browser task", "the code agent", "browser 01" → "2", "browser", "code", "browser 01".
QString taskWords() {
    static const QString roles = u"browser|web|code|coding|screen|phone|general"_s;
    static const QString numbers = u"\\d+|one|two|three|four|five|six|seven|eight|nine|ten"_s;
    return u"(?:the )?(?:task (?<n>%2)|(?<r>%1) (?:task|agent)|(?<a>(?:%1|agent)[ -]?\\d+))"_s.arg(roles, numbers);
}

QString targetOf(const QRegularExpressionMatch &found) {
    static const QStringList numbers{u"zero"_s, u"one"_s, u"two"_s, u"three"_s, u"four"_s, u"five"_s,
                                     u"six"_s,  u"seven"_s, u"eight"_s, u"nine"_s, u"ten"_s};
    if (const qsizetype at = numbers.indexOf(found.captured(u"n"_s).toLower()); at >= 0) return QString::number(at);
    for (const QString &name : {u"n"_s, u"r"_s, u"a"_s}) {
        const QString value = found.captured(name);
        if (!value.isEmpty()) return value.toLower() == u"coding"_s ? u"code"_s : value.toLower();
    }
    return {};
}

} // namespace

std::optional<IntentMatch> TaskIntent::match(const IntentInput &input) const {
    const QString &text = input.normalized;
    if (whole(u"(?:pause|hold) (?:all(?: (?:the )?tasks)?|every ?thing|all agents)"_s, text).hasMatch()) {
        return IntentMatch{.control = Control::PauseAll};
    }
    if (whole(u"(?:resume|continue|unpause) (?:all(?: (?:the )?tasks)?|every ?thing|all agents)"_s, text).hasMatch()) {
        return IntentMatch{.control = Control::ResumeAll};
    }
    const QList<std::pair<QString, Control>> verbs{{u"pause|hold"_s, Control::PauseTask},
                                                   {u"resume|continue|unpause"_s, Control::ResumeTask},
                                                   {u"stop|cancel|kill|end"_s, Control::StopTask}};
    for (const auto &[verb, control] : verbs) {
        if (const QRegularExpressionMatch found = whole(u"(?:%1) %2"_s.arg(verb, taskWords()), text); found.hasMatch()) {
            return IntentMatch{.control = control, .target = targetOf(found)};
        }
    }
    if (whole(u"approve(?: it| that)?|yes|yeah|yes,? (?:do it|go ahead|please)|go ahead|allow it|"
              "ok(?:ay)?,? do it|do it|confirm"_s,
              text)
            .hasMatch()) {
        return IntentMatch{.control = Control::Approve};
    }
    if (whole(u"deny(?: it| that)?|no|nope|no,? (?:don'?t|do not|stop)|don'?t(?: do (?:it|that))?|reject(?: it)?"_s, text)
            .hasMatch()) {
        return IntentMatch{.control = Control::Deny};
    }
    if (whole(u"what are you doing|what'?s running|what is running|status|task status|what'?s happening|"
              "how are (?:the|my) tasks(?: going)?|what are (?:the|my) tasks"_s,
              text)
            .hasMatch()) {
        return IntentMatch{.control = Control::Status};
    }
    // The task's words come from what was said, as said ("… and run them now" keeps its "now").
    static const QRegularExpression addTask(
        u"^(?:add (?:a )?(?:new )?task|new task|start (?:a )?(?:new )?task)[:,]?\\s+(?<words>.+)$"_s, kCase);
    if (const QRegularExpressionMatch found = addTask.match(input.raw.trimmed()); found.hasMatch()) {
        return IntentMatch{.control = Control::AddTask, .target = found.captured(u"words"_s).trimmed(),
                           .verbatimPayload = true};
    }
    if (whole(u"that'?s (?:all|it)|thanks,? that'?s (?:all|it)|go to sleep|stop listening|goodbye|bye(?: bye)?"_s, text)
            .hasMatch()) {
        return IntentMatch{.control = Control::EndConversation};
    }
    return std::nullopt;
}

} // namespace nexa
