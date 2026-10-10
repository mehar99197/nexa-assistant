#include "app/AppController.h"

#include "core/Logging.h"
#include "core/agent/Agent.h"
#include "core/ai/IAiProvider.h"
#include "core/ai/ProviderRegistry.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/ToolRegistry.h"
#include "voice/VoiceBackends.h"
#include "voice/pipeline/VoicePipeline.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/tts/ITextToSpeech.h"

#include <QElapsedTimer>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr qsizetype kMaxShownOutput = 6'000;

// OmniRoute's auto/* routing combos are slow, so never pick one as the default.
QString pickDefaultModel(const QStringList &models) {
    for (const QString &model : models) {
        if (!model.startsWith(u"auto"_s, Qt::CaseInsensitive)) return model;
    }
    return models.first();
}

QString stepText(const QString &tool, const QString &summary, qint64 elapsedMs) {
    return u"%1 — %2 · %3 ms"_s.arg(tool, summary).arg(elapsedMs);
}

// Spoken requests — at the PC or into the phone — are marked for the model, whose reply is then
// read out (see systemPrompt()).
QString forModel(const QString &prompt, const QString &source) {
    return source == u"voice"_s || source == u"phone-voice"_s ? u"[voice] "_s + prompt : prompt;
}

// The provider is out of reach, as opposed to refusing a key or a request.
bool isOffline(const Error &error) {
    return error.code == ErrorCode::ConnectionRefused || error.code == ErrorCode::Network
           || error.code == ErrorCode::Timeout;
}

} // namespace

AppController::AppController(AppServices services, Config config, QString environment, QObject *parent)
    : QObject(parent),
      services_(services),
      config_(std::move(config)),
      environment_(std::move(environment)) {
    conversation_.setSystemPrompt(systemPrompt());
    services_.policy->setMode(PolicyEngine::modeFromString(config_.policyMode));

    Agent *agent = services_.agent;
    connect(agent, &Agent::textDelta, this, [this](const QString &delta) {
        replyHasContent_ = true;
        emit replyDelta(delta);
    });
    connect(agent, &Agent::toolFinished, this, [this](const QString &tool, bool ok, const QString &summary, qint64 ms) {
        replyHasContent_ = true;
        emit replyAction(tool, ok, summary, ms);
        emit replyStep(stepText(tool, summary, ms), ok);
    });
    connect(agent, &Agent::finished, this, &AppController::finishReply);
    connect(agent, &Agent::failed, this, [this](const Error &error) {
        noteProviderError(error);
        failReply(describe(error));
        if (error.code == ErrorCode::Unauthorized) emit apiKeyRequired(providerName());
    });

    // Every action, from the fast path or the agent, lights the core and its satellites.
    connect(services_.executor, &ToolExecutor::actionStarted, this,
            [this](quint64, const QString &tool, const QJsonObject &, const QString &) {
                ++runningActions_;
                const CoreNodes lit = litNodes_ | coreNodesForTool(tool);
                if (lit != litNodes_) {
                    litNodes_ = lit;
                    emit litNodesChanged(litNodes_);
                }
                activity_.acting = true;
                refreshCoreMode();
            });
    connect(services_.executor, &ToolExecutor::actionFinished, this, [this](quint64, bool, const QString &, qint64) {
        if (--runningActions_ > 0) return;
        runningActions_ = 0;
        activity_.acting = false;
        if (litNodes_ != NoNode) {
            litNodes_ = NoNode;
            emit litNodesChanged(NoNode);
        }
        refreshCoreMode();
    });

    if (VoicePipeline *voice = services_.voice) {
        connect(voice, &VoicePipeline::request, this, [this](const QString &text) { submit(text, u"voice"_s); });
        // Speaking over a reply replaces it; the new utterance itself must keep going.
        connect(voice, &VoicePipeline::interruptRequested, this, &AppController::cancelReply);
        connect(this, &AppController::replyStarted, voice, &VoicePipeline::onReplyStarted);
        connect(this, &AppController::replyDelta, voice, &VoicePipeline::onReplyDelta);
        connect(this, &AppController::replyFinished, voice, &VoicePipeline::onReplyFinished);
        connect(this, &AppController::replyFailed, voice, &VoicePipeline::onReplyFailed);
        connect(voice, &VoicePipeline::stateChanged, this, [this](VoiceState state) {
            activity_.listening = state == VoiceState::Listening;
            activity_.transcribing = state == VoiceState::Transcribing;
            activity_.speaking = state == VoiceState::Speaking;
            refreshCoreMode();
        });
        connect(voice, &VoicePipeline::settingsChanged, this, [this] {
            activity_.wakeWord = services_.voice->wakeWordActive();
            refreshCoreMode();
        });
    }
}

AppController::~AppController() {
    services_.agent->disconnect(this);
    services_.agent->cancel();
}

void AppController::start() {
    rebuildProvider();
    applyVoiceConfig();
    const ProviderConfig *active = config_.active();
    if (!active || !provider_) return;
    if (active->requiresKey && active->apiKey.isEmpty()) {
        emit statusChanged(tr("%1 needs an API key.").arg(active->name), true);
        emit apiKeyRequired(active->name);
        return;
    }
    refreshModels();
}

void AppController::updateConfig(const Config &config) {
    cancel();
    config_ = config;
    conversation_.setSystemPrompt(systemPrompt());
    services_.policy->setMode(PolicyEngine::modeFromString(config_.policyMode));
    persist();
    emit configChanged(config_);
    emit themeChanged(config_.ui.theme);
    start();    // rebuilds the provider and the voice backends
}

void AppController::setTheme(const QString &theme) {
    if (config_.ui.theme == theme) return;
    config_.ui.theme = theme;
    persist();
    emit themeChanged(theme);
}

QString AppController::providerName() const {
    const ProviderConfig *active = config_.active();
    return active ? active->name : QString();
}

void AppController::setApiKey(const QString &key) {
    ProviderConfig *active = activeProviderConfig();
    if (!active) return;
    active->apiKey = key.trimmed();
    persist();
    rebuildProvider();
    applyVoiceConfig();
    refreshModels();
}

QString AppController::currentModel() const {
    const ProviderConfig *active = config_.active();
    return active ? active->defaultModel : QString();
}

void AppController::setCurrentModel(const QString &model) {
    ProviderConfig *active = activeProviderConfig();
    const QString chosen = model.trimmed();
    if (!active || chosen.isEmpty() || active->defaultModel == chosen) return;
    active->defaultModel = chosen;
    persist();
    emit statusChanged(tr("Model: %1").arg(chosen), false);
}

void AppController::refreshModels() {
    if (!provider_) return;
    emit statusChanged(tr("Loading models from %1…").arg(providerName()), false);
    // The callback is dropped if the provider is rebuilt meanwhile.
    provider_->listModels([this](Result<QStringList> result) {
        if (!result) {
            noteProviderError(result.error());
            emit statusChanged(describe(result.error()), true);
            if (result.error().code == ErrorCode::Unauthorized) emit apiKeyRequired(providerName());
            return;
        }
        setOffline(false);
        models_ = result.value();
        if (currentModel().isEmpty() && !models_.isEmpty()) setCurrentModel(pickDefaultModel(models_));
        emit modelsChanged(models_, currentModel());
        emit statusChanged(tr("%1: %2 models available").arg(providerName()).arg(models_.size()), false);
    });
}

void AppController::submit(const QString &text, const QString &source) {
    const QString prompt = text.trimmed();
    if (prompt.isEmpty() || busy_) return;

    const std::optional<IntentMatch> match = services_.router->route(prompt);
    if (match && match->panic) {
        emit userMessageAccepted(prompt, source);
        panic();
        return;
    }
    if (!match && !canUseModel(true)) return;

    emit userMessageAccepted(prompt, source);
    beginReply(source);
    if (match) {
        activity_.fastPath = true;
        refreshCoreMode();
        emit replyRoute(u"fast"_s);
        runFastPath(prompt, *match, source);
        return;
    }
    conversation_.addUser(forModel(prompt, source));
    runAgent(source);
}

void AppController::askAboutScreen(const QString &question) {
    askAboutPicture(u"take_screenshot"_s, question, tr("Describe what is on my screen."));
}

void AppController::askAboutPhone(const QString &question) {
    askAboutPicture(u"android_screenshot"_s, question, tr("Describe what is on my phone's screen."));
}

void AppController::askAboutPicture(const QString &tool, const QString &question, const QString &fallback) {
    if (busy_ || !canUseModel(true)) return;
    const QString prompt = question.trimmed().isEmpty() ? fallback : question.trimmed();
    emit userMessageAccepted(prompt, u"gui"_s);
    beginReply(u"gui"_s);
    activity_.fastPath = true;
    refreshCoreMode();
    emit replyRoute(u"fast"_s);
    runFastPath(prompt, IntentMatch{.toolName = tool, .forwardToAgent = true, .agentPrompt = prompt}, u"gui"_s);
}

void AppController::runFastPath(const QString &prompt, const IntentMatch &match, const QString &source) {
    const quint64 generation = replyGeneration_;
    QElapsedTimer timer;
    timer.start();
    qCDebug(lcApp) << "fast path:" << match.toolName << match.arguments;
    services_.executor->execute(match.toolName, match.arguments, source,
                                [this, generation, timer, prompt, match, source](Result<ToolResult> result) {
        if (generation != replyGeneration_) return;  // cancelled meanwhile
        const qint64 elapsed = timer.elapsed();
        if (!result && result.error().code == ErrorCode::Cancelled) {
            finishReply(FinishReason::Cancelled);
            return;
        }
        // The fast path could not resolve it (e.g. an app name it does not know): let the model try.
        if (!result && result.error().code == ErrorCode::NotFound && !match.forwardToAgent && canUseModel(false)) {
            qCDebug(lcApp) << "fast path missed, asking the model:" << result.error().message;
            conversation_.addUser(forModel(prompt, source));
            runAgent(source);
            return;
        }

        const QString summary = result ? summarize(result.value().text) : result.error().message;
        replyHasContent_ = true;
        emit replyAction(match.toolName, result.isOk(), summary, elapsed);
        emit replyStep(stepText(match.toolName, summary, elapsed), result.isOk());
        if (!result) {
            failReply(describe(result.error()));
            return;
        }

        if (match.forwardToAgent) {
            if (!canUseModel(true)) {
                finishReply(FinishReason::Stop);
                return;
            }
            QList<ImageAttachment> images;
            if (result.value().image) images.append(*result.value().image);
            const QString question = match.agentPrompt.isEmpty() ? prompt : match.agentPrompt;
            conversation_.addUser(u"%1\n\n(%2)"_s.arg(forModel(question, source), result.value().text), images);
            runAgent(source);
            return;
        }
        // Multi-line results (command output, …) are shown in full under the step line.
        const QString details = result.value().text.section(u'\n', 1).trimmed();
        if (!details.isEmpty()) {
            emit replyDelta(u"```text\n"_s + (details.size() > kMaxShownOutput ? details.left(kMaxShownOutput) + u"\n…"_s
                                                                                 : details)
                            + u"\n```"_s);
        }
        // Keep the model's view of the conversation complete for later questions.
        conversation_.addUser(forModel(prompt, source));
        conversation_.addAssistant(u"[Done directly with %1: %2]"_s.arg(match.toolName, result.value().text));
        finishReply(FinishReason::Stop);
    });
}

void AppController::runAgent(const QString &source) {
    qCDebug(lcApp) << "agent:" << currentModel();
    activity_.fastPath = false;
    activity_.thinking = true;
    refreshCoreMode();
    emit replyRoute(u"agent"_s);
    services_.agent->run(provider_.get(), &conversation_, currentModel(), source);
}

bool AppController::canUseModel(bool report) {
    if (!provider_) {
        if (report) emit statusChanged(tr("No AI provider is configured — open Settings."), true);
        return false;
    }
    if (currentModel().isEmpty()) {
        if (report) emit statusChanged(tr("Choose a model first (model list in the toolbar)."), true);
        return false;
    }
    return true;
}

void AppController::cancel() {
    if (services_.voice) services_.voice->cancel();     // Esc also silences a reply being read out
    cancelReply();
}

void AppController::cancelReply() {
    if (!busy_) return;
    ++replyGeneration_;
    if (services_.agent->isRunning()) {
        services_.agent->cancel();  // emits finished(Cancelled) → finishReply
        return;
    }
    services_.executor->cancelAll();
    finishReply(FinishReason::Cancelled);
}

void AppController::panic() {
    cancel();
    services_.panic->panic();
    emit panicked();
}

void AppController::newConversation() {
    cancel();
    conversation_.clear();
    emit conversationCleared();
}

void AppController::setConfirmer(ToolExecutor::Confirmer confirmer) {
    services_.executor->setConfirmer(std::move(confirmer));
}

void AppController::applyVoiceConfig() {
    VoicePipeline *voice = services_.voice;
    if (!voice) return;
    const VoiceConfig &settings = config_.voice;
    const ProviderConfig *active = config_.active();
    QStringList problems;

    Result<std::unique_ptr<ISpeechRecognizer>> recognizer =
        VoiceBackends::createRecognizer(settings, active, services_.http);
    if (recognizer) {
        voice->setRecognizer(std::move(recognizer).value());
    } else {
        voice->setRecognizer(nullptr);
        problems.append(recognizer.error().message);
    }

    // The wake word listens all the time, so it only ever uses the local recogniser.
    std::unique_ptr<ISpeechRecognizer> wake;
    if (settings.wakeWord) {
        Result<std::unique_ptr<ISpeechRecognizer>> local = VoiceBackends::createLocalRecognizer(settings);
        if (local) {
            wake = std::move(local).value();
        } else {
            problems.append(tr("The wake word needs local speech recognition: %1.").arg(local.error().message));
        }
    }
    voice->setWakeRecognizer(std::move(wake));

    Result<std::unique_ptr<ITextToSpeech>> speech = VoiceBackends::createTextToSpeech(settings, active, services_.http);
    if (speech) {
        voice->setTextToSpeech(std::move(speech).value());
    } else {
        voice->setTextToSpeech(nullptr);
        problems.append(speech.error().message);
    }

    voice->setSettings(VoicePipeline::Settings{.speak = VoicePipeline::speakModeFromString(settings.speakReplies),
                                               .wakeWord = settings.wakeWord,
                                               .bargeIn = settings.bargeIn});
    qCDebug(lcApp) << "voice: recognition" << voice->recognizerName() << "| speech" << voice->speechName();
    if (!problems.isEmpty()) emit statusChanged(problems.join(u' '), true);
}

void AppController::beginReply(const QString &source) {
    ++replyGeneration_;
    replyHasContent_ = false;
    setBusy(true);
    emit replyStarted(source);
}

void AppController::finishReply(FinishReason reason) {
    if (!busy_) return;
    if (reason == FinishReason::Stop) setOffline(false);
    activity_.fastPath = false;
    activity_.thinking = false;
    refreshCoreMode();
    setBusy(false);
    emit replyFinished(reason, !replyHasContent_);
}

void AppController::failReply(const QString &message) {
    if (!busy_) return;
    activity_.fastPath = false;
    activity_.thinking = false;
    refreshCoreMode();
    setBusy(false);
    emit replyFailed(message);
}

void AppController::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged(busy);
}

void AppController::rebuildProvider() {
    provider_.reset();
    models_.clear();
    const ProviderConfig *active = config_.active();
    if (!active) {
        emit providerChanged({});
        emit statusChanged(tr("No AI provider is configured — open Settings."), true);
        return;
    }
    config_.activeProvider = active->id;
    emit providerChanged(active->name);

    Result<std::unique_ptr<IAiProvider>> created = services_.providers->create(*active);
    if (!created) {
        emit statusChanged(created.error().message, true);
        return;
    }
    provider_ = std::move(created).value();
    emit modelsChanged(models_, currentModel());
}

void AppController::persist() {
    if (const Result<void> saved = services_.store->save(config_); !saved) {
        emit statusChanged(tr("Could not save settings: %1").arg(saved.error().message), true);
    }
}

void AppController::refreshCoreMode() {
    const CoreMode mode = resolveCoreMode(activity_);
    if (mode == coreMode_) return;
    coreMode_ = mode;
    emit coreModeChanged(mode);
}

void AppController::noteProviderError(const Error &error) {
    if (isOffline(error)) setOffline(true, describe(error));
}

void AppController::setOffline(bool offline, const QString &reason) {
    if (offline) offlineReason_ = reason;
    if (activity_.offline == offline) return;
    activity_.offline = offline;
    if (!offline) offlineReason_.clear();
    refreshCoreMode();
}

ProviderConfig *AppController::activeProviderConfig() {
    if (ProviderConfig *chosen = config_.provider(config_.activeProvider)) return chosen;
    return config_.providers.isEmpty() ? nullptr : &config_.providers.first();
}

QString AppController::systemPrompt() const {
    QString prompt = config_.systemPrompt;
    if (services_.voice) {
        prompt += u"\n\nMessages that start with [voice] were spoken, and your reply to them is read aloud: answer "
                  "in one to three short sentences of plain text, without Markdown, lists or code."_s;
    }
    if (!environment_.isEmpty()) prompt += u"\n\nThis PC: "_s + environment_;
    return prompt;
}

QString AppController::describe(const Error &error) const {
    const ProviderConfig *active = config_.active();
    const QString name = active ? active->name : tr("The provider");
    switch (error.code) {
    case ErrorCode::ConnectionRefused:
        return tr("Cannot reach %1 at %2 — is it running?").arg(name, active ? active->baseUrl : QString());
    case ErrorCode::Unauthorized:
        return tr("%1 rejected the API key (%2). Set it in Settings → Providers.").arg(name, error.message);
    default:
        return error.message;
    }
}

} // namespace nexa
