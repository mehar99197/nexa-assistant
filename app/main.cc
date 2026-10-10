#include "app/AppController.h"
#include "app/GlobalShortcuts.h"
#include "app/MainWindow.h"
#include "app/PhoneLink.h"
#include "app/RemoteBridge.h"
#include "app/SingleInstance.h"
#include "app/TrayIcon.h"
#include "app/SystemStatus.h"
#include "app/UnixSignals.h"
#include "core/agent/Agent.h"
#include "core/Logging.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/intent/PcIntents.h"
#include "core/intent/PhoneIntents.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"
#include "system/SystemTools.h"
#include "remote/PairingManager.h"
#include "remote/RemoteServer.h"
#include "remote/TlsIdentity.h"
#include "system/android/AdbClient.h"
#include "system/android/AdbDiscovery.h"
#include "system/android/MdnsBrowser.h"
#include "system/android/AndroidDevice.h"
#include "system/android/AndroidTools.h"
#include "system/apps/DesktopEntryIndex.h"
#include "system/audio/WpctlAudioController.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/commands/RootCommandRunner.h"
#include "system/helper/HelperClient.h"
#include "system/input/IInputBackend.h"
#include "system/input/InputBackendSelector.h"
#include "system/input/RemoteDesktopSession.h"
#include "system/files/FileAccess.h"
#include "system/info/SystemInfo.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenCapture.h"
#include "ui/NexaTheme.h"
#include "voice/VoiceTools.h"
#include "voice/Wav.h"
#include "voice/capture/AudioCapture.h"
#include "voice/capture/AudioConverter.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <functional>

using namespace Qt::StringLiterals;
using namespace nexa;

namespace {

// Loads the settings; an unreadable file is moved aside (never silently overwritten).
Config loadConfig(const ConfigStore &store, QString &warning) {
    Config config;
    if (Result<Config> loaded = store.load(); loaded) {
        config = std::move(loaded).value();
    } else if (const Result<QString> moved = store.moveAside(); moved) {
        warning = QObject::tr("Your settings file could not be read (%1).\n\nIt was moved to %2 and Nexa started "
                              "with default settings.")
                      .arg(loaded.error().message, moved.value());
    } else {
        warning = QObject::tr("Your settings file could not be read (%1) and could not be moved aside. Saving "
                              "settings will overwrite it.")
                      .arg(loaded.error().message);
    }
    if (config.providers.isEmpty()) {
        config.providers.append(ProviderRegistry::defaultProvider());
        config.activeProvider = config.providers.first().id;
    }
    if (config.version < Config::kVersion && warning.isEmpty()) {
        ProviderRegistry::upgrade(config);
        if (const Result<void> saved = store.save(config); !saved) qCWarning(lcConfig) << saved.error().message;
    }
    return config;
}

// A WAV file → 16 kHz mono PCM, as the microphone would have delivered it.
Result<QByteArray> loadRecording(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Error{ErrorCode::NotFound, file.errorString()};
    const Result<PcmAudio> audio = wav::decode(file.readAll());
    if (!audio) return audio.error();
    QAudioFormat format;
    format.setSampleRate(audio.value().sampleRate);
    format.setChannelCount(audio.value().channels);
    format.setSampleFormat(QAudioFormat::Int16);
    AudioConverter converter(format);
    return converter.convert(audio.value().samples);
}

// Which backend types and clicks can change while Nexa runs — installing the helper switches it
// from the portal to uinput. Retired backends are kept alive, because a tool call already in flight
// may still be holding one.
class InputBackends {
public:
    InputBackends(HelperClient *helper, RemoteDesktopSession *remoteDesktop)
        : helper_(helper), remoteDesktop_(remoteDesktop) {
        reselect();
    }

    IInputBackend *current() const { return current_; }
    QString summary() const { return summary_; }
    QStringList notes() const { return notes_; }

    void reselect() {
        InputBackendSelector::Choice choice = InputBackendSelector::choose(helper_, remoteDesktop_);
        summary_ = choice.summary;
        notes_ = choice.notes;
        current_ = choice.backend.get();
        if (choice.backend) kept_.push_back(std::move(choice.backend));
    }

private:
    HelperClient *helper_;
    RemoteDesktopSession *remoteDesktop_;
    std::vector<std::unique_ptr<IInputBackend>> kept_;
    IInputBackend *current_{nullptr};
    QString summary_;
    QStringList notes_;
};

bool programExists(const QString &program) {
    if (program.contains(u'/')) return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

} // namespace

// Composition root: every object is created here and wired by constructor injection.
// Declaration order matters: users are declared after (and so destroyed before) what they use.
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(u"nexa"_s);
    QApplication::setApplicationVersion(QString::fromLatin1(NEXA_VERSION));
    // Never "nexa": that desktop id (and /usr/bin/nexa) belongs to Nexa Download Manager. The
    // reverse-DNS form is also what the portal needs to grant global hotkeys.
    QApplication::setDesktopFileName(u"org.nexa.Assistant"_s);
    // The installed theme icon when there is one, else the copy built into Nexa.
    QApplication::setWindowIcon(QIcon::fromTheme(u"org.nexa.Assistant"_s, QIcon(u":/nexa/nexa.svg"_s)));

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Nexa — AI assistant for the Linux desktop"_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption askOption({u"a"_s, u"ask"_s},
                                       u"Send <text> on start-up as if it were typed (repeatable; runs in order)."_s,
                                       u"text"_s);
    parser.addOption(askOption);
    const QCommandLineOption voiceFileOption(
        u"voice-file"_s, u"Treat the WAV <file> as a spoken request (repeatable; runs before --ask)."_s, u"file"_s);
    parser.addOption(voiceFileOption);
    const QCommandLineOption sayOption(u"say"_s, u"Say <text> aloud on start-up (tests the voice)."_s, u"text"_s);
    parser.addOption(sayOption);
    const QCommandLineOption newInstanceOption(
        u"new-instance"_s, u"Start even when Nexa is already running (for testing); this copy takes no requests from later ones."_s);
    parser.addOption(newInstanceOption);
    const QCommandLineOption screenshotOption(
        u"screenshot"_s, u"Save a picture of the window to <file> once the start-up requests are done, then quit."_s,
        u"file"_s);
    parser.addOption(screenshotOption);
    const QCommandLineOption themeOption(u"theme"_s, u"Look for this run only: system, dark or light."_s, u"scheme"_s);
    parser.addOption(themeOption);
    const QCommandLineOption settingsShotsOption(
        u"screenshot-settings"_s, u"Save a picture of every Settings tab into <dir>, then quit."_s, u"dir"_s);
    parser.addOption(settingsShotsOption);
    const QCommandLineOption pairPhoneOption(
        u"pair-phone"_s,
        u"Pair Nexa Phone on the phone connected through adb: a pairing code is started here and handed to the app over adb."_s);
    parser.addOption(pairPhoneOption);
    parser.process(app);

    // One Nexa per user: a second start hands what it was asked to do to the running one and
    // leaves, before building anything heavy.
    SingleInstance instance(u"nexa-assistant"_s);
    if (!parser.isSet(newInstanceOption)) {
        QJsonArray voiceFiles;
        for (const QString &file : parser.values(voiceFileOption)) voiceFiles.append(QFileInfo(file).absoluteFilePath());
        const QJsonObject handOver{{u"say"_s, QJsonArray::fromStringList(parser.values(sayOption))},
                                   {u"voiceFile"_s, voiceFiles},
                                   {u"ask"_s, QJsonArray::fromStringList(parser.values(askOption))},
                                   {u"pairPhone"_s, parser.isSet(pairPhoneOption)}};
        if (instance.handOver(handOver)) {
            qCInfo(lcApp) << "Nexa is already running: handed over to it";
            return 0;
        }
        instance.listen();
    }

    UnixSignals unixSignals;
    QObject::connect(&unixSignals, &UnixSignals::received, &app, [](int signal) {
        qCInfo(lcApp) << "Signal" << signal << "- quitting";
        QCoreApplication::quit();
    });

    ConfigStore store;
    QString startupWarning;
    Config config = loadConfig(store, startupWarning);
    // The look: Nexa's own stylesheet and palette, dark or light as set (or as the desktop is).
    if (parser.isSet(themeOption)) config.ui.theme = parser.value(themeOption);
    NexaTheme::apply(&app, NexaTheme::schemeFor(config.ui.theme));

    // AI providers
    HttpClient http;
    ProviderRegistry providers(&http);

    // Tools, policy, audit
    ToolRegistry tools;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor(&tools, &policy, &audit);
    PanicController panic;
    panic.addHandler([&executor] { executor.cancelAll(); });

    // PC services behind the tools
    LocalCommandRunner commands;
    DesktopEntryIndex apps;
    apps.refresh();
    WpctlAudioController audio;
    MprisController media;
    FileAccess files;
    ScreenCapture screen;

    // Root commands and input injection go through nexa-helper, which is started by D-Bus only
    // when one of them is actually used, and authorises every call against polkit. Putting the
    // pointer on a given spot goes through the desktop's RemoteDesktop portal instead, which
    // GNOME asks the user to allow once.
    HelperClient helperClient;
    RootCommandRunner rootCommands(&helperClient);
    RemoteDesktopSession remoteDesktop(QApplication::desktopFileName(), RemoteDesktopSession::defaultTokenPath());
    InputBackends input(&helperClient, &remoteDesktop);
    panic.addHandler([&remoteDesktop] { remoteDesktop.close(); });
    QObject::connect(&helperClient, &HelperClient::readyChanged, &app, [&input] { input.reselect(); });
    panic.addHandler([&helperClient] { helperClient.cancelAllJobs(); });

    // The phone, through adb: over USB, or over Wi-Fi once paired in Settings → Android. Nothing
    // here starts adb until a phone is remembered or a phone tool is used.
    AdbClient adb;
    AndroidDevice phone(&adb);
    MdnsBrowser mdns;                   // finds the phone's wireless debugging even where adb cannot
    AdbDiscovery discovery(&adb, &mdns);
    registerAndroidTools(tools, &phone, &files);
    panic.addHandler([&phone] { phone.cancelAll(); });
    const auto applyAndroid = [&phone, &discovery](const AndroidConfig &android) {
        phone.setPreferred(android.serial);
        const bool reconnect = android.autoReconnect && !android.serial.isEmpty();
        discovery.setAutoConnect(reconnect ? android.serial : QString());
        discovery.setBackground(reconnect);
    };
    applyAndroid(config.android);
    // Whatever discovery notices, the phone in use is looked up again, so the status bar follows a
    // phone that comes or goes without any tool running.
    QObject::connect(&discovery, &AdbDiscovery::changed, &phone, [&phone] {
        phone.forgetDevices();
        phone.withDevice([](Result<AdbDevice>) {});
    });

    registerSystemTools(tools, SystemServices{.commands = &commands,
                                              .apps = &apps,
                                              .audio = &audio,
                                              .media = &media,
                                              .files = &files,
                                              .screen = &screen,
                                              .rootCommands = &rootCommands,
                                              .input = [&input] { return input.current(); }});

    // Voice: microphone → recognition → requests; replies → speech (backends built by AppController)
    AudioCapture microphone;
    VoicePipeline voice(&microphone);
    registerVoiceTools(tools, &voice);
    panic.addHandler([&voice] { voice.cancel(); });

    // Fast path (no model) for common phrases; order = priority
    IntentRouter router([&tools](const QString &tool) { return tools.contains(tool); });
    router.add(std::make_unique<ControlIntent>());
    router.add(std::make_unique<PhoneIntent>());     // before the PC's: "open youtube on my phone"
    router.add(std::make_unique<MediaIntent>());
    router.add(std::make_unique<AudioIntent>());
    router.add(std::make_unique<ScreenIntent>());
    router.add(std::make_unique<AppIntent>(&apps));
    router.add(std::make_unique<CommandIntent>(&apps, programExists));
    router.add(std::make_unique<InputIntent>());

    Agent agent(&executor, &tools);
    agent.setMaxSteps(config.agent.maxToolRounds);

    AppController controller(
        AppServices{&store, &providers, &tools, &executor, &router, &agent, &panic, &policy, &voice, &http},
        std::move(config), SystemInfo::summary());

    // The web remote: a phone's browser talks to Nexa. Off until Settings → Remote switches it on;
    // then every request it makes goes through the controller like one typed here.
    PairingManager pairing(PairingManager::defaultPath());
    TlsIdentity tlsIdentity(TlsIdentity::defaultDirectory());
    RemoteBridge remoteBridge(&controller, &http);
    RemoteServer remote(&pairing, &remoteBridge, &tlsIdentity);
    QObject::connect(&controller, &AppController::userMessageAccepted, &remote, &RemoteServer::onUserMessage);
    QObject::connect(&controller, &AppController::replyStarted, &remote, &RemoteServer::onReplyStarted);
    QObject::connect(&controller, &AppController::replyDelta, &remote, &RemoteServer::onReplyDelta);
    QObject::connect(&controller, &AppController::replyStep, &remote, &RemoteServer::onReplyStep);
    QObject::connect(&controller, &AppController::replyFailed, &remote, &RemoteServer::onReplyFailed);
    QObject::connect(&controller, &AppController::busyChanged, &remote, &RemoteServer::onBusyChanged);
    QObject::connect(&controller, &AppController::statusChanged, &remote, &RemoteServer::onStatus);
    // The phone draws the same core as the PC: its state and the satellites an action lights.
    const auto tellCore = [&controller, &remote] {
        QStringList nodes;
        const CoreNodes lit = controller.litNodes();
        for (const CoreNode node : {VoiceNode, PcNode, ToolsNode, PhoneNode}) {
            if (lit & node) nodes.append(coreNodeName(node).toLower());
        }
        remote.onCoreChanged(coreModeKey(controller.coreMode()), nodes);
    };
    QObject::connect(&controller, &AppController::coreModeChanged, &remote, tellCore);
    QObject::connect(&controller, &AppController::litNodesChanged, &remote, tellCore);
    QObject::connect(&controller, &AppController::replyFinished, &remote, [&remote](FinishReason reason, bool) {
        remote.onReplyFinished(reason == FinishReason::Stop        ? u"stop"_s
                               : reason == FinishReason::Cancelled ? u"cancelled"_s
                                                                   : u"other"_s);
    });
    const auto applyRemote = [&remote, &remoteBridge, &phone](const Config &updated) {
        remoteBridge.applyConfig(updated);
        if (!updated.remote.enabled) {
            remote.stop();
            return;
        }
        phone.setReversePort(updated.remote.port + 1);
        if (remote.isRunning() && remote.port() == updated.remote.port) return;
        remote.start(updated.remote.port, [](Result<QString> started) {
            qCInfo(lcApp).noquote() << (started ? started.value() : started.error().message);
        });
    };
    applyRemote(controller.config());

    QObject::connect(&controller, &AppController::configChanged, &app,
                     [applyAndroid, applyRemote, &agent](const Config &updated) {
                         agent.setMaxSteps(updated.agent.maxToolRounds);
                         applyAndroid(updated.android);
                         applyRemote(updated);
                     });

    // Nexa Phone paired over adb (--pair-phone, Settings → Android): the web remote is switched on
    // if it is not, the code is made here and carried to the app by adb, and the outcome is told.
    PhoneLink phoneLink(&phone, &pairing, &remote);
    QObject::connect(&phoneLink, &PhoneLink::progress, &controller,
                     [&controller](const QString &message) { controller.report(message, false); });
    const auto pairPhone = [&controller, &phoneLink] {
        if (!controller.config().remote.enabled) {
            Config enabled = controller.config();
            enabled.remote.enabled = true;
            controller.updateConfig(enabled);
        }
        phoneLink.pair([&controller](Result<QString> result) {
            controller.report(result ? result.value() : result.error().message, !result.isOk());
        });
    };
    // Hotkeys that work from any window. The compositor grants them (GNOME asks once) and the
    // user can change them in Settings → Keyboard; without the portal, the in-window shortcuts
    // (Ctrl+M, Esc, Ctrl+Alt+Esc) still work.
    GlobalShortcuts shortcuts(QApplication::desktopFileName());
    MainWindow window(&controller, &providers, &executor, &http, &shortcuts,
                      SystemStatus{.helper = &helperClient,
                                   .inputSummary = [&input] { return input.summary(); },
                                   .inputNotes = [&input] { return input.notes(); },
                                   .phone = &phone,
                                   .discovery = &discovery,
                                   .remote = &remote,
                                   .pairing = &pairing,
                                   .phoneLink = &phoneLink,
                                   .shortcuts = &shortcuts},
                      AuditLog::defaultPath());
    window.show();
    // Nexa in the top bar where the desktop shows tray icons; then closing the window only hides
    // it, and the hotkeys, wake word and web remote keep working.
    TrayIcon tray(&window, &controller);
    if (tray.isVisible()) {
        QApplication::setQuitOnLastWindowClosed(false);
        window.setCloseToTray([&tray] {
            tray.noteHidden();
            return true;
        });
    }
    shortcuts.bind({
        {u"talk"_s, QObject::tr("Talk to Nexa (hold to speak)"), u"CTRL+ALT+space"_s},
        {u"panic"_s, QObject::tr("Stop everything Nexa is doing"), u"CTRL+ALT+Escape"_s},
    });

    if (!startupWarning.isEmpty()) QMessageBox::warning(&window, QObject::tr("Nexa settings"), startupWarning);
    QTimer::singleShot(0, &controller, &AppController::start);

    // Scripted requests (--say, --voice-file, --ask, from this start or handed over by a later
    // one): one step at a time, each once Nexa is idle again — reply finished and spoken.
    QList<std::function<void()>> steps;
    // --screenshot: once everything queued is done and Nexa is idle again, a picture of the
    // window (it also works offscreen: QT_QPA_PLATFORM=offscreen), then quit.
    const QString screenshotPath = parser.value(screenshotOption);
    bool screenshotTaken = false;
    const auto takeScreenshot = [&window, screenshotPath, &screenshotTaken] {
        if (screenshotPath.isEmpty() || screenshotTaken) return;
        screenshotTaken = true;
        QTimer::singleShot(700, &window, [&window, screenshotPath] {
            const bool saved = window.grab().save(screenshotPath);
            qCInfo(lcApp).noquote() << (saved ? u"Saved %1"_s.arg(screenshotPath) : u"Could not save %1"_s.arg(screenshotPath));
            QCoreApplication::quit();
        });
    };
    std::function<void()> runNext = [&controller, &voice, &steps, takeScreenshot] {
        if (controller.isBusy() || voice.state() != VoiceState::Idle) return;
        if (!steps.isEmpty()) {
            steps.takeFirst()();
        } else {
            takeScreenshot();
        }
    };
    const auto later = [&runNext] { QTimer::singleShot(250, runNext); };
    QObject::connect(&controller, &AppController::busyChanged, &controller, [later](bool busy) {
        if (!busy) later();
    });
    QObject::connect(&voice, &VoicePipeline::stateChanged, &controller, [later](VoiceState state) {
        if (state == VoiceState::Idle) later();
    });
    const auto queue = [&](const QStringList &say, const QStringList &voiceFiles, const QStringList &ask, int delayMs) {
        for (const QString &text : say) steps.append([&voice, text] { voice.speak(text); });
        for (const QString &file : voiceFiles) {
            steps.append([&voice, &window, file] {
                const Result<QByteArray> recording = loadRecording(file);
                if (recording) {
                    voice.submitRecording(recording.value());
                } else {
                    QMessageBox::warning(&window, QObject::tr("Nexa"),
                                         QObject::tr("Cannot read %1: %2").arg(file, recording.error().message));
                }
            });
        }
        for (const QString &text : ask) steps.append([&controller, text] { controller.submit(text); });
        if (!steps.isEmpty()) QTimer::singleShot(delayMs, runNext);
    };
    queue(parser.values(sayOption), parser.values(voiceFileOption), parser.values(askOption), 800);
    if (steps.isEmpty() && !screenshotPath.isEmpty()) QTimer::singleShot(1500, runNext);
    if (parser.isSet(settingsShotsOption)) {
        const QString directory = parser.value(settingsShotsOption);
        QTimer::singleShot(1500, &window, [&window, directory] { window.snapshotSettings(directory); });
        QTimer::singleShot(3500, &app, &QCoreApplication::quit);
    }
    if (parser.isSet(pairPhoneOption)) QTimer::singleShot(2000, &controller, pairPhone);

    // A later start: take over its requests. Without any, it means "show me Nexa" and the window
    // comes forward; with some, it is a script or a shortcut at work, and the window only comes
    // out of hiding — it must not take the focus from what the user is doing.
    QObject::connect(&instance, &SingleInstance::messageReceived, &window, [&](const QJsonObject &message) {
        const auto strings = [&message](const QString &key) {
            QStringList values;
            for (const QJsonValue &value : message.value(key).toArray()) values.append(value.toString());
            return values;
        };
        const QStringList say = strings(u"say"_s), voiceFiles = strings(u"voiceFile"_s), asks = strings(u"ask"_s);
        const bool pairThePhone = message.value(u"pairPhone"_s).toBool();
        if (say.isEmpty() && voiceFiles.isEmpty() && asks.isEmpty() && !pairThePhone) {
            window.show();
            window.raise();
            window.activateWindow();
            return;
        }
        if (!window.isVisible()) window.show();
        if (pairThePhone) pairPhone();
        queue(say, voiceFiles, asks, 250);
    });
    return app.exec();
}
