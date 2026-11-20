#pragma once

#include "core/Result.h"
#include "core/agent/AppContext.h"
#include "core/agent/Attachments.h"
#include "core/agent/Conversation.h"
#include "core/ai/ChatStream.h"
#include "core/config/Config.h"
#include "core/intent/IIntentHandler.h"
#include "core/mission/HardcoreMode.h"
#include "core/mission/MissionControl.h"
#include "core/tools/ToolExecutor.h"
#include "ui/NexaCoreSpec.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace nexa {

class AccountManager;
class ConfigStore;
class HttpClient;
class IAiProvider;
class IntentRouter;
class MemoryStore;
class PanicController;
class PolicyEngine;
class ProviderRegistry;
class SkillCatalog;
class ToolRegistry;
class VoicePipeline;
class WpctlAudioController;
class SherpaRuntime;
class SpeakerGate;
class MicrophoneLeveler;

// Everything the controller coordinates; owned by the composition root (app/main.cc).
struct AppServices {
    ConfigStore *store{nullptr};
    ProviderRegistry *providers{nullptr};
    ToolRegistry *tools{nullptr};
    ToolExecutor *executor{nullptr};
    IntentRouter *router{nullptr};
    PanicController *panic{nullptr};
    PolicyEngine *policy{nullptr};
    VoicePipeline *voice{nullptr};
    HttpClient *http{nullptr};          // for the cloud voice backends
    WpctlAudioController *microphoneVolume{nullptr};    // the input's volume, for "Lower it" only
    SherpaRuntime *sherpa{nullptr};     // the spotter, Silero and noise reduction, when installed
    SpeakerGate *speakerGate{nullptr};  // whose voice it is (the owner's profile, Settings → Voice)
    AppContext *context{nullptr};       // the app the user is in (filled by open_app, close_app)
    std::function<bool()> windowActive; // Nexa's own window has the keyboard focus
    std::function<bool()> canType;      // an input backend can type (the helper, or the portal)
    MemoryStore *memory{nullptr};       // what Nexa remembers about the user (Settings → Memory)
    SkillCatalog *skills{nullptr};      // the skills the model may load (Settings → Skills)
    // Where the browser_* tools work, in words for the prompt; empty while they are not there.
    std::function<QString()> browserTarget;
    IInputLock *inputLock{nullptr};     // hardcore mode's lock on the laptop's keyboard and mouse (the root helper)
    AccountManager *account{nullptr};   // licence/account entitlements + managed Nexa AI runtime token
};

// Mediator between the GUI, voice and core. Every request is a task on the mission board (MissionControl, which it
// owns): the fast path's tool when the IntentRouter recognises it, otherwise a model, each task on a provider instance
// of its own — and the controller is what every task asks for providers, models and its prompt (ITaskEnvironment).
// The reply signals are the conversation: the chat, the voice, the tray and the phone follow the newest request, a
// reply still running when another is asked goes on in the background and its answer is added once it is done. It
// also keeps the one state the Nexa core shows (coreMode): the voice pipeline's state, tools running, the fast path,
// a model at work and a provider that cannot be reached, resolved by NexaCoreSpec's precedence.
class AppController : public QObject, public ITaskEnvironment {
    Q_OBJECT
public:
    // `environment` is appended to the system prompt (OS, desktop, user, home…).
    AppController(AppServices services, Config config, QString environment, QObject *parent = nullptr);
    ~AppController() override;

    // Builds the active provider and loads its models (or asks for the missing API key).
    void start();

    const Config &config() const { return config_; }
    void updateConfig(const Config &config);        // from Settings: saves, rebuilds, reloads models
    // A program's sign-in or sign-out in Settings succeeded: the providers are built again and their models listed
    // (the config itself is untouched). Does nothing while a reply is running.
    void reloadProviders();
    void setTheme(const QString &theme);            // the toolbar toggle: saved, nothing rebuilt
    QString providerName() const;
    void setApiKey(const QString &key);             // for the active provider

    QString currentModel() const;
    // Of the models listed, those known to take images (the first choice's, as its provider knows them).
    QStringList imageModels() const;
    void setCurrentModel(const QString &model);
    void refreshModels();

    // A request, as a task carrying on this channel's conversation (the PC's or the phone's). The task's id; 0 when
    // none was made (nothing said, the panic, no model to ask). `attachments`: files given with the words (prepared by
    // AttachmentLoader): the model reads them as labelled data after the words, pictures as pictures, and the chat
    // shows their names; a request with files always goes to the model (no fast path), and may have no words.
    quint64 submit(const QString &text, const QString &source = QStringLiteral("gui"),
                   const QList<PreparedAttachment> &attachments = {});
    // A spoken request from the PC's microphone; `fromOwner`: the enrolled voice was checked and it is the owner's.
    quint64 submitVoice(const QString &text, bool fromOwner);
    // Several at once (Add tasks): one task per line that says something, independent of the conversation, on
    // `model` when one is given (else what each role is given).
    // `hardcore`: they lock the laptop's keyboard and mouse while they run (Add tasks' box).
    QList<quint64> addTasks(const QStringList &lines, const std::optional<ModelChoice> &model,
                            const QString &source = QStringLiteral("gui"), bool hardcore = false);
    MissionControl *mission() const { return mission_.get(); }
    // Hardcore mode: the switch, the lock and its state; null without an input lock to hold (no helper wired).
    HardcoreMode *hardcore() const { return hardcore_.get(); }
    // The keyboard and mouse back now (Unlock PC on the phone, "unlock" from the phone or the owner's voice); the tasks
    // go on.
    void unlockHardcore(const QString &source);
    // Whether hardcore mode may be switched on: the root helper can lock, and something can end a lock early — a paired
    // phone with the web remote on, or the owner's enrolled voice with the wake word on. `why` says what is missing.
    bool canUseHardcore(QString *why = nullptr) const;
    // How the owner can end a lock early, in a sentence: the voice when the wake word is on, the phone when the web
    // remote is; otherwise that only the task's end (or the power button) will.
    QString hardcoreWaysOut() const;
    // Whether a phone is paired (the web remote's devices): only then is the phone a way out of a lock.
    void setPhonePaired(std::function<bool()> paired) { phonePaired_ = std::move(paired); }
    // Takes a screenshot of the phone and asks the model about it (default: describe it).
    void askAboutPhone(const QString &question);
    // The Share screen switch. Off at every start: the model is not offered take_screenshot and is
    // told to work through commands and tools instead; on, it may look whenever that helps. Your
    // own "what's on my screen?" takes one picture either way — you asked for it.
    bool isScreenShared() const { return screenShared_; }
    void setScreenShared(bool shared);
    // The app Nexa opened last for the user, if it still counts as in front (AppContext).
    QString appContextName() const;
    QString appContextHint() const;
    void cancel();                                  // the reply in view and its speech
    void cancelReply();                             // the reply in view only (its task stops)
    void lowerMicrophone();                         // "Lower it": step the input volume down until nothing clips
    // Listen for the wake name whatever the settings say — this run only, never saved (--hear).
    void setWakeWordForced(bool on);
    void panic();                                   // everything: replies, tools, child processes, voice
    void newConversation();
    bool isBusy() const;                            // a task runs, waits for a slot or for its sub-task
    bool isReplying() const { return replying_; }   // the reply in view is still being written
    QString lastHeard() const { return lastHeard_; }    // the last spoken request

    CoreMode coreMode() const { return coreMode_; }
    CoreNodes litNodes() const { return litNodes_; }
    QString offlineReason() const { return offlineReason_; }    // why the core shows Offline
    // A line for the status bar (and the phones, when it is an error) from outside the controller.
    void report(const QString &message, bool isError) { emit statusChanged(message, isError); }
    // A note in the chat as well as the status bar: something the user should read once.
    void notify(const QString &message) {
        emit replyNotice(message);
        emit statusChanged(message, false);
    }

    // Asks about an action that belongs to no task (the board answers the tasks' own questions).
    void setConfirmer(ToolExecutor::Confirmer confirmer);
    VoicePipeline *voice() const { return services_.voice; }
    Config runtimeConfig() const;       // copy with runtime Nexa AI token injected + entitlement overrides

    // ITaskEnvironment: what every task is given.
    Result<std::shared_ptr<IAiProvider>> createProvider(const ModelChoice &choice) override;
    QString modelFor(const ModelChoice &choice) override;
    QString describeChoice(const ModelChoice &choice) override;
    QList<ProviderFacts> providerFacts() override;
    QString systemPrompt(const QString &source, AgentRole role) override;
    int maxSteps() override;

signals:
    void providerChanged(const QString &name);
    // Settings were saved; the parts of Nexa outside the controller (the phone) follow them.
    void configChanged(const nexa::Config &config);
    void themeChanged(const QString &theme);        // "system" | "dark" | "light"
    void modelsChanged(const QStringList &models, const QString &current);
    void modelsLoading(bool loading);                // the provider is being asked for its models (agy takes ~16 s)
    void statusChanged(const QString &message, bool isError);
    void apiKeyRequired(const QString &providerName);   // an API provider rejected, or lacks, its key
    // A program provider (agy, opencode, Codex) is not signed in: Settings → Providers has its Sign in button.
    void signInRequired(const QString &providerName);
    // `attachments`: the line naming the files given with the words ("📎 notes.txt (3 KB)"), empty without any.
    void userMessageAccepted(const QString &text, const QString &source, const QString &attachments);
    void replyStarted(const QString &source);       // "gui", "voice", …
    void replyRoute(const QString &route);          // "fast" (no model) or "agent"; may change mid-reply
    void replyVia(const QString &route);            // a fallback provider answered: "agy · claude-sonnet-4-6"
    void replyDelta(const QString &delta);
    void replyNotice(const QString &message);       // a note inside the reply, not its end
    void replyStep(const QString &description, bool ok);    // a tool ran (fast path or agent), as one line
    void replyAction(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);   // the same, in parts
    void replyFinished(nexa::FinishReason reason, bool empty);
    void replyFailed(const QString &message);
    void busyChanged(bool busy);                    // the board: something runs or waits
    void replyingChanged(bool replying);            // the reply in view started or ended
    void lastHeardChanged(const QString &text);
    void announced(const QString &sentence);        // a board task said aloud that it ended or needs the user
    // A task came from the phone (Nexa Phone or the web remote): the PC says so, and the tray shows it.
    void phoneMessageReceived(const QString &text);
    void conversationCleared();
    void panicked();
    void microphoneOverloaded(const QString &notice);   // with a "Lower it" button beside it
    void coreModeChanged(nexa::CoreMode mode);
    void litNodesChanged(unsigned nodes);           // CoreNodes: the satellites the running action lights
    void screenSharingChanged(bool shared);
    void accountRequired(const QString &message);   // active provider requires Nexa account / Pro-Team licence
    // The app the user is in changed: its name and a one-line hint, both empty when there is none.
    void appContextChanged(const QString &name, const QString &hint);

private:
    bool handlesControl(const IntentMatch &match) const;
    // While hardcore mode holds the keyboard and mouse, what would end that work early — unlock, stopping or pausing a
    // hardcore task, "stop everything" — is taken only from the phone, the web remote or the owner's own voice.
    bool hardcoreHolds() const;
    bool endsHardcoreWork(const IntentMatch &match) const;
    bool mayEndHardcore(const QString &source) const;
    bool ownerVoiceReady() const;       // the wake word on and an enrolled voice the gate checks
    bool phoneReady() const;            // the web remote on and a phone paired
    void carryOut(const IntentMatch &match, const QString &source);
    void acknowledge(const QString &text, const QString &source);
    bool shouldAnnounce(const Task &task) const;
    void announce(const QString &sentence);
    void sayNextAnnouncement();
    void onTaskAdded(quint64 id);
    void onTaskFinished(quint64 id, FinishReason reason, bool empty);
    void onTaskFailed(quint64 id, const Error &error);
    void showBacklog();
    bool isRunning(quint64 id) const;               // the task still runs or waits
    void setReplying(bool replying);
    void refreshTaskActivity();
    static QString channelOf(const QString &source);    // "pc" | "phone": whose conversation a request carries on
    Result<std::shared_ptr<IAiProvider>> makeChain();
    static std::shared_ptr<IAiProvider> shareProvider(std::unique_ptr<IAiProvider> provider);
    void applyScreenSharing();
    AppContext::Situation situation(const QString &source = QString()) const;
    void refreshAppContext();           // tells the window when the app in front changed
    // `textTool` stands in for a model that cannot see images (android_snapshot for android_screenshot).
    void askAboutPicture(const QString &tool, const QString &textTool, const QString &question, const QString &fallback);
    bool canUseModel(bool report);
    void applyVoiceConfig();
    void rebuildProvider();
    void persist();
    void refreshCoreMode();
    void noteProviderError(const Error &error);
    // The first choice is refused for want of credentials: a key to ask for (an API), or a sign-in (a program, when
    // it says nobody is signed in).
    // `told`: the chat already says so (a reply that failed), otherwise a note is added.
    void credentialsRequired(const Error &error, bool told);
    void setOffline(bool offline, const QString &reason = {});
    ProviderConfig runtimeProvider(const ProviderConfig &provider) const;
    ProviderConfig *activeProviderConfig();
    QString basePrompt(const QString &source = QString()) const;     // the request's source shapes it
    QString browserParagraph() const;                                 // empty while there are no browser tools
    QString describe(const Error &error) const;

    AppServices services_;
    Config config_;
    QString environment_;
    std::unique_ptr<MissionControl> mission_;
    std::unique_ptr<HardcoreMode> hardcore_;
    std::function<bool()> phonePaired_;
    bool ownerSpeaking_{false};                 // the request being handled is the owner's verified voice
    std::unique_ptr<IAiProvider> provider_;     // the first choice's chain, for the model list
    std::shared_ptr<IAiProvider> spare_;        // made and warmed up for the next Auto task
    QStringList models_;
    QHash<QString, quint64> heads_;             // per channel: the task the next request carries on
    std::optional<quint64> shown_;              // the task the conversation shows
    QSet<quint64> detached_;                    // went on in the background; its answer is due in the chat
    QList<quint64> backlog_;                    // finished in the background; shown once the chat is free
    bool replying_{false};
    bool addingToBoard_{false};
    QSet<quint64> boardOnly_;                   // tasks added to the board (Add tasks, "add a task"): not the chat's
    QStringList announcements_;
    QTimer announcer_;
    QElapsedTimer lastAnnouncement_;
    QString lastHeard_;
    CoreActivity activity_;
    CoreMode coreMode_{CoreMode::Idle};
    std::unique_ptr<MicrophoneLeveler> leveler_;
    bool wakeForced_{false};
    CoreNodes litNodes_{NoNode};
    QHash<quint64, QString> runningActions_;    // the executor's action id → the tool, while it runs
    QString offlineReason_;
    bool screenShared_{false};
    QString announcedApp_;              // the app id the window was last told about
    QString announcedHint_;             // and the hint, which the policy and the input backend shape
    QTimer contextExpiry_;              // the window hears when the app in front is forgotten
};

} // namespace nexa
