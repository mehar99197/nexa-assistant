#pragma once

#include "core/Result.h"
#include "core/agent/Conversation.h"
#include "core/ai/ChatStream.h"
#include "core/config/Config.h"
#include "core/intent/IIntentHandler.h"
#include "core/tools/ToolExecutor.h"
#include "ui/NexaCoreSpec.h"

#include <QObject>
#include <QStringList>

#include <memory>

namespace nexa {

class Agent;
class ConfigStore;
class HttpClient;
class IAiProvider;
class IntentRouter;
class PanicController;
class PolicyEngine;
class ProviderRegistry;
class ToolRegistry;
class VoicePipeline;

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
    void setTheme(const QString &theme);            // the toolbar toggle: saved, nothing rebuilt
    QString providerName() const;
    void setApiKey(const QString &key);             // for the active provider

    QString currentModel() const;
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
    void cancel();                                  // the current reply and its speech
    void cancelReply();                             // the current reply only
    void panic();                                   // everything: replies, tools, child processes, voice
    void newConversation();
    bool isBusy() const { return busy_; }

    CoreMode coreMode() const { return coreMode_; }
    CoreNodes litNodes() const { return litNodes_; }
    QString offlineReason() const { return offlineReason_; }    // why the core shows Offline
    // A line for the status bar (and the phones, when it is an error) from outside the controller.
    void report(const QString &message, bool isError) { emit statusChanged(message, isError); }

    void setConfirmer(ToolExecutor::Confirmer confirmer);
    VoicePipeline *voice() const { return services_.voice; }

signals:
    void providerChanged(const QString &name);
    // Settings were saved; the parts of Nexa outside the controller (the phone) follow them.
    void configChanged(const nexa::Config &config);
    void themeChanged(const QString &theme);        // "system" | "dark" | "light"
    void modelsChanged(const QStringList &models, const QString &current);
    void statusChanged(const QString &message, bool isError);
    void apiKeyRequired(const QString &providerName);
    void userMessageAccepted(const QString &text, const QString &source);
    void replyStarted(const QString &source);       // "gui", "voice", …
    void replyRoute(const QString &route);          // "fast" (no model) or "agent"; may change mid-reply
    void replyDelta(const QString &delta);
    void replyStep(const QString &description, bool ok);    // a tool ran (fast path or agent), as one line
    void replyAction(const QString &tool, bool ok, const QString &summary, qint64 elapsedMs);   // the same, in parts
    void replyFinished(nexa::FinishReason reason, bool empty);
    void replyFailed(const QString &message);
    void busyChanged(bool busy);
    void conversationCleared();
    void panicked();
    void coreModeChanged(nexa::CoreMode mode);
    void litNodesChanged(unsigned nodes);           // CoreNodes: the satellites the running action lights
    void screenSharingChanged(bool shared);

private:
    void runFastPath(const QString &prompt, const IntentMatch &match, const QString &source);
    void answerDirectly(const QString &prompt, const QString &reply, const QString &source);
    void applyScreenSharing();
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
    void setOffline(bool offline, const QString &reason = {});
    ProviderConfig *activeProviderConfig();
    QString systemPrompt() const;
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
    CoreNodes litNodes_{NoNode};
    int runningActions_{0};
    QString offlineReason_;
    bool screenShared_{false};
};

} // namespace nexa
