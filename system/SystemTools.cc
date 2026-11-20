#include "system/SystemTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolArguments.h"
#include "core/tools/ToolRegistry.h"
#include "system/apps/AppLauncher.h"
#include "system/apps/DesktopEntryIndex.h"
#include "system/audio/WpctlAudioController.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/commands/RootCommandRunner.h"
#include "system/files/FileAccess.h"
#include "system/input/IInputBackend.h"
#include "system/info/SystemInfo.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenCapture.h"

#include "proto/KeyMap.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QScreen>
#ifdef Q_OS_WIN
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#endif

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kDefaultTimeoutSeconds = 30;
constexpr int kMaxTimeoutSeconds = 600;
constexpr int kDefaultRootTimeoutSeconds = 60;
// Typing a whole document through a virtual keyboard takes milliseconds per character, so there
// is a point past which a file would be the better answer.
constexpr qsizetype kMaxTypedCharacters = 4000;

using namespace toolargs;

// What the model sees after a command: how it ended, how long it took, what was run, and its output.
QString commandReport(const QString &command, const CommandOutput &result, int timeoutSeconds) {
    QString status = u"exit code %1"_s.arg(result.exitCode);
    if (result.timedOut) status = u"killed after the %1 s timeout"_s.arg(timeoutSeconds);
    else if (result.crashed) status = u"terminated by a signal"_s;
    QString body = result.output.trimmed();
    if (body.isEmpty()) body = u"(no output)"_s;
    return u"%1 (%2 s)\n$ %3\n%4"_s.arg(status).arg(result.elapsedMs / 1000.0, 0, 'f', 1).arg(command, body);
}

// The screen in real pixels, which is the space the portal's screenshots and the helper's absolute
// pointer both use. A fractionally scaled desktop reports fewer logical pixels than it has.
QSize screenPixelSize() {
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) return {};
    const QSize logical = screen->geometry().size();
    const double ratio = screen->devicePixelRatio();
    return QSize(int(std::lround(logical.width() * ratio)), int(std::lround(logical.height() * ratio)));
}

QString volumeText(const VolumeState &state) {
    return u"Volume %1%%2"_s.arg(state.percent).arg(state.muted ? u" (muted)"_s : QString());
}

Result<DesktopEntryIndex::Match> findApp(DesktopEntryIndex *apps, const QString &name) {
    if (!apps->isReady()) apps->refreshNow();
    const QList<DesktopEntryIndex::Match> matches = apps->search(name, 5);
    if (matches.isEmpty() || matches.first().score < 0.75) {
        QStringList names;
        for (const auto &match : matches) names.append(match.entry.name);
        return Error{ErrorCode::NotFound, u"No installed application matches \"%1\"%2"_s.arg(
                                              name, names.isEmpty() ? QString() : u". Closest: "_s + names.join(u", "_s))};
    }
    return matches.first();
}

void addCommandTools(ToolRegistry &registry, LocalCommandRunner *commands) {
    ToolSpec spec{
        u"run_command"_s,
#ifdef Q_OS_WIN
        u"Run a Windows command on the user's PC (cmd.exe, as the user, in the home folder unless working_directory "
        "is set; prefix with `powershell -NoProfile -Command ...` for PowerShell) and return its exit code and output. "
        "Only for commands that finish on their own; never for graphical apps (use open_app). Set background=true to "
        "start a server or long-running program without waiting for it."_s,
        schema::object({{u"command"_s, schema::string(u"The command line, e.g. \"dir\" or \"powershell -NoProfile -Command Get-Process\""_s)},
#else
        u"Run a shell command on the user's PC (bash, as the user, in the home folder unless working_directory "
        "is set) and return its exit code and output. Only for commands that finish on their own; never for "
        "graphical apps (use open_app). Set background=true to start a server or long-running program without "
        "waiting for it."_s,
        schema::object({{u"command"_s, schema::string(u"The command line, e.g. \"df -h\" or \"ls ~/Downloads | head\""_s)},
#endif
                        {u"timeout_seconds"_s, schema::integer(u"Kill the command after this many seconds (default 30)"_s,
                                                               1, kMaxTimeoutSeconds)},
                        {u"working_directory"_s, schema::string(u"Folder to run in (default: home)"_s)},
                        {u"background"_s, schema::boolean(u"Start it detached and return immediately"_s)}},
                       {u"command"_s}),
    };
    registry.add(std::make_unique<FunctionTool>(
        spec,
        [commands](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> command = requiredString(arguments, u"command"_s);
            if (!command) {
                done(command.error());
                return;
            }
            // sudo here would sit waiting for a password on a terminal that does not exist, and
            // time out. run_root_command is the way to ask for root.
            static const QRegularExpression elevator(u"^\\s*(sudo|pkexec|doas)\\b"_s);
            if (elevator.match(command.value()).hasMatch()) {
                done(Error{ErrorCode::InvalidArgument,
                           u"Do not use sudo here — there is no terminal to type a password into. Use the "
                           "run_root_command tool instead, with the command itself."_s});
                return;
            }
            const QString directory = arguments.value(u"working_directory"_s).toString();
            if (boolArg(arguments, u"background"_s).value_or(false)) {
                const Result<qint64> pid = commands->startDetached(command.value(), directory);
                if (!pid) {
                    done(pid.error());
                    return;
                }
                done(text(u"Started in the background (pid %1): %2"_s.arg(pid.value()).arg(command.value())));
                return;
            }
            const int seconds = qBound(1, intArg(arguments, u"timeout_seconds"_s).value_or(kDefaultTimeoutSeconds),
                                       kMaxTimeoutSeconds);
            commands->run(command.value(), directory, seconds * 1000,
                          [command = command.value(), seconds, done](Result<CommandOutput> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                const CommandOutput &result = output.value();
                QString status = u"exit code %1"_s.arg(result.exitCode);
                if (result.timedOut) status = u"killed after the %1 s timeout"_s.arg(seconds);
                else if (result.crashed) status = u"terminated by a signal"_s;
                QString body = result.output.trimmed();
                if (body.isEmpty()) body = u"(no output)"_s;
                done(text(u"%1 (%2 s)\n$ %3\n%4"_s.arg(status)
                              .arg(result.elapsedMs / 1000.0, 0, 'f', 1)
                              .arg(command, body)));
            });
        },
        [commands] { commands->cancelAll(); }));
}

OpenedApp openedApp(const DesktopEntry &entry) {
    return OpenedApp{entry.name, entry.id, entry.executableName(), entry.categories};
}

void addAppTools(ToolRegistry &registry, DesktopEntryIndex *apps, const SystemServices &services) {
    registry.add(FunctionTool::sync(
        ToolSpec{u"open_app"_s,
#ifdef Q_OS_WIN
                 u"Open (launch) an installed Windows application by name, e.g. \"chrome\", \"notepad\", "
                 "\"calculator\", \"settings\", \"terminal\", \"file explorer\", \"paint\". Names are matched fuzzily."_s,
#else
                 u"Open (launch) an installed desktop application by name, e.g. \"firefox\", \"files\", "
                 "\"terminal\", \"text editor\", \"settings\", \"calculator\". Names are matched fuzzily."_s,
#endif
                 schema::object({{u"name"_s, schema::string(u"Application name"_s)}}, {u"name"_s})},
        [apps, opened = services.appOpened](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> name = requiredString(arguments, u"name"_s);
            if (!name) return name.error();
            const Result<DesktopEntryIndex::Match> app = findApp(apps, name.value());
            if (!app) return app.error();
            const Result<QString> launched = AppLauncher::launch(app.value().entry);
            if (!launched) return launched.error();
            // The app the user asked for is the one in front of them now.
            if (opened) opened(openedApp(app.value().entry));
            return text(launched.value());
        }));

    registry.add(FunctionTool::sync(
        ToolSpec{u"close_app"_s,
#ifdef Q_OS_WIN
                 u"Close a running application by name (asks its window to close)."_s,
#else
                 u"Close a running application by name (asks it to quit with SIGTERM)."_s,
#endif
                 schema::object({{u"name"_s, schema::string(u"Application name"_s)}}, {u"name"_s})},
        [apps, closed = services.appClosed](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> name = requiredString(arguments, u"name"_s);
            if (!name) return name.error();
            const Result<DesktopEntryIndex::Match> app = findApp(apps, name.value());
            if (!app) return app.error();
            // Asked to close it, the user has left it — whether or not the process can be found
            // (a terminal's launcher hands over to a server process the entry does not name).
            if (closed) closed(openedApp(app.value().entry));
            const Result<QString> result = AppLauncher::close(app.value().entry);
            if (!result) return result.error();
            return text(result.value());
        }));

    registry.add(FunctionTool::sync(
        ToolSpec{u"list_apps"_s, u"List installed desktop applications, optionally filtered by a search term."_s,
                 schema::object({{u"filter"_s, schema::string(u"Search term (optional)"_s)}}), false},
        [apps](const QJsonObject &arguments) -> Result<ToolResult> {
            if (!apps->isReady()) apps->refreshNow();
            const QString filter = arguments.value(u"filter"_s).toString().trimmed();
            QStringList lines;
            if (filter.isEmpty()) {
                for (const DesktopEntry &entry : apps->entries()) lines.append(entry.name);
                return text(u"%1 applications: %2"_s.arg(lines.size()).arg(lines.join(u", "_s)));
            }
            for (const auto &match : apps->search(filter, 25)) {
                lines.append(u"%1 (%2)"_s.arg(match.entry.name, match.entry.id));
            }
            return text(lines.isEmpty() ? u"No application matches \"%1\""_s.arg(filter) : lines.join(u'\n'));
        }));
}

void addAudioTools(ToolRegistry &registry, WpctlAudioController *audio) {
    const auto report = [](ITool::Done done) {
        return [done = std::move(done)](Result<VolumeState> state) {
            if (!state) {
                done(state.error());
                return;
            }
            done(text(volumeText(state.value())));
        };
    };

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"get_volume"_s, u"Get the speaker volume (percent) and whether it is muted."_s, schema::object(), false},
        [audio, report](const QJsonObject &, ITool::Done done) { audio->volume(report(std::move(done))); }));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"set_volume"_s,
                 u"Set the speaker volume: an absolute `level` (0-100) or a relative `change` (-100..100)."_s,
                 schema::object({{u"level"_s, schema::integer(u"Absolute volume in percent"_s, 0, 100)},
                                 {u"change"_s, schema::integer(u"Relative change in percentage points"_s, -100, 100)}})},
        [audio, report](const QJsonObject &arguments, ITool::Done done) {
            if (const std::optional<int> level = intArg(arguments, u"level"_s)) {
                audio->setVolume(*level, report(std::move(done)));
            } else if (const std::optional<int> change = intArg(arguments, u"change"_s)) {
                audio->changeVolume(*change, report(std::move(done)));
            } else {
                done(Error{ErrorCode::InvalidArgument, u"Give either \"level\" or \"change\""_s});
            }
        }));

    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"mute"_s, u"Mute or unmute the speakers; omit `muted` to toggle."_s,
                 schema::object({{u"muted"_s, schema::boolean(u"true to mute, false to unmute"_s)}})},
        [audio, report](const QJsonObject &arguments, ITool::Done done) {
            audio->setMuted(boolArg(arguments, u"muted"_s), report(std::move(done)));
        }));
}

void addMediaTool(ToolRegistry &registry, MprisController *media) {
    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"media_control"_s,
                 u"Control the active media player (Spotify, browsers, VLC, … via MPRIS)."_s,
                 schema::object({{u"action"_s, schema::choice(u"What to do"_s, {u"play"_s, u"pause"_s, u"toggle"_s,
                                                                                u"next"_s, u"previous"_s, u"stop"_s})}},
                                {u"action"_s})},
        [media](const QJsonObject &arguments, ITool::Done done) {
            const std::optional<MprisController::Action> action =
                MprisController::actionFromString(arguments.value(u"action"_s).toString());
            if (!action) {
                done(Error{ErrorCode::InvalidArgument, u"Unknown media action"_s});
                return;
            }
            media->control(*action, [done](Result<QString> result) {
                if (!result) {
                    done(result.error());
                    return;
                }
                done(text(result.value()));
            });
        }));
}

void addScreenTool(ToolRegistry &registry, ScreenCapture *screen) {
    registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"take_screenshot"_s,
                 u"Take a screenshot of the PC screen and look at it. Use it whenever you need to see what is on "
                 "the screen."_s,
                 schema::object(), false},
        [screen](const QJsonObject &, ITool::Done done) {
            screen->capture([done](Result<ScreenCapture::Shot> shot) {
                if (!shot) {
                    done(shot.error());
                    return;
                }
                const ScreenCapture::Shot &value = shot.value();
                const QString description =
                    u"Screenshot of the PC screen attached: %1x%2 JPEG; 1 image pixel = %3 screen pixels "
                    "(screen %4x%5)."_s.arg(value.imageSize.width())
                        .arg(value.imageSize.height())
                        .arg(value.scale, 0, 'f', 2)
                        .arg(value.realSize.width())
                        .arg(value.realSize.height());
                done(ToolResult{description, ImageAttachment{value.jpeg, u"image/jpeg"_s}});
            });
        },
        [screen] { screen->cancel(); }));
}

void addFileTools(ToolRegistry &registry, FileAccess *files) {
    registry.add(FunctionTool::sync(
        ToolSpec{u"read_file"_s, u"Read a text file in the user's home folder or /tmp."_s,
                 schema::object({{u"path"_s, schema::string(u"File path; ~ is the home folder"_s)},
                                 {u"max_bytes"_s, schema::integer(u"Read at most this many bytes (default 65536)"_s,
                                                                  1, 1024 * 1024)}},
                                {u"path"_s}),
                 false},
        [files](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> path = requiredString(arguments, u"path"_s);
            if (!path) return path.error();
            const Result<QString> content = files->read(path.value(), intArg(arguments, u"max_bytes"_s).value_or(65536));
            if (!content) return content.error();
            return text(content.value());
        }));

    registry.add(FunctionTool::sync(
        ToolSpec{u"write_file"_s,
                 u"Write a text file in the user's home folder or /tmp (creates missing folders). Overwrites the "
                 "file unless append=true."_s,
                 schema::object({{u"path"_s, schema::string(u"File path; ~ is the home folder"_s)},
                                 {u"content"_s, schema::string(u"Text to write"_s)},
                                 {u"append"_s, schema::boolean(u"Append instead of overwriting"_s)}},
                                {u"path"_s, u"content"_s})},
        [files](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> path = requiredString(arguments, u"path"_s);
            if (!path) return path.error();
            const bool append = boolArg(arguments, u"append"_s).value_or(false);
            const Result<qint64> written = files->write(path.value(), arguments.value(u"content"_s).toString(), append);
            if (!written) return written.error();
            const Result<QString> resolved = files->resolve(path.value());
            return text(u"%1 %2 bytes to %3"_s.arg(append ? u"Appended"_s : u"Wrote"_s)
                            .arg(written.value())
                            .arg(resolved ? resolved.value() : path.value()));
        }));
}

void addInfoTool(ToolRegistry &registry) {
    registry.add(FunctionTool::sync(
        ToolSpec{u"get_system_info"_s,
                 u"Facts about this PC: OS, kernel, desktop, CPU, memory, disk space, battery, load and uptime."_s,
                 schema::object(), false},
        [](const QJsonObject &) -> Result<ToolResult> {
            return text(QString::fromUtf8(QJsonDocument(SystemInfo::collect()).toJson(QJsonDocument::Indented)));
        }));
}

// run_root_command: the same shape as run_command, but through nexa-helper. Marked privileged, so
// the policy engine can ask first and the audit log records it as an elevated action.
void addRootCommandTool(ToolRegistry &registry, RootCommandRunner *root) {
    ToolSpec spec{
        u"run_root_command"_s,
        u"Run a shell command as root (administrator) on the user's PC. The user is asked for their password the "
        "first time in a session, then not again. Use it only when the command genuinely needs root, such as "
        "installing packages or editing system files; prefer run_command otherwise."_s,
        schema::object({{u"command"_s, schema::string(u"The command line, e.g. \"apt-get -y install htop\""_s)},
                        {u"timeout_seconds"_s, schema::integer(u"Kill the command after this many seconds "
                                                               "(default 60)"_s, 1, kMaxTimeoutSeconds)},
                        {u"working_directory"_s, schema::string(u"Folder to run in"_s)}},
                       {u"command"_s}),
    };
    spec.privileged = true;
    registry.add(std::make_unique<FunctionTool>(
        spec,
        [root](const QJsonObject &arguments, ITool::Done done) {
            const Result<QString> command = requiredString(arguments, u"command"_s);
            if (!command) {
                done(command.error());
                return;
            }
            const int seconds = qBound(1, intArg(arguments, u"timeout_seconds"_s).value_or(kDefaultRootTimeoutSeconds),
                                       kMaxTimeoutSeconds);
            root->run(command.value(), arguments.value(u"working_directory"_s).toString(), seconds * 1000,
                      [command = command.value(), seconds, done](Result<CommandOutput> output) {
                if (!output) {
                    done(output.error());
                    return;
                }
                done(text(commandReport(command, output.value(), seconds)));
            });
        },
        [root] { root->cancelAll(); }));
}

using InputProvider = std::function<IInputBackend *()>;

// Where a pointer tool should go, in real screen pixels. Coordinates are read off the last
// screenshot by default, because that is what the model can actually see; "screen" takes them as
// physical pixels instead.
struct PointerTarget {
    int x{0};
    int y{0};
    QSize screen;
    QString note;
};

Result<PointerTarget> pointerTarget(const QJsonObject &arguments, ScreenCapture *capture) {
    const std::optional<int> x = intArg(arguments, u"x"_s);
    const std::optional<int> y = intArg(arguments, u"y"_s);
    if (!x || !y) return Error{ErrorCode::InvalidArgument, u"Both \"x\" and \"y\" are needed."_s};

    const QString space = arguments.value(u"space"_s).toString().trimmed().toLower();
    const bool imageSpace = space != u"screen"_s;
    PointerTarget target{*x, *y, screenPixelSize(), {}};

    if (imageSpace && capture && capture->lastScreenSize().isValid()) {
        const double scale = capture->lastScale();
        target.screen = capture->lastScreenSize();
        target.x = int(std::lround(*x * scale));
        target.y = int(std::lround(*y * scale));
    } else if (imageSpace) {
        target.note = u" (no screenshot to measure against, so the numbers were taken as screen pixels)"_s;
    }
    if (!target.screen.isValid()) {
        return Error{ErrorCode::NotSupported, u"The screen size is unknown; take a screenshot first."_s};
    }
    target.x = qBound(0, target.x, target.screen.width() - 1);
    target.y = qBound(0, target.y, target.screen.height() - 1);
    return target;
}

void addInputTools(ToolRegistry &registry, const InputProvider &input, ScreenCapture *capture) {
    // Shared by every tool here: fetch the current backend or explain why there is none.
    const auto backendOr = [input](ITool::Done &done) -> IInputBackend * {
        IInputBackend *backend = input();
        if (!backend) {
            done(Error{ErrorCode::NotSupported,
                       u"Nexa cannot type or click on this desktop: the root helper is not installed "
                       "(sudo ./packaging/install-helper.sh), the desktop offers no remote-control portal, and "
                       "there is no X11 fallback."_s});
        }
        return backend;
    };
    // Every one of these ends the same way: a sentence plus whatever the backend wants to add.
    const auto finish = [](QString summary, ITool::Done done) {
        return [summary = std::move(summary), done](Result<QString> note) {
            if (!note) {
                done(note.error());
                return;
            }
            done(text(note.value().isEmpty() ? summary : summary + u" "_s + note.value()));
        };
    };

    ToolSpec typeSpec{
        u"type_text"_s,
        u"Type text on the keyboard, into whichever window has focus. Make sure the right window is focused "
        "first — open it with open_app, or click into it. Only characters on a US keyboard can be typed; "
        "anything else is reported back and left out."_s,
        schema::object({{u"text"_s, schema::string(u"The text to type. Use \\n for Enter."_s)}}, {u"text"_s}),
    };
    typeSpec.privileged = true;
    typeSpec.risk = u"types on your keyboard"_s;
    registry.add(std::make_unique<FunctionTool>(typeSpec, [backendOr, finish](const QJsonObject &arguments,
                                                                             ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        const Result<QString> value = requiredText(arguments, u"text"_s);
        if (!value) {
            done(value.error());
            return;
        }
        if (value.value().size() > kMaxTypedCharacters) {
            done(Error{ErrorCode::InvalidArgument,
                       u"That is %1 characters; type at most %2 at a time, or write a file instead."_s
                           .arg(value.value().size()).arg(kMaxTypedCharacters)});
            return;
        }
        const QString summary = u"Typed %1 characters with the %2."_s.arg(value.value().size()).arg(backend->name());
        backend->typeText(value.value(), finish(summary, done));
    }));

    ToolSpec keysSpec{
        u"press_keys"_s,
        u"Press a key or a combination, into whichever window has focus — \"ctrl+s\", \"alt+tab\", \"enter\", "
        "\"f5\", \"ctrl+shift+t\". Modifiers are ctrl, alt, shift and super."_s,
        schema::object({{u"keys"_s, schema::string(u"The combination, joined with +"_s)}}, {u"keys"_s}),
    };
    keysSpec.privileged = true;
    keysSpec.risk = u"presses keys for you"_s;
    registry.add(std::make_unique<FunctionTool>(keysSpec, [backendOr, finish](const QJsonObject &arguments,
                                                                             ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        // The schema asks for "ctrl+s", but models sometimes send ["ctrl", "s"].
        QString chord = arguments.value(u"keys"_s).toString().trimmed();
        if (chord.isEmpty() && arguments.value(u"keys"_s).isArray()) {
            QStringList parts;
            for (const QJsonValue &part : arguments.value(u"keys"_s).toArray()) {
                if (!part.toString().trimmed().isEmpty()) parts.append(part.toString().trimmed());
            }
            chord = parts.join(u'+');
        }
        if (chord.isEmpty()) {
            done(Error{ErrorCode::InvalidArgument, u"Missing \"keys\""_s});
            return;
        }
        const QString keys = chord;
        const Result<QList<int>> codes = keymap::parseChord(keys);
        if (!codes) {
            done(codes.error());
            return;
        }
        backend->pressKeys(codes.value(), finish(u"Pressed %1."_s.arg(keys), done));
    }));

    ToolSpec moveSpec{
        u"mouse_move"_s,
        u"Nudge the mouse pointer by a number of pixels from where it is now. For pointing at something you can "
        "see, use mouse_move_abs instead."_s,
        schema::object({{u"dx"_s, schema::integer(u"Pixels right (negative for left)"_s, -10000, 10000)},
                        {u"dy"_s, schema::integer(u"Pixels down (negative for up)"_s, -10000, 10000)}}),
    };
    moveSpec.privileged = true;
    moveSpec.risk = u"moves your mouse pointer"_s;
    registry.add(std::make_unique<FunctionTool>(moveSpec, [backendOr, finish](const QJsonObject &arguments,
                                                                             ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        const int dx = intArg(arguments, u"dx"_s).value_or(0);
        const int dy = intArg(arguments, u"dy"_s).value_or(0);
        if (dx == 0 && dy == 0) {
            done(Error{ErrorCode::InvalidArgument, u"Give dx, dy, or both."_s});
            return;
        }
        backend->mouseMove(dx, dy, finish(u"Moved the pointer by %1, %2."_s.arg(dx).arg(dy), done));
    }));

    const QJsonObject pointerArguments{
        {u"x"_s, schema::integer(u"Horizontal position"_s, 0, 100000)},
        {u"y"_s, schema::integer(u"Vertical position"_s, 0, 100000)},
        {u"space"_s, schema::choice(u"Which pixels x and y are measured in: \"image\" (the last screenshot, the "
                                    "default) or \"screen\""_s, {u"image"_s, u"screen"_s})},
    };

    ToolSpec moveAbsSpec{
        u"mouse_move_abs"_s,
        u"Put the mouse pointer on a particular spot. By default x and y are pixel coordinates in the screenshot "
        "you last took, so you can point straight at something you can see in it."_s,
        schema::object(pointerArguments, {u"x"_s, u"y"_s}),
    };
    moveAbsSpec.privileged = true;
    moveAbsSpec.risk = u"moves your mouse pointer"_s;
    registry.add(std::make_unique<FunctionTool>(moveAbsSpec, [backendOr, finish, capture](const QJsonObject &arguments,
                                                                                         ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        const Result<PointerTarget> target = pointerTarget(arguments, capture);
        if (!target) {
            done(target.error());
            return;
        }
        const PointerTarget &at = target.value();
        backend->mouseMoveAbs(at.x, at.y, at.screen.width(), at.screen.height(),
                              finish(u"Pointer moved to %1, %2 on screen.%3"_s.arg(at.x).arg(at.y).arg(at.note),
                                     done));
    }));

    QJsonObject clickArguments = pointerArguments;
    clickArguments.insert(u"button"_s, schema::choice(u"Which button (default left)"_s,
                                                      {u"left"_s, u"right"_s, u"middle"_s}));
    clickArguments.insert(u"count"_s, schema::integer(u"1 for a single click, 2 to double-click"_s, 1, 3));
    ToolSpec clickSpec{
        u"mouse_click"_s,
        u"Click the mouse. Give x and y to move there first (by default they are coordinates in the screenshot you "
        "last took), or leave them out to click where the pointer already is."_s,
        schema::object(clickArguments),
    };
    clickSpec.privileged = true;
    clickSpec.risk = u"clicks for you"_s;
    registry.add(std::make_unique<FunctionTool>(clickSpec, [backendOr, finish, capture](const QJsonObject &arguments,
                                                                                       ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        const QString buttonName = arguments.value(u"button"_s).toString().trimmed().toLower();
        const int button = buttonName == u"right"_s ? 2 : buttonName == u"middle"_s ? 3 : 1;
        const int count = qBound(1, intArg(arguments, u"count"_s).value_or(1), 3);
        const QString what = u"%1 %2 click"_s.arg(count == 2 ? u"Double"_s : u"Single"_s,
                                                  buttonName.isEmpty() ? u"left"_s : buttonName);

        if (!arguments.contains(u"x"_s) && !arguments.contains(u"y"_s)) {
            backend->mouseClick(button, count, finish(u"%1 where the pointer was."_s.arg(what), done));
            return;
        }
        const Result<PointerTarget> target = pointerTarget(arguments, capture);
        if (!target) {
            done(target.error());
            return;
        }
        const PointerTarget &at = target.value();
        // Move, then click once the pointer has actually arrived.
#ifdef Q_OS_WIN
        const QPointer<IInputBackend> safeBackend(backend);
        backend->mouseMoveAbs(at.x, at.y, at.screen.width(), at.screen.height(),
                              [safeBackend, button, count, at, what, finish, done](Result<QString> moved) {
            if (!moved) {
                done(moved.error());
                return;
            }
            QTimer::singleShot(15, QCoreApplication::instance(), [safeBackend, button, count, at, what, finish, done] {
                if (!safeBackend) {
                    done(Error{ErrorCode::Cancelled, u"Input backend went away."_s});
                    return;
                }
                safeBackend->mouseClick(button, count,
                                        finish(u"%1 at %2, %3.%4"_s.arg(what).arg(at.x).arg(at.y).arg(at.note), done));
            });
        });
#else
        backend->mouseMoveAbs(at.x, at.y, at.screen.width(), at.screen.height(),
                              [backend, button, count, at, what, finish, done](Result<QString> moved) {
            if (!moved) {
                done(moved.error());
                return;
            }
            backend->mouseClick(button, count,
                                finish(u"%1 at %2, %3.%4"_s.arg(what).arg(at.x).arg(at.y).arg(at.note), done));
        });
#endif
    }));

    ToolSpec scrollSpec{
        u"mouse_scroll"_s,
        u"Scroll the wheel over whatever is under the pointer."_s,
        schema::object({{u"dy"_s, schema::integer(u"Steps up; negative scrolls down"_s, -100, 100)},
                        {u"dx"_s, schema::integer(u"Steps right; negative scrolls left"_s, -100, 100)}}),
    };
    scrollSpec.privileged = true;
    scrollSpec.risk = u"scrolls for you"_s;
    registry.add(std::make_unique<FunctionTool>(scrollSpec, [backendOr, finish](const QJsonObject &arguments,
                                                                               ITool::Done done) {
        IInputBackend *backend = backendOr(done);
        if (!backend) return;
        const int dy = intArg(arguments, u"dy"_s).value_or(0);
        const int dx = intArg(arguments, u"dx"_s).value_or(0);
        if (dx == 0 && dy == 0) {
            done(Error{ErrorCode::InvalidArgument, u"Give dy (or dx) as a number of steps."_s});
            return;
        }
        backend->mouseScroll(dx, dy, finish(u"Scrolled %1, %2."_s.arg(dx).arg(dy), done));
    }));
}

} // namespace

void registerSystemTools(ToolRegistry &registry, const SystemServices &services) {
    if (services.apps) addAppTools(registry, services.apps, services);
    if (services.commands) addCommandTools(registry, services.commands);
    if (services.rootCommands) addRootCommandTool(registry, services.rootCommands);
    if (services.audio) addAudioTools(registry, services.audio);
    if (services.media) addMediaTool(registry, services.media);
    if (services.screen) addScreenTool(registry, services.screen);
    if (services.files) addFileTools(registry, services.files);
    if (services.input) addInputTools(registry, services.input, services.screen);
    addInfoTool(registry);
}

} // namespace nexa
