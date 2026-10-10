#include "system/android/AdbShell.h"

#include <QHash>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa::adbshell {

QString quote(const QString &text) {
    QString quoted = text;
    quoted.replace(u'\'', u"'\\''"_s);
    return u'\'' + quoted + u'\'';
}

TextCommand inputText(const QString &text) {
    QString typed;
    QString skipped;
    for (const QChar c : text) {
        if (c.unicode() >= 0x20 && c.unicode() < 0x7f) {
            typed += c;
        } else if (!skipped.contains(c)) {
            skipped += c;
        }
    }
    typed.replace(u' ', u"%s"_s);
    return TextCommand{typed.isEmpty() ? QString() : u"input text "_s + quote(typed), skipped};
}

std::optional<QString> keyCode(const QString &name) {
    static const QHash<QString, QString> keys{
        {u"home"_s, u"HOME"_s},
        {u"back"_s, u"BACK"_s},
        {u"recents"_s, u"APP_SWITCH"_s},
        {u"recent apps"_s, u"APP_SWITCH"_s},
        {u"app switch"_s, u"APP_SWITCH"_s},
        {u"overview"_s, u"APP_SWITCH"_s},
        {u"enter"_s, u"ENTER"_s},
        {u"return"_s, u"ENTER"_s},
        {u"delete"_s, u"DEL"_s},
        {u"backspace"_s, u"DEL"_s},
        {u"del"_s, u"DEL"_s},
        {u"forward delete"_s, u"FORWARD_DEL"_s},
        {u"tab"_s, u"TAB"_s},
        {u"escape"_s, u"ESCAPE"_s},
        {u"esc"_s, u"ESCAPE"_s},
        {u"space"_s, u"SPACE"_s},
        {u"power"_s, u"POWER"_s},
        {u"wake"_s, u"WAKEUP"_s},
        {u"wakeup"_s, u"WAKEUP"_s},
        {u"wake up"_s, u"WAKEUP"_s},
        {u"sleep"_s, u"SLEEP"_s},
        {u"lock"_s, u"SLEEP"_s},
        {u"volume up"_s, u"VOLUME_UP"_s},
        {u"volume down"_s, u"VOLUME_DOWN"_s},
        {u"mute"_s, u"VOLUME_MUTE"_s},
        {u"menu"_s, u"MENU"_s},
        {u"search"_s, u"SEARCH"_s},
        {u"camera"_s, u"CAMERA"_s},
        {u"notifications"_s, u"NOTIFICATION"_s},
        {u"up"_s, u"DPAD_UP"_s},
        {u"down"_s, u"DPAD_DOWN"_s},
        {u"left"_s, u"DPAD_LEFT"_s},
        {u"right"_s, u"DPAD_RIGHT"_s},
        {u"center"_s, u"DPAD_CENTER"_s},
        {u"page up"_s, u"PAGE_UP"_s},
        {u"page down"_s, u"PAGE_DOWN"_s},
        {u"move home"_s, u"MOVE_HOME"_s},
        {u"move end"_s, u"MOVE_END"_s},
        {u"play pause"_s, u"MEDIA_PLAY_PAUSE"_s},
        {u"brightness up"_s, u"BRIGHTNESS_UP"_s},
        {u"brightness down"_s, u"BRIGHTNESS_DOWN"_s},
        {u"screenshot"_s, u"SYSRQ"_s},
    };
    QString key = name.trimmed();
    static const QRegularExpression keycode(u"^KEYCODE_[A-Z0-9_]+$"_s);
    if (keycode.match(key).hasMatch()) return key;
    bool numeric = false;
    if (const int code = key.toInt(&numeric); numeric && code > 0 && code < 400) return QString::number(code);

    key = key.toLower();
    key.replace(QRegularExpression(u"[_\\-]+"_s), u" "_s);
    key = key.simplified();
    if (key.endsWith(u" key"_s)) key.chop(4);
    if (key.endsWith(u" button"_s)) key.chop(7);
    if (const auto found = keys.constFind(key); found != keys.cend()) return u"KEYCODE_"_s + found.value();
    return std::nullopt;
}

std::optional<QString> mediaKey(const QString &action) {
    static const QHash<QString, QString> actions{
        {u"play"_s, u"MEDIA_PLAY"_s},     {u"pause"_s, u"MEDIA_PAUSE"_s},   {u"toggle"_s, u"MEDIA_PLAY_PAUSE"_s},
        {u"next"_s, u"MEDIA_NEXT"_s},     {u"previous"_s, u"MEDIA_PREVIOUS"_s}, {u"stop"_s, u"MEDIA_STOP"_s},
    };
    const auto found = actions.constFind(action.trimmed().toLower());
    if (found == actions.cend()) return std::nullopt;
    return u"KEYCODE_"_s + found.value();
}

QString phoneNumber(const QString &text) {
    const QString trimmed = text.trimmed();
    // Only separators may accompany the digits: "mom" or "room 12" are not numbers.
    static const QRegularExpression shape(u"^\\+?[\\d\\s().\\-]+$"_s);
    if (!shape.match(trimmed).hasMatch()) return {};
    QString number;
    for (const QChar c : trimmed) {
        if (c.isDigit() || (c == u'+' && number.isEmpty())) number += c;
    }
    const qsizetype digits = number.size() - (number.startsWith(u'+') ? 1 : 0);
    return digits >= 3 ? number : QString();
}

QString callCommand(const QString &number) {
    return u"am start -a android.intent.action.CALL -d "_s + quote(u"tel:"_s + number);
}

QString composeSmsCommand(const QString &number, const QString &body) {
    return u"am start -a android.intent.action.SENDTO -d "_s + quote(u"sms:"_s + number) + u" --es sms_body "_s
           + quote(body) + u" --ez exit_on_sent true"_s;
}

} // namespace nexa::adbshell
