#include "system/android/AndroidTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"
#include "system/android/AdbParsers.h"
#include "system/android/AdbShell.h"
#include "system/android/AndroidDevice.h"
#include "system/android/PhoneLookup.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/files/FileAccess.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using namespace toolargs;

constexpr int kDefaultShellSeconds = 30;
constexpr int kMaxShellSeconds = 300;
constexpr qsizetype kMaxTypedCharacters = 1000;
constexpr int kDumpTimeoutMs = 20'000;
constexpr int kMediaStream = 3;         // STREAM_MUSIC

// Every action that ends in a sentence.
AndroidDevice::Done say(ITool::Done done) {
    return [done = std::move(done)](Result<QString> result) {
        if (!result) {
            done(result.error());
            return;
        }
        done(text(result.value()));
    };
}

// Coordinates are read off the last phone screenshot by default, because that is what the model
// can see; "screen" takes them as the phone's own pixels.
struct PhonePoint {
    int x{0};
    int y{0};
    QString note;
};

PhonePoint phonePoint(int x, int y, const QJsonObject &arguments, const AndroidDevice *device) {
    const bool imageSpace = arguments.value(u"space"_s).toString().trimmed().toLower() != u"screen"_s;
    if (imageSpace && device->lastScreenSize().isValid()) {
        const double scale = device->lastScale();
        return {int(std::lround(x * scale)), int(std::lround(y * scale)), {}};
    }
    return {x, y,
            imageSpace ? u" (no phone screenshot to measure against, so the numbers were taken as phone pixels)"_s
                       : QString()};
}

QString when(qint64 ms) {
    if (ms <= 0) return {};
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(ms);
    return time.toString(time.date() == QDate::currentDate() ? u"HH:mm"_s : u"d MMM HH:mm"_s);
}

const QJsonObject kSpaceArgument = schema::choice(
    u"Which pixels the numbers are measured in: \"image\" (the last phone screenshot, the default) or \"screen\""_s,
    {u"image"_s, u"screen"_s});

void addControlTools(ToolRegistry &registry, AndroidDevice *device, const std::function<void()> &cancel) {
    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_status"_s,
                 u"Which Android phone is connected: model, Android version, battery, screen size, and whether it is "
                 "reached over Wi-Fi or USB."_s,
                 schema::object(), false},
        [device](const QJsonObject &, ITool::Done done) {
            device->info([done](Result<AndroidDevice::Info> info) {
                if (!info) {
                    done(info.error());
                    return;
                }
                done(text(info.value().summary()));
            });
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_open_app"_s,
                 u"Open an app on the phone by name: \"whatsapp\", \"youtube\", \"camera\", \"settings\", \"messages\"."_s,
                 schema::object({{u"name"_s, schema::string(u"App name"_s)}}, {u"name"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> name = requiredString(arguments, u"name"_s);
            if (!name) {
                done(name.error());
                return;
            }
            device->openApp(name.value(), say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_list_apps"_s, u"List the apps on the phone (package names), optionally filtered."_s,
                 schema::object({{u"filter"_s, schema::string(u"Part of a package name (optional)"_s)}}), false},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const QString filter = arguments.value(u"filter"_s).toString().trimmed().toLower();
            device->launchableApps([filter, done](Result<QStringList> apps) {
                if (!apps) {
                    done(apps.error());
                    return;
                }
                QStringList packages;
                for (const QString &component : apps.value()) {
                    const QString package = component.section(u'/', 0, 0);
                    if ((filter.isEmpty() || package.contains(filter)) && !packages.contains(package)) packages.append(package);
                }
                packages.sort();
                done(text(packages.isEmpty() ? u"No app on the phone matches \"%1\"."_s.arg(filter)
                                             : u"%1 apps on the phone: %2"_s.arg(packages.size()).arg(packages.join(u", "_s))));
            });
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_tap"_s,
                 u"Tap the phone's screen. By default x and y are pixel coordinates in the phone screenshot you last "
                 "took, so you can tap something you can see in it."_s,
                 schema::object({{u"x"_s, schema::integer(u"Horizontal position"_s, 0, 20000)},
                                 {u"y"_s, schema::integer(u"Vertical position"_s, 0, 20000)},
                                 {u"space"_s, kSpaceArgument}},
                                {u"x"_s, u"y"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const std::optional<int> x = intArg(arguments, u"x"_s);
            const std::optional<int> y = intArg(arguments, u"y"_s);
            if (!x || !y) {
                done(Error{ErrorCode::InvalidArgument, u"Both \"x\" and \"y\" are needed."_s});
                return;
            }
            const PhonePoint at = phonePoint(*x, *y, arguments, device);
            device->act(u"input tap %1 %2"_s.arg(at.x).arg(at.y),
                        u"Tapped %1, %2 on the phone.%3"_s.arg(at.x).arg(at.y).arg(at.note), say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_swipe"_s,
                 u"Swipe on the phone's screen: give a direction (\"up\" scrolls a list down, like a finger moving up), "
                 "or the start and end points x1, y1, x2, y2."_s,
                 schema::object({{u"direction"_s, schema::choice(u"Which way the finger moves"_s,
                                                                 {u"up"_s, u"down"_s, u"left"_s, u"right"_s})},
                                 {u"x1"_s, schema::integer(u"Start, horizontal"_s, 0, 20000)},
                                 {u"y1"_s, schema::integer(u"Start, vertical"_s, 0, 20000)},
                                 {u"x2"_s, schema::integer(u"End, horizontal"_s, 0, 20000)},
                                 {u"y2"_s, schema::integer(u"End, vertical"_s, 0, 20000)},
                                 {u"duration_ms"_s, schema::integer(u"How long the swipe takes (default 300)"_s, 50, 5000)},
                                 {u"space"_s, kSpaceArgument}})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const int duration = qBound(50, intArg(arguments, u"duration_ms"_s).value_or(300), 5000);
            const QString direction = arguments.value(u"direction"_s).toString().trimmed().toLower();
            if (!direction.isEmpty()) {
                device->screenSize([device, direction, duration, done](Result<QSize> size) {
                    if (!size) {
                        done(size.error());
                        return;
                    }
                    const int w = size.value().width();
                    const int h = size.value().height();
                    // Through the middle of the screen, over half its length.
                    QPoint from(w / 2, h * 3 / 4);
                    QPoint to(w / 2, h / 4);
                    if (direction == u"down") {
                        std::swap(from, to);
                    } else if (direction == u"left") {
                        from = QPoint(w * 4 / 5, h / 2);
                        to = QPoint(w / 5, h / 2);
                    } else if (direction == u"right") {
                        from = QPoint(w / 5, h / 2);
                        to = QPoint(w * 4 / 5, h / 2);
                    }
                    device->act(u"input swipe %1 %2 %3 %4 %5"_s.arg(from.x()).arg(from.y()).arg(to.x()).arg(to.y()).arg(duration),
                                u"Swiped %1 on the phone."_s.arg(direction), say(done));
                });
                return;
            }
            const std::optional<int> x1 = intArg(arguments, u"x1"_s);
            const std::optional<int> y1 = intArg(arguments, u"y1"_s);
            const std::optional<int> x2 = intArg(arguments, u"x2"_s);
            const std::optional<int> y2 = intArg(arguments, u"y2"_s);
            if (!x1 || !y1 || !x2 || !y2) {
                done(Error{ErrorCode::InvalidArgument, u"Give a direction, or all of x1, y1, x2 and y2."_s});
                return;
            }
            const PhonePoint from = phonePoint(*x1, *y1, arguments, device);
            const PhonePoint to = phonePoint(*x2, *y2, arguments, device);
            device->act(u"input swipe %1 %2 %3 %4 %5"_s.arg(from.x).arg(from.y).arg(to.x).arg(to.y).arg(duration),
                        u"Swiped from %1, %2 to %3, %4 on the phone.%5"_s.arg(from.x).arg(from.y).arg(to.x).arg(to.y).arg(from.note),
                        say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_type"_s,
                 u"Type text into whatever field has focus on the phone (tap the field first). adb types plain ASCII "
                 "only; anything else is left out and reported. For Enter use android_key."_s,
                 schema::object({{u"text"_s, schema::string(u"The text to type"_s)}}, {u"text"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const QString value = arguments.value(u"text"_s).toString();
            if (value.isEmpty()) {
                done(Error{ErrorCode::InvalidArgument, u"Missing \"text\""_s});
                return;
            }
            if (value.size() > kMaxTypedCharacters) {
                done(Error{ErrorCode::InvalidArgument, u"That is %1 characters; type at most %2 at a time."_s
                                                           .arg(value.size()).arg(kMaxTypedCharacters)});
                return;
            }
            const adbshell::TextCommand command = adbshell::inputText(value);
            if (command.command.isEmpty()) {
                done(Error{ErrorCode::InvalidArgument,
                           u"None of that can be typed through adb, which types plain ASCII only: %1"_s.arg(command.skipped)});
                return;
            }
            const QString note = command.skipped.isEmpty()
                                     ? QString()
                                     : u" Left out, because adb types plain ASCII only: %1"_s.arg(command.skipped);
            device->act(command.command, u"Typed %1 characters on the phone.%2"_s.arg(value.size()).arg(note), say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_key"_s,
                 u"Press a key on the phone: home, back, recents, enter, delete, tab, space, power, wake, lock, "
                 "volume up, volume down, menu, search, camera, notifications — or any KEYCODE_ name."_s,
                 schema::object({{u"key"_s, schema::string(u"The key"_s)}}, {u"key"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> key = requiredString(arguments, u"key"_s);
            if (!key) {
                done(key.error());
                return;
            }
            const std::optional<QString> code = adbshell::keyCode(key.value());
            if (!code) {
                done(Error{ErrorCode::InvalidArgument, u"There is no phone key called \"%1\"; try home, back, recents, "
                                                       "enter, delete, power, volume up or a KEYCODE_ name."_s.arg(key.value())});
                return;
            }
            device->act(u"input keyevent "_s + *code, u"Pressed %1 on the phone."_s.arg(key.value()), say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_screenshot"_s,
                 u"Take a screenshot of the phone and look at it. Use it to see what is on the phone before tapping."_s,
                 schema::object(), false},
        [device](const QJsonObject &, ITool::Done done) {
            device->screenshot([done](Result<AndroidDevice::Shot> shot) {
                if (!shot) {
                    done(shot.error());
                    return;
                }
                const AndroidDevice::Shot &value = shot.value();
                const QString description =
                    u"Screenshot of the phone attached: %1x%2 JPEG; 1 image pixel = %3 phone pixels (screen %4x%5). "
                    "android_tap takes coordinates in this image."_s.arg(value.imageSize.width())
                        .arg(value.imageSize.height())
                        .arg(value.scale, 0, 'f', 2)
                        .arg(value.realSize.width())
                        .arg(value.realSize.height());
                done(ToolResult{description, ImageAttachment{value.jpeg, u"image/jpeg"_s}});
            });
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_media"_s, u"Control music or video playing on the phone."_s,
                 schema::object({{u"action"_s, schema::choice(u"What to do"_s, {u"play"_s, u"pause"_s, u"toggle"_s,
                                                                                u"next"_s, u"previous"_s, u"stop"_s})}},
                                {u"action"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const QString action = arguments.value(u"action"_s).toString().trimmed().toLower();
            const std::optional<QString> key = adbshell::mediaKey(action);
            if (!key) {
                done(Error{ErrorCode::InvalidArgument, u"Unknown media action \"%1\""_s.arg(action)});
                return;
            }
            device->act(u"input keyevent "_s + *key, u"Sent %1 to the phone's player."_s.arg(action), say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_volume"_s,
                 u"Set the phone's media volume: an absolute `level` (0-100) or a relative `change` (-100..100)."_s,
                 schema::object({{u"level"_s, schema::integer(u"Volume in percent"_s, 0, 100)},
                                 {u"change"_s, schema::integer(u"Change in percentage points"_s, -100, 100)}})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const std::optional<int> level = intArg(arguments, u"level"_s);
            const std::optional<int> change = intArg(arguments, u"change"_s);
            if (!level && !change) {
                done(Error{ErrorCode::InvalidArgument, u"Give either \"level\" or \"change\""_s});
                return;
            }
            const QString get = u"cmd media_session volume --stream %1 --get"_s.arg(kMediaStream);
            device->shell(get, kDumpTimeoutMs, [device, level, change, done](Result<AdbClient::Output> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                const std::optional<StreamVolume> volume = adb::parseVolume(output.value().text());
                if (!volume) {
                    // Not every Android version has `cmd media_session`; the volume keys always work.
                    if (!change) {
                        done(Error{ErrorCode::NotSupported,
                                   u"This phone does not report its volume, so it can only be changed by steps."_s});
                        return;
                    }
                    const int steps = std::max(1, int(std::lround(std::abs(*change) / 100.0 * 15)));
                    QStringList keys;
                    for (int i = 0; i < steps; ++i) keys.append(*change > 0 ? u"KEYCODE_VOLUME_UP"_s : u"KEYCODE_VOLUME_DOWN"_s);
                    device->act(u"input keyevent "_s + keys.join(u' '),
                                u"Pressed volume %1 %2 times on the phone."_s.arg(*change > 0 ? u"up"_s : u"down"_s).arg(steps),
                                say(done));
                    return;
                }
                const int range = volume->maximum - volume->minimum;
                const int target = std::clamp(
                    level ? volume->minimum + int(std::lround(*level / 100.0 * range))
                          : volume->current + int(std::lround(*change / 100.0 * range)),
                    volume->minimum, volume->maximum);
                const int percent = int(std::lround(100.0 * (target - volume->minimum) / range));
                device->act(u"cmd media_session volume --show --stream %1 --set %2"_s.arg(kMediaStream).arg(target),
                            u"Phone media volume set to %1% (%2 of %3)."_s.arg(percent).arg(target).arg(volume->maximum),
                            say(done));
            });
        },
        cancel));
}

void addCommunicationTools(ToolRegistry &registry, AndroidDevice *device, const std::function<void()> &cancel) {
    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_notifications"_s, u"Read the notifications currently on the phone, newest first."_s,
                 schema::object({{u"count"_s, schema::integer(u"How many (default 15)"_s, 1, 50)}}), false},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const int count = qBound(1, intArg(arguments, u"count"_s).value_or(15), 50);
            device->shell(u"dumpsys notification --noredact"_s, kDumpTimeoutMs,
                          [count, done](Result<AdbClient::Output> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                QList<PhoneNotification> list = adb::parseNotifications(output.value().text());
                if (list.isEmpty()) {
                    done(text(u"There are no notifications on the phone."_s));
                    return;
                }
                std::stable_sort(list.begin(), list.end(),
                                 [](const PhoneNotification &a, const PhoneNotification &b) { return a.timeMs > b.timeMs; });
                QStringList lines;
                for (const PhoneNotification &notification : list.mid(0, count)) {
                    QString line = u"• "_s + phonelookup::labelForPackage(notification.package);
                    if (!notification.title.isEmpty()) line += u": "_s + notification.title.simplified();
                    if (!notification.text.isEmpty()) line += u" — "_s + notification.text.simplified();
                    if (const QString time = when(notification.timeMs); !time.isEmpty()) line += u" ("_s + time + u')';
                    lines.append(line);
                }
                done(text(u"%1 notification(s) on the phone:\n%2"_s.arg(list.size()).arg(lines.join(u'\n'))));
            });
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_sms_read"_s,
                 u"Read the newest text messages (SMS) in the phone's inbox, optionally only those from one sender."_s,
                 schema::object({{u"count"_s, schema::integer(u"How many (default 10)"_s, 1, 50)},
                                 {u"from"_s, schema::string(u"Part of the sender's number or name (optional)"_s)}}),
                 false},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const int count = qBound(1, intArg(arguments, u"count"_s).value_or(10), 50);
            const QString from = arguments.value(u"from"_s).toString().trimmed().toLower();
            // Body last: it may contain commas and line breaks, so it takes the rest of the row.
            const QString query = u"content query --uri content://sms/inbox --projection address:date:body --sort "_s
                                  + adbshell::quote(u"date DESC LIMIT %1"_s.arg(from.isEmpty() ? count : 300));
            device->shell(query, kDumpTimeoutMs, [count, from, done](Result<AdbClient::Output> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                const QString words = output.value().text() + output.value().errorText();
                if (words.contains(u"Permission Denial"_s) || words.contains(u"SecurityException"_s)) {
                    done(Error{ErrorCode::PermissionDenied, u"This phone does not let adb read text messages."_s});
                    return;
                }
                QStringList lines;
                for (const QStringList &row : adb::parseContentRows(output.value().text(),
                                                                    {u"address"_s, u"date"_s, u"body"_s})) {
                    if (!from.isEmpty() && !row.value(0).toLower().contains(from)) continue;
                    lines.append(u"From %1 (%2): %3"_s.arg(row.value(0), when(row.value(1).toLongLong()),
                                                           row.value(2).simplified()));
                    if (lines.size() >= count) break;
                }
                done(text(lines.isEmpty() ? u"No text messages%1."_s.arg(from.isEmpty() ? QString() : u" from "_s + from)
                                          : lines.join(u'\n')));
            });
        },
        cancel));

    ToolSpec smsSpec{u"android_sms_send"_s,
                     u"Start a text message (SMS) on the phone: opens the messaging app with the recipient and text "
                     "filled in. It is not sent until Send is tapped — take android_screenshot and tap it."_s,
                     schema::object({{u"to"_s, schema::string(u"Phone number or contact name"_s)},
                                     {u"message"_s, schema::string(u"The text of the message"_s)}},
                                    {u"to"_s, u"message"_s})};
    smsSpec.privileged = true;
    smsSpec.risk = u"writes a text message from your phone"_s;
    registry.add(std::make_unique<FunctionTool>(smsSpec, [device](const QJsonObject &arguments, ITool::Done done) {
        const Result<QString> to = requiredString(arguments, u"to"_s);
        const Result<QString> message = requiredString(arguments, u"message"_s);
        if (!to || !message) {
            done(!to ? to.error() : message.error());
            return;
        }
        device->resolveNumber(to.value(), [device, body = message.value(), done](Result<phonelookup::Contact> contact) {
            if (!contact) {
                done(contact.error());
                return;
            }
            const phonelookup::Contact &who = contact.value();
            const QString name = who.name == who.number ? who.number : u"%1 (%2)"_s.arg(who.name, who.number);
            device->act(adbshell::composeSmsCommand(who.number, body),
                        u"Opened a message to %1 with the text filled in. It is NOT sent yet: take android_screenshot "
                        "and tap Send to send it."_s.arg(name),
                        say(done));
        });
    }, cancel));

    ToolSpec callSpec{u"android_call"_s, u"Call a phone number or a contact from the phone."_s,
                      schema::object({{u"to"_s, schema::string(u"Phone number or contact name"_s)}}, {u"to"_s})};
    callSpec.privileged = true;
    callSpec.risk = u"places a phone call"_s;
    registry.add(std::make_unique<FunctionTool>(callSpec, [device](const QJsonObject &arguments, ITool::Done done) {
        const Result<QString> to = requiredString(arguments, u"to"_s);
        if (!to) {
            done(to.error());
            return;
        }
        device->resolveNumber(to.value(), [device, done](Result<phonelookup::Contact> contact) {
            if (!contact) {
                done(contact.error());
                return;
            }
            const phonelookup::Contact &who = contact.value();
            const QString name = who.name == who.number ? who.number : u"%1 (%2)"_s.arg(who.name, who.number);
            device->act(adbshell::callCommand(who.number), u"Calling %1 from the phone."_s.arg(name), say(done));
        });
    }, cancel));
}

void addFileTools(ToolRegistry &registry, AndroidDevice *device, FileAccess *files, const std::function<void()> &cancel) {
    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_push_file"_s,
                 u"Copy a file from this PC (home folder or /tmp) to the phone; it goes to the phone's Download "
                 "folder unless a destination is given."_s,
                 schema::object({{u"path"_s, schema::string(u"File on the PC"_s)},
                                 {u"destination"_s, schema::string(u"Path on the phone (optional)"_s)}},
                                {u"path"_s})},
        [device, files](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> path = requiredString(arguments, u"path"_s);
            if (!path) {
                done(path.error());
                return;
            }
            const Result<QString> local = files->resolve(path.value());
            if (!local) {
                done(local.error());
                return;
            }
            if (!QFileInfo(local.value()).isFile()) {
                done(Error{ErrorCode::NotFound, u"There is no file at %1"_s.arg(local.value())});
                return;
            }
            QString remote = arguments.value(u"destination"_s).toString().trimmed();
            if (remote.isEmpty()) remote = u"/sdcard/Download/"_s;
            if (remote.endsWith(u'/')) remote += QFileInfo(local.value()).fileName();
            device->push(local.value(), remote, say(done));
        },
        cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_pull_file"_s,
                 u"Copy a file from the phone to this PC; it goes to ~/Downloads unless a destination is given."_s,
                 schema::object({{u"path"_s, schema::string(u"Path on the phone, e.g. /sdcard/DCIM/Camera/x.jpg"_s)},
                                 {u"destination"_s, schema::string(u"Where on the PC (optional)"_s)}},
                                {u"path"_s})},
        [device, files](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> path = requiredString(arguments, u"path"_s);
            if (!path) {
                done(path.error());
                return;
            }
            const QString name = path.value().section(u'/', -1);
            QString destination = arguments.value(u"destination"_s).toString().trimmed();
            if (destination.isEmpty()) destination = u"~/Downloads/"_s;
            if (destination.endsWith(u'/')) destination += name;
            const Result<QString> local = files->resolve(destination);
            if (!local) {
                done(local.error());
                return;
            }
            QDir().mkpath(QFileInfo(local.value()).absolutePath());
            device->pull(path.value(), local.value(), say(done));
        },
        cancel));

    ToolSpec installSpec{u"android_install_apk"_s, u"Install (or update) an APK file from this PC on the phone."_s,
                         schema::object({{u"path"_s, schema::string(u"The .apk file on the PC"_s)}}, {u"path"_s})};
    installSpec.privileged = true;
    installSpec.risk = u"installs an app on your phone"_s;
    registry.add(std::make_unique<FunctionTool>(installSpec, [device, files](const QJsonObject &arguments, ITool::Done done) {
        const Result<QString> path = requiredString(arguments, u"path"_s);
        if (!path) {
            done(path.error());
            return;
        }
        const Result<QString> local = files->resolve(path.value());
        if (!local) {
            done(local.error());
            return;
        }
        if (!QFileInfo(local.value()).isFile() || !local.value().endsWith(u".apk"_s, Qt::CaseInsensitive)) {
            done(Error{ErrorCode::InvalidArgument, u"%1 is not an .apk file."_s.arg(local.value())});
            return;
        }
        device->install(local.value(), say(done));
    }, cancel));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"android_shell"_s,
                 u"Run a shell command on the phone (adb shell) and return its output. For anything the other "
                 "android_* tools do not cover."_s,
                 schema::object({{u"command"_s, schema::string(u"The command line"_s)},
                                 {u"timeout_seconds"_s, schema::integer(u"Default 30"_s, 1, kMaxShellSeconds)}},
                                {u"command"_s})},
        [device](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> command = requiredString(arguments, u"command"_s);
            if (!command) {
                done(command.error());
                return;
            }
            const int seconds = qBound(1, intArg(arguments, u"timeout_seconds"_s).value_or(kDefaultShellSeconds),
                                       kMaxShellSeconds);
            device->shell(command.value(), seconds * 1000,
                          [command = command.value(), done](Result<AdbClient::Output> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                OutputCollector collected;
                collected.append(output.value().standardOutput);
                if (!output.value().standardError.isEmpty()) collected.append("\n" + output.value().standardError);
                QString body = QString::fromUtf8(collected.text()).trimmed();
                if (body.isEmpty()) body = u"(no output)"_s;
                done(text(u"exit code %1 on the phone\n$ %2\n%3"_s.arg(output.value().exitCode).arg(command, body)));
            });
        },
        cancel));
}

} // namespace

void registerAndroidTools(ToolRegistry &registry, AndroidDevice *device, FileAccess *files) {
    const std::function<void()> cancel = [device] { device->cancelAll(); };
    addControlTools(registry, device, cancel);
    addCommunicationTools(registry, device, cancel);
    if (files) addFileTools(registry, device, files, cancel);
}

} // namespace nexa
