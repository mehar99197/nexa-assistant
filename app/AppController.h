#pragma once

#include "core/Result.h"
#include "core/agent/AppContext.h"
#include "core/agent/Conversation.h"
#include "core/ai/ChatStream.h"
#include "core/config/Config.h"
#include "core/intent/IIntentHandler.h"
#include "core/tools/ToolExecutor.h"
#include "ui/NexaCoreSpec.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace nexa {

class Agent;
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
class MicrophoneLeveler;

// Everything the controller coordinates; owned by the composition root (app/main.cc).
struct AppServices {
    ConfigStore *store{nullptr};
    ProviderRegistry *providers{nullptr};
    ToolRegistry *tools{nullptr};
    ToolExecutor *executor{nullptr};
    IntentRouter *router{nullptr};
    Agent *agent{nullptr};
    PanicController *panic{nullptr};
    PolicyEngine *policy{nullptr};
    VoicePipeline *voice{nullptr};
    HttpClient *http{nullptr};          // for the cloud voice backends
    WpctlAudioController *microphoneVolume{nullptr};    // the input's volume, for "Lower it" only
    SherpaRuntime *sherpa{nullptr};     // the spotter, Silero and noise reduction, when installed
    AppContext *context{nullptr};       // the app the user is in (filled by open_app, close_app)
    std::function<bool()> windowActive; // Nexa's own window has the keyboard focus
    std::function<bool()> canType;      // an input backend can type (the helper, or the portal)
    MemoryStore *memory{nullptr};       // what Nexa remembers about the user (Settings → Memory)
    SkillCatalog *skills{nullptr};      // the skills the model may load (Settings → Skills)
    // Where the browser_* tools work, in words for the prompt; empty while they are not there.
    std::function<QString()> browserTarget;
};

// Mediator between the GUI, voice and core. Each request takes the fast path when the
// IntentRouter recognises it (a direct tool call, no model) and otherwise runs the Agent; both
// report through the same reply signals, so the GUI renders them and the voice pipeline speaks
// them alike (and the phone sees them too). It also keeps the one state the Nexa core shows
// (coreMode): the voice pipeline's state, tools running, the fast path, the agent streaming and
// a provider that cannot be reached, resolved by NexaCoreSpec's precedence.
class AppController : public QObject {
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

    void submit(const QString &text, const QString &source = QStringLiteral("gui"));
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
    void cancel();                                  // the current reply and its speech
    void cancelReply();                             // the current reply only
    void lowerMicrophone();                         // "Lower it": step the input volume down until nothing clips
    // Listen for the wake name whatever the settings say — this run only, never saved (--hear).
    void setWakeWordForced(bool on);
    void panic();                                   // everything: replies, tools, child processes, voice
    void newConversation();
    bool isBusy() const { return busy_; }

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

    void setConfirmer(ToolExecutor::Confirmer confirmer);
    VoicePipeline *voice() const { return services_.voice; }

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
    void userMessageAccepted(const QString &text, const QString &source);
    void replyStarted(const QString &source);       // "gui", "voice", …
    void replyRoute(const QString &route);          // "fast" (no model) or "agent"; may change mid-reply
    void replyVia(const QString &route);            // a fallback provider answered: "agy · claude-sonnet-4-6"
    void replyDelta(const QString &delta);
    void replyNotice(const QString &message);       // a note inside the reply, not its end
    void replyStep(const QString &description, bool ok);    // a tool ran (fast path or agent), as one line
    void replyAction(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);   // the same, in parts
    void replyFinished(nexa::FinishReason reason, bool empty);
    void replyFailed(const QString &message);
    void busyChanged(bool busy);
    void conversationCleared();
    void panicked();
    void microphoneOverloaded(const QString &notice);   // with a "Lower it" button beside it
    void coreModeChanged(nexa::CoreMode mode);
    void litNodesChanged(unsigned nodes);           // CoreNodes: the satellites the running action lights
    void screenSharingChanged(bool shared);
    // The app the user is in changed: its name and a one-line hint, both empty when there is none.
    void appContextChanged(const QString &name, const QString &hint);

private:
    void runFastPath(const QString &prompt, const IntentMatch &match, const QString &source);
    void answerDirectly(const QString &prompt, const QString &reply, const QString &source);
    void applyScreenSharing();
    AppContext::Situation situation(const QString &source = QString()) const;
    void refreshAppContext();           // tells the window when the app in front changed
    void askAboutPicture(const QString &tool, const QString &question, const QString &fallback);
    void runAgent(const QString &source);
    bool canUseModel(bool report);
    void applyVoiceConfig();
    void beginReply(const QString &source);
    void finishReply(FinishReason reason);
    void failReply(const QString &message);
    void setBusy(bool busy);
    void rebuildProvider();
    void persist();
    void refreshCoreMode();
    void noteProviderError(const Error &error);
    // The first choice is refused for want of credentials: a key to ask for (an API), or a sign-in (a program, when
    // it says nobody is signed in).
    // `told`: the chat already says so (a reply that failed), otherwise a note is added.
    void credentialsRequired(const Error &error, bool told);
    void setOffline(bool offline, const QString &reason = {});
    ProviderConfig *activeProviderConfig();
    QString systemPrompt(const QString &source = QString()) const;   // the request's source shapes it
    QString browserParagraph() const;                                 // empty while there are no browser tools
    QString describe(const Error &error) const;

    AppServices services_;
    Config config_;
    QString environment_;
    std::unique_ptr<IAiProvider> provider_;
    Conversation conversation_;
    QStringList models_;
    bool busy_{false};
    bool replyHasContent_{false};
    quint64 replyGeneration_{0};    // fast-path callbacks of a cancelled reply are ignored
    CoreActivity activity_;
    CoreMode coreMode_{CoreMode::Idle};
    std::unique_ptr<MicrophoneLeveler> leveler_;
    bool wakeForced_{false};
    CoreNodes litNodes_{NoNode};
    int runningActions_{0};
    QString offlineReason_;
    bool screenShared_{false};
    QString announcedApp_;              // the app id the window was last told about
    QString announcedHint_;             // and the hint, which the policy and the input backend shape
    QTimer contextExpiry_;              // the window hears when the app in front is forgotten
};

} // namespace nexa
