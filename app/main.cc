#include "app/AppController.h"
#include "app/GlobalShortcuts.h"
#include "app/MainWindow.h"
#include "app/PhoneLink.h"
#include "app/RemoteAccess.h"
#include "app/RemoteBridge.h"
#include "app/SingleInstance.h"
#include "app/TrayIcon.h"
#include "app/SystemStatus.h"
#include "app/UnixSignals.h"
#include "core/agent/AppContext.h"
#include "core/Logging.h"
#include "core/ai/ProgramLookup.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/intent/MemoryIntents.h"
#include "core/intent/MissionIntents.h"
#include "core/intent/PcIntents.h"
#include "core/intent/PhoneIntents.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/memory/MemoryStore.h"
#include "core/memory/MemoryTools.h"
#include "core/policy/PolicyEngine.h"
#include "core/skills/SkillCatalog.h"
#include "core/skills/SkillTools.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"
#include "system/SystemTools.h"
#include "system/files/AttachmentLoader.h"
#include "system/office/OfficeTools.h"
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
#include "system/browser/BrowserService.h"
#include "system/browser/PlaywrightInstaller.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/commands/RootCommandRunner.h"
#include "system/helper/HelperClient.h"
#include "system/input/HelperInputLock.h"
#include "system/input/IInputBackend.h"
#include "system/input/InputBackendSelector.h"
#include "system/input/RemoteDesktopSession.h"
#include "system/files/FileAccess.h"
#include "system/info/SystemInfo.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenAwake.h"
#include "system/screen/ScreenCapture.h"
#include "core/mcp/McpConnectors.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"
#include "voice/VoiceTools.h"
#include "voice/Wav.h"
#include "voice/capture/AudioCapture.h"
#include "voice/capture/AudioConverter.h"
#include "voice/capture/RecordedAudioInput.h"
#include "voice/listen/SpeakerGate.h"
#include "voice/pipeline/VoicePipeline.h"
#include "voice/wake/SherpaRuntime.h"

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
    // The programs installed for this user are what a first start offers (and what an upgrade puts
    // into a list an older Nexa left empty). A list the user empties stays empty.
    const auto installed = [](const QString &program) { return !findProgram(program).isEmpty(); };
    if (!QFile::exists(store.path()) && warning.isEmpty()) {
        config.providers = ProviderRegistry::defaultProviders(installed);
        if (!config.providers.isEmpty()) config.activeProvider = config.providers.first().id;
    }
    if (config.version < Config::kVersion && warning.isEmpty()) {
        ProviderRegistry::upgrade(config, installed);
        if (const Result<void> saved = store.save(config); !saved) qCWarning(lcConfig) << saved.error().message;
    }
    return config;
}

// A WAV file → 16 kHz mono PCM, as the microphone would have delivered it.
// --attach paths as the running copy (whose working folder is another) will find them.
QStringList absolutePaths(const QStringList &paths) {
    QStringList absolute;
    for (const QString &path : paths) absolute.append(QFileInfo(path).absoluteFilePath());
    return absolute;
}

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
    const QCommandLineOption attachOption(
        u"attach"_s, u"Give <file> with the first --ask, as if attached in the window (repeatable)."_s, u"file"_s);
    parser.addOption(attachOption);
    const QCommandLineOption voiceFileOption(
        u"voice-file"_s, u"Treat the WAV <file> as a spoken request (repeatable; runs before --ask)."_s, u"file"_s);
    parser.addOption(voiceFileOption);
    const QCommandLineOption sayOption(u"say"_s, u"Say <text> aloud on start-up (tests the voice)."_s, u"text"_s);
    parser.addOption(sayOption);
    const QCommandLineOption hearOption(
        u"hear"_s,
        u"Play the WAV <file> into Nexa as if the microphone heard it, with the wake word on (repeatable; this run "
        "only, in a copy of its own)."_s,
        u"file"_s);
    parser.addOption(hearOption);
    const QCommandLineOption newInstanceOption(
        u"new-instance"_s, u"Start even when Nexa is already running (for testing); this copy takes no requests from later ones."_s);
    parser.addOption(newInstanceOption);
    const QCommandLineOption screenshotOption(
        u"screenshot"_s, u"Save a picture of the window to <file> once the start-up requests are done, then quit."_s,
        u"file"_s);
    parser.addOption(screenshotOption);
    const QCommandLineOption sizeOption(
        u"size"_s, u"Open the window at <width>x<height> for this run (to see a design at a given size)."_s, u"size"_s);
    parser.addOption(sizeOption);
    const QCommandLineOption themeOption(
        u"theme"_s, u"Palette for this run only: system, dark, light, or a palette's id (ocean-dark)."_s, u"palette"_s);
    parser.addOption(themeOption);
    const QCommandLineOption designOption(
        u"design"_s, u"Design for this run only: studio, focus, console, sidebar, orb, dashboard, gauge, paper."_s, u"design"_s);
    parser.addOption(designOption);
    const QCommandLineOption settingsShotsOption(
        u"screenshot-settings"_s, u"Save a picture of every Settings tab into <dir>, then quit."_s, u"dir"_s);
    parser.addOption(settingsShotsOption);
    const QCommandLineOption pairPhoneOption(
        u"pair-phone"_s,
        u"Pair Nexa Phone on the phone connected through adb: a pairing code is started here and handed to the app over adb."_s);
    parser.addOption(pairPhoneOption);
    const QCommandLineOption hardcoreOption(
        u"hardcore"_s,
        u"Switch hardcore mode on without asking: the requests of this run lock the laptop's keyboard and mouse while "
        "they run."_s);
    parser.addOption(hardcoreOption);
    parser.process(app);

    // One Nexa per user: a second start hands what it was asked to do to the running one and
    // leaves, before building anything heavy.
    SingleInstance instance(u"nexa-assistant"_s);
    // --hear replaces the microphone, so it never goes to the copy already running.
    if (!parser.isSet(newInstanceOption) && !parser.isSet(hearOption)) {
        QJsonArray voiceFiles;
        for (const QString &file : parser.values(voiceFileOption)) voiceFiles.append(QFileInfo(file).absoluteFilePath());
        const QJsonObject handOver{{u"say"_s, QJsonArray::fromStringList(parser.values(sayOption))},
                                   {u"voiceFile"_s, voiceFiles},
                                   {u"ask"_s, QJsonArray::fromStringList(parser.values(askOption))},
                                   {u"attach"_s, QJsonArray::fromStringList(absolutePaths(parser.values(attachOption)))},
                                   {u"pairPhone"_s, parser.isSet(pairPhoneOption)},
                                   {u"hardcore"_s, parser.isSet(hardcoreOption)}};
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
    // The look: Nexa's own stylesheet and palette — the design's pair as set (or as the desktop
    // is), or any theme pack from the gallery, with the motion style it wears.
    // --design and --theme are for this run only: they never reach the saved settings. A design
    // was drawn in a palette of its own; a --theme beside it still wins.
    QString runDesign = parser.isSet(designOption) ? parser.value(designOption) : QString();
    QString runPalette = parser.isSet(themeOption) ? parser.value(themeOption) : QString();
    if (!runDesign.isEmpty() && runPalette.isEmpty()) runPalette = NexaDesigns::resolve(runDesign).palette;
    const ThemePack startingTheme = NexaThemes::resolve(runPalette.isEmpty() ? config.ui.theme : runPalette);
    const DesignSpec startingDesign = NexaDesigns::resolve(runDesign.isEmpty() ? config.ui.design : runDesign);
    NexaTheme::apply(&app, startingTheme.tokens, startingTheme.style, startingDesign);

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
    WpctlAudioController microphoneVolume(WpctlAudioController::Target::Microphone);   // "Lower it" only
    // sherpa-onnx (scripts/download-sherpa.sh): the wake-name spotter, Silero and noise reduction, loaded when used.
    SherpaRuntime sherpa;
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
    // Hardcore mode's lock on the laptop's own keyboard and mouse, through the same helper; the screen stays on
    // while it is held.
    ScreenAwake screenAwake;
    HelperInputLock inputLock(&helperClient, &screenAwake);

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

    // The app the user is in — the one Nexa opened last for them — so the request after goes to
    // the right place: a command into the terminal, a document into LibreOffice.
    AppContext appContext;
    const auto appOpened = [&appContext](const OpenedApp &app) { appContext.noteOpened(app); };
    registerSystemTools(tools, SystemServices{.commands = &commands,
                                              .apps = &apps,
                                              .audio = &audio,
                                              .media = &media,
                                              .files = &files,
                                              .screen = &screen,
                                              .rootCommands = &rootCommands,
                                              .input = [&input] { return input.current(); },
                                              .appOpened = appOpened,
                                              .appClosed = [&appContext](const OpenedApp &app) { appContext.noteClosed(app); }});
    // Documents, presentations and spreadsheets as files for LibreOffice.
    registerOfficeTools(tools, OfficeServices{.files = &files, .commands = &commands, .appOpened = appOpened});

    // Voice: microphone → recognition → requests; replies → speech (backends built by AppController).
    // With --hear, recordings stand in for the microphone.
    const QStringList heard = parser.values(hearOption);
    RecordedAudioInput *recording = nullptr;
    std::unique_ptr<IAudioInput> microphone;
    if (heard.isEmpty()) {
        microphone = std::make_unique<AudioCapture>();
    } else {
        auto played = std::make_unique<RecordedAudioInput>();
        recording = played.get();
        microphone = std::move(played);
    }
    VoicePipeline voice(microphone.get());
    // Whose voice it is: hands-free requests go on only in the owner's, once it is enrolled (Settings → Voice).
    SpeakerGate speakerGate(&sherpa);
    voice.setSpeakerGate(&speakerGate);
    registerVoiceTools(tools, &voice);
    panic.addHandler([&voice] { voice.cancel(); });

    // MCP connectors (Settings → Connectors): each server's tools join the registry as
    // mcp_<server>_<tool> once it answers, and leave with it.
    McpConnectors mcp(&tools, &http);
    mcp.apply(config.mcp.servers);

    // The browser that is part of Nexa: Playwright, installed on first start into
    // ~/.local/share/nexa/playwright, working in the user's own Chrome through its extension — or
    // in a Chrome with Nexa's own profile until that is set up (Settings → Browser). Its tools are
    // browser_navigate, browser_snapshot, …: text, so they work for a model that cannot see.
    PlaywrightInstaller playwright;
    BrowserService browser(&tools, &http, &playwright);
    browser.apply(config.browser);

    // What Nexa remembers about the user, and the skills the model may load (Settings → Memory,
    // Settings → Skills); both come back in the system prompt.
    MemoryStore memory;
    if (const Result<void> loaded = memory.load(); !loaded) qCWarning(lcConfig).noquote() << loaded.error().message;
    syncMemoryTools(tools, &memory, config.memory);
    SkillCatalog skills;
    syncSkillTools(tools, &skills, config.skills);

    // Fast path (no model) for common phrases; order = priority
    IntentRouter router([&tools](const QString &tool) { return tools.contains(tool); });
    router.add(std::make_unique<ControlIntent>());
    router.add(std::make_unique<TaskIntent>());      // the mission board: "pause task two", "approve", "status"
    router.add(std::make_unique<MemoryIntent>());    // after the stops: "forget it" stays one
    router.add(std::make_unique<PhoneIntent>());     // before the PC's: "open youtube on my phone"
    router.add(std::make_unique<MediaIntent>());
    router.add(std::make_unique<AudioIntent>());
    router.add(std::make_unique<ScreenIntent>());
    router.add(std::make_unique<AppIntent>(&apps));
    router.add(std::make_unique<CommandIntent>(&apps, programExists));
    router.add(std::make_unique<InputIntent>());


    AppController controller(
        AppServices{.store = &store,
                    .providers = &providers,
                    .tools = &tools,
                    .executor = &executor,
                    .router = &router,
                    .panic = &panic,
                    .policy = &policy,
                    .voice = &voice,
                    .http = &http,
                    .microphoneVolume = &microphoneVolume,
                    .sherpa = &sherpa,
                    .speakerGate = &speakerGate,
                    .context = &appContext,
                    // Typing goes to the window with the keyboard: when that is Nexa's own, it
                    // must not be done.
                    .windowActive = [] { return QApplication::activeWindow() != nullptr; },
                    .canType = [&input] { return input.current() != nullptr; },
                    .memory = &memory,
                    .skills = &skills,
                    .browserTarget = [&browser] { return browser.targetPhrase(); },
                    .inputLock = &inputLock},
        std::move(config), SystemInfo::summary());
    QObject::connect(&browser, &BrowserService::notice, &controller, &AppController::notify);

    // The web remote: a phone's browser talks to Nexa. Off until Settings → Remote switches it on;
    // then every request it makes goes through the controller like one typed here.
    PairingManager pairing(PairingManager::defaultPath());
    TlsIdentity tlsIdentity(TlsIdentity::defaultDirectory());
    RemoteBridge remoteBridge(&controller, &http);
    controller.setPhonePaired([&pairing] { return !pairing.devices().isEmpty(); });
    RemoteServer remote(&pairing, &remoteBridge, &tlsIdentity);
    // Away from home: Tailscale watched, the Cloudflare and ngrok tunnels run as Settings → Remote says.
    RemoteAccess remoteAccess(&remote);
    QObject::connect(&controller, &AppController::userMessageAccepted, &remote, &RemoteServer::onUserMessage);
    // Hardcore mode's lock and the questions tasks wait on: the phone shows them, unlocks and answers.
    if (HardcoreMode *hardcore = controller.hardcore()) {
        QObject::connect(hardcore, &HardcoreMode::stateChanged, &remote, [&remote, &remoteBridge] {
            remote.onLockChanged(remoteBridge.isLocked(), remoteBridge.lockText());
        });
    }
    QObject::connect(controller.mission(), &MissionControl::approvalRequested, &remote,
                     [&remote, &controller](quint64, quint64 actionId) {
        for (const Approval &approval : controller.mission()->approvals()) {
            if (approval.actionId == actionId) {
                remote.onApprovalAsked(actionId, approval.agentId, approval.tool, approval.question);
            }
        }
    });
    QObject::connect(controller.mission(), &MissionControl::approvalSettled, &remote,
                     [&remote](quint64, quint64 actionId, bool allowed) { remote.onApprovalSettled(actionId, allowed); });
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
    const auto applyRemote = [&remote, &remoteBridge, &remoteAccess, &phone](const Config &updated) {
        remoteBridge.applyConfig(updated);
        remoteAccess.apply(updated.remote);
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
                     [applyAndroid, applyRemote, &mcp, &browser, &tools, &memory, &skills](const Config &updated) {
                         applyAndroid(updated.android);
                         applyRemote(updated);
                         mcp.apply(updated.mcp.servers);
                         browser.apply(updated.browser);
                         syncMemoryTools(tools, &memory, updated.memory);
                         syncSkillTools(tools, &skills, updated.skills);
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
                                   .remoteAccess = &remoteAccess,
                                   .pairing = &pairing,
                                   .phoneLink = &phoneLink,
                                   .mcp = &mcp,
                                   .shortcuts = &shortcuts,
                                   .browser = &browser,
                                   .memory = &memory,
                                   .skills = &skills,
                                   .sherpa = &sherpa,
                                   .voice = &voice,
                                   .speakerGate = &speakerGate,
                                   .microphoneSummary =
                                       [&voice] {
                                           const QString trouble = voice.microphoneTrouble();
                                           return trouble.isEmpty() ? QObject::tr("fine")
                                                                    : trouble.section(u". "_s, 0, 0) + u'.';
                                       },
                                   .lowerMicrophone = [&controller] { controller.lowerMicrophone(); },
                                   .replyRunning = [&controller] { return controller.isBusy(); },
                                   .providersChanged = [&controller] { controller.reloadProviders(); }},
                      AuditLog::defaultPath());
    if (!runDesign.isEmpty() || !runPalette.isEmpty()) window.wear(runPalette, runDesign);
    if (parser.isSet(sizeOption)) {
        const QStringList parts = parser.value(sizeOption).split(u'x');
        if (parts.size() == 2 && parts.at(0).toInt() > 0 && parts.at(1).toInt() > 0) {
            window.resize(parts.at(0).toInt(), parts.at(1).toInt());
        }
    }
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
    if (parser.isSet(hardcoreOption)) window.setHardcore(true, false);
    if (recording) controller.setWakeWordForced(true);     // --hear: the room is listened to, this run only
    QTimer::singleShot(0, &controller, &AppController::start);

    // Scripted requests (--say, --voice-file, --ask, --hear, from this start or handed over by a later
    // one): one step at a time, each once Nexa is idle again — reply finished and spoken, recording played.
    QList<std::function<void()>> steps;
    // --screenshot: once everything queued is done and Nexa is idle again, a picture of the
    // window (it also works offscreen: QT_QPA_PLATFORM=offscreen), then quit.
    const QString screenshotPath = parser.value(screenshotOption);
    bool screenshotTaken = false;
    const auto takeScreenshot = [&window, screenshotPath, &screenshotTaken] {
        if (screenshotPath.isEmpty() || screenshotTaken) return;
        screenshotTaken = true;
        QTimer::singleShot(700, &window, [&window, screenshotPath] {
            qCInfo(lcApp) << "window" << window.size() << "smallest" << window.minimumSizeHint();
            const bool saved = window.grab().save(screenshotPath);
            qCInfo(lcApp).noquote() << (saved ? u"Saved %1"_s.arg(screenshotPath) : u"Could not save %1"_s.arg(screenshotPath));
            QCoreApplication::quit();
        });
    };
    std::function<void()> runNext = [&controller, &voice, &steps, takeScreenshot, recording] {
        if (controller.isBusy() || voice.state() != VoiceState::Idle) return;
        if (recording && recording->playing()) return;
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
    if (recording) QObject::connect(recording, &RecordedAudioInput::drained, &controller, later);
    const auto queue = [&](const QStringList &say, const QStringList &voiceFiles, const QStringList &ask,
                           const QStringList &attach, int delayMs) {
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
        QStringList files = attach;         // --attach: with the first request
        for (const QString &text : ask) {
            // A command about Nexa itself ("unlock", "stop everything", "pause all") acts at once: queued behind the
            // task it is about, it would wait for that task to end.
            if (const std::optional<IntentMatch> match = router.route(text);
                match && (match->panic || match->control != Control::None)) {
                QTimer::singleShot(0, &controller, [&controller, text] { controller.submit(text); });
                continue;
            }
            if (files.isEmpty()) {
                steps.append([&controller, text] { controller.submit(text); });
                continue;
            }
            // Read here and now (a scripted run; the window reads them on a worker): the request follows at once,
            // so the next step waits for it like for any other.
            steps.append([&controller, &window, text, files = std::exchange(files, {})] {
                QList<PreparedAttachment> prepared;
                for (const QString &path : files) {
                    const Result<Attachment> file = attachments::describe(path);
                    if (!file) {
                        QMessageBox::warning(&window, QObject::tr("Nexa"), file.error().message);
                        continue;
                    }
                    prepared.append(AttachmentLoader::prepareOne(file.value(), AttachmentLoader::defaultKeepDir(),
                                                                 QStandardPaths::findExecutable(u"soffice"_s),
                                                                 QStandardPaths::findExecutable(u"pdftotext"_s)));
                }
                controller.submit(text, u"gui"_s, prepared);
            });
        }
        if (!steps.isEmpty()) QTimer::singleShot(delayMs, runNext);
    };
    queue(parser.values(sayOption), parser.values(voiceFileOption), parser.values(askOption),
          absolutePaths(parser.values(attachOption)), 800);
    // --hear: each recording played into the microphone, as if the room said it.
    for (const QString &file : heard) {
        steps.append([recording, &window, file] {
            const Result<QByteArray> pcm = loadRecording(file);
            if (pcm) {
                recording->play(pcm.value());
            } else {
                QMessageBox::warning(&window, QObject::tr("Nexa"), QObject::tr("Cannot read %1: %2").arg(file, pcm.error().message));
            }
        });
    }
    if (!heard.isEmpty()) QTimer::singleShot(1500, runNext);
    if (steps.isEmpty() && !screenshotPath.isEmpty()) QTimer::singleShot(1500, runNext);
    if (parser.isSet(settingsShotsOption)) {
        const QString directory = parser.value(settingsShotsOption);
        // Quits once the pictures are taken: a slow start (loading a whisper model) must not cut them short.
        QTimer::singleShot(1500, &window, [&window, directory] { window.snapshotSettings(directory, [] { QCoreApplication::quit(); }); });
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
        const QStringList say = strings(u"say"_s), voiceFiles = strings(u"voiceFile"_s), asks = strings(u"ask"_s),
                          attach = strings(u"attach"_s);
        const bool pairThePhone = message.value(u"pairPhone"_s).toBool();
        if (message.value(u"hardcore"_s).toBool()) window.setHardcore(true, false);
        if (say.isEmpty() && voiceFiles.isEmpty() && asks.isEmpty() && !pairThePhone) {
            window.show();
            window.raise();
            window.activateWindow();
            return;
        }
        if (!window.isVisible()) window.show();
        if (pairThePhone) pairPhone();
        queue(say, voiceFiles, asks, attach, 250);
    });
    return app.exec();
}
