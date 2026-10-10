#include "app/AppController.h"
#include "app/GlobalShortcuts.h"
#include "app/MainWindow.h"
#include "app/UnixSignals.h"
#include "core/agent/Agent.h"
#include "core/Logging.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/intent/PcIntents.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"
#include "system/SystemTools.h"
#include "system/apps/DesktopEntryIndex.h"
#include "system/audio/WpctlAudioController.h"
#include "system/commands/LocalCommandRunner.h"
#include "system/files/FileAccess.h"
#include "system/info/SystemInfo.h"
#include "system/media/MprisController.h"
#include "system/screen/ScreenCapture.h"
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
    parser.process(app);

    UnixSignals unixSignals;
    QObject::connect(&unixSignals, &UnixSignals::received, &app, [](int signal) {
        qCInfo(lcApp) << "Signal" << signal << "- quitting";
        QCoreApplication::quit();
    });

    ConfigStore store;
    QString startupWarning;
    Config config = loadConfig(store, startupWarning);

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
    registerSystemTools(tools, SystemServices{&commands, &apps, &audio, &media, &files, &screen});

    // Voice: microphone → recognition → requests; replies → speech (backends built by AppController)
    AudioCapture microphone;
    VoicePipeline voice(&microphone);
    registerVoiceTools(tools, &voice);
    panic.addHandler([&voice] { voice.cancel(); });

    // Fast path (no model) for common phrases; order = priority
    IntentRouter router([&tools](const QString &tool) { return tools.contains(tool); });
    router.add(std::make_unique<ControlIntent>());
    router.add(std::make_unique<MediaIntent>());
    router.add(std::make_unique<AudioIntent>());
    router.add(std::make_unique<ScreenIntent>());
    router.add(std::make_unique<AppIntent>(&apps));
    router.add(std::make_unique<CommandIntent>(&apps, programExists));
    router.add(std::make_unique<InputIntent>());

    Agent agent(&executor, &tools);

    AppController controller(
        AppServices{&store, &providers, &tools, &executor, &router, &agent, &panic, &policy, &voice, &http},
        std::move(config), SystemInfo::summary());
    // Hotkeys that work from any window. The compositor grants them (GNOME asks once) and the
    // user can change them in Settings → Keyboard; without the portal, the in-window shortcuts
    // (Ctrl+M, Esc, Ctrl+Alt+Esc) still work.
    GlobalShortcuts shortcuts(QApplication::desktopFileName());
    MainWindow window(&controller, &providers, &executor, &http, &shortcuts);
    window.show();
    shortcuts.bind({
        {u"talk"_s, QObject::tr("Talk to Nexa (hold to speak)"), u"CTRL+ALT+space"_s},
        {u"panic"_s, QObject::tr("Stop everything Nexa is doing"), u"CTRL+ALT+Escape"_s},
    });

    if (!startupWarning.isEmpty()) QMessageBox::warning(&window, QObject::tr("Nexa settings"), startupWarning);
    QTimer::singleShot(0, &controller, &AppController::start);

    // Scripted start-up (--say, --voice-file, --ask): one step at a time, each once Nexa is idle
    // again — reply finished and spoken.
    QList<std::function<void()>> steps;
    for (const QString &text : parser.values(sayOption)) steps.append([&voice, text] { voice.speak(text); });
    for (const QString &path : parser.values(voiceFileOption)) {
        steps.append([&voice, &window, path] {
            const Result<QByteArray> recording = loadRecording(path);
            if (recording) {
                voice.submitRecording(recording.value());
            } else {
                QMessageBox::warning(&window, QObject::tr("Nexa"),
                                     QObject::tr("Cannot read %1: %2").arg(path, recording.error().message));
            }
        });
    }
    for (const QString &text : parser.values(askOption)) steps.append([&controller, text] { controller.submit(text); });
    std::function<void()> runNext = [&controller, &voice, &steps] {
        if (!steps.isEmpty() && !controller.isBusy() && voice.state() == VoiceState::Idle) steps.takeFirst()();
    };
    const auto later = [&runNext] { QTimer::singleShot(250, runNext); };
    QObject::connect(&controller, &AppController::busyChanged, &controller, [later](bool busy) {
        if (!busy) later();
    });
    QObject::connect(&voice, &VoicePipeline::stateChanged, &controller, [later](VoiceState state) {
        if (state == VoiceState::Idle) later();
    });
    if (!steps.isEmpty()) QTimer::singleShot(800, runNext);
    return app.exec();
}
