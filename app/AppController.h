#pragma once

#include "core/Result.h"
#include "core/agent/Conversation.h"
#include "core/ai/ChatStream.h"
#include "core/config/Config.h"
#include "core/intent/IIntentHandler.h"
#include "core/tools/ToolExecutor.h"

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
// them alike (later the phone too).
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
    QString providerName() const;
    void setApiKey(const QString &key);             // for the active provider

    QString currentModel() const;
    void setCurrentModel(const QString &model);
    void refreshModels();

    void submit(const QString &text, const QString &source = QStringLiteral("gui"));
    // Takes a screenshot and asks the model about it (default: describe the screen).
    void askAboutScreen(const QString &question);
    void cancel();                                  // the current reply and its speech
    void cancelReply();                             // the current reply only
    void panic();                                   // everything: replies, tools, child processes, voice
    void newConversation();
    bool isBusy() const { return busy_; }

    void setConfirmer(ToolExecutor::Confirmer confirmer);
    VoicePipeline *voice() const { return services_.voice; }

signals:
    void providerChanged(const QString &name);
    void modelsChanged(const QStringList &models, const QString &current);
    void statusChanged(const QString &message, bool isError);
    void apiKeyRequired(const QString &providerName);
    void userMessageAccepted(const QString &text);
    void replyStarted(const QString &source);       // "gui", "voice", …
    void replyDelta(const QString &delta);
    void replyStep(const QString &description, bool ok);    // a tool ran (fast path or agent)
    void replyFinished(nexa::FinishReason reason, bool empty);
    void replyFailed(const QString &message);
    void busyChanged(bool busy);
    void conversationCleared();
    void panicked();

private:
    void runFastPath(const QString &prompt, const IntentMatch &match, const QString &source);
    void runAgent(const QString &source);
    bool canUseModel(bool report);
    void applyVoiceConfig();
    void beginReply(const QString &source);
    void finishReply(FinishReason reason);
    void failReply(const QString &message);
    void setBusy(bool busy);
    void rebuildProvider();
    void persist();
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
};

} // namespace nexa
