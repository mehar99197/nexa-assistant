#include "system/SystemTools.h"

#include "core/tools/FunctionTool.h"
#include "core/tools/ToolRegistry.h"
#include "system/apps/AppLauncher.h"
#include "system/apps/DesktopEntryIndex.h"
#include "system/audio/WpctlAudioController.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/files/FileAccess.h"
#include "system/info/SystemInfo.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenCapture.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kDefaultTimeoutSeconds = 30;
constexpr int kMaxTimeoutSeconds = 600;

// Models sometimes send numbers and booleans as strings; accept both.
std::optional<int> intArg(const QJsonObject &arguments, const QString &key) {
    const QJsonValue value = arguments.value(key);
    if (value.isDouble()) return int(value.toDouble());
    bool ok = false;
    const int parsed = value.toString().trimmed().toInt(&ok);
    return ok ? std::optional<int>(parsed) : std::nullopt;
}

std::optional<bool> boolArg(const QJsonObject &arguments, const QString &key) {
    const QJsonValue value = arguments.value(key);
    if (value.isBool()) return value.toBool();
    const QString text = value.toString().trimmed().toLower();
    if (text == u"true" || text == u"yes" || text == u"1") return true;
    if (text == u"false" || text == u"no" || text == u"0") return false;
    return std::nullopt;
}

Result<QString> requiredString(const QJsonObject &arguments, const QString &key) {
    const QString value = arguments.value(key).toString().trimmed();
    if (value.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Missing \"%1\""_s.arg(key)};
    return value;
}

ToolResult text(const QString &value) {
    return ToolResult{value, std::nullopt};
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
        u"Run a shell command on the user's PC (bash, as the user, in the home folder unless working_directory "
        "is set) and return its exit code and output. Only for commands that finish on their own; never for "
        "graphical apps (use open_app). Set background=true to start a server or long-running program without "
        "waiting for it."_s,
        schema::object({{u"command"_s, schema::string(u"The command line, e.g. \"df -h\" or \"ls ~/Downloads | head\""_s)},
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

void addAppTools(ToolRegistry &registry, DesktopEntryIndex *apps) {
    registry.add(FunctionTool::sync(
        ToolSpec{u"open_app"_s,
                 u"Open (launch) an installed desktop application by name, e.g. \"firefox\", \"files\", "
                 "\"terminal\", \"text editor\", \"settings\", \"calculator\". Names are matched fuzzily."_s,
                 schema::object({{u"name"_s, schema::string(u"Application name"_s)}}, {u"name"_s})},
        [apps](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> name = requiredString(arguments, u"name"_s);
            if (!name) return name.error();
            const Result<DesktopEntryIndex::Match> app = findApp(apps, name.value());
            if (!app) return app.error();
            const Result<QString> launched = AppLauncher::launch(app.value().entry);
            if (!launched) return launched.error();
            return text(launched.value());
        }));

    registry.add(FunctionTool::sync(
        ToolSpec{u"close_app"_s, u"Close a running application by name (asks it to quit with SIGTERM)."_s,
                 schema::object({{u"name"_s, schema::string(u"Application name"_s)}}, {u"name"_s})},
        [apps](const QJsonObject &arguments) -> Result<ToolResult> {
            const Result<QString> name = requiredString(arguments, u"name"_s);
            if (!name) return name.error();
            const Result<DesktopEntryIndex::Match> app = findApp(apps, name.value());
            if (!app) return app.error();
            const Result<QString> closed = AppLauncher::close(app.value().entry);
            if (!closed) return closed.error();
            return text(closed.value());
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

} // namespace

void registerSystemTools(ToolRegistry &registry, const SystemServices &services) {
    if (services.apps) addAppTools(registry, services.apps);
    if (services.commands) addCommandTools(registry, services.commands);
    if (services.audio) addAudioTools(registry, services.audio);
    if (services.media) addMediaTool(registry, services.media);
    if (services.screen) addScreenTool(registry, services.screen);
    if (services.files) addFileTools(registry, services.files);
    addInfoTool(registry);
}

} // namespace nexa
