#include "app/MainWindow.h"

#include "app/ActionLogDock.h"
#include "app/AppController.h"
#include "app/ChatView.h"
#include "app/GlobalShortcuts.h"
#include "app/MicButton.h"
#include "app/SettingsDialog.h"
#include "core/tools/ToolExecutor.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QAction>
#include <QComboBox>
#include <QCompleter>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Ids of the portal hotkeys (see app/main.cc).
const QLatin1StringView kTalkShortcut{"talk"};
const QLatin1StringView kPanicShortcut{"panic"};

} // namespace

MainWindow::MainWindow(AppController *controller, ProviderRegistry *registry, ToolExecutor *executor,
                       HttpClient *http, GlobalShortcuts *shortcuts, QWidget *parent)
    : QMainWindow(parent),
      controller_(controller),
      registry_(registry),
      executor_(executor),
      http_(http),
      shortcuts_(shortcuts) {
    setWindowTitle(tr("Nexa"));
    resize(1240, 760);
    buildUi();
    connectController();
    connectVoice();
}

void MainWindow::buildUi() {
    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(u"mainToolbar"_s);
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    providerLabel_ = new QLabel(this);
    providerLabel_->setContentsMargins(4, 0, 8, 0);
    toolbar->addWidget(providerLabel_);

    modelCombo_ = new QComboBox(this);
    modelCombo_->setEditable(true);
    modelCombo_->setInsertPolicy(QComboBox::NoInsert);
    modelCombo_->setMinimumContentsLength(30);
    modelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    modelCombo_->setToolTip(tr("Model used for replies (type to filter)"));
    modelCombo_->lineEdit()->setPlaceholderText(tr("model"));
    modelCombo_->completer()->setFilterMode(Qt::MatchContains);
    modelCombo_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    toolbar->addWidget(modelCombo_);
    connect(modelCombo_, &QComboBox::textActivated, controller_, &AppController::setCurrentModel);

    QAction *refresh = toolbar->addAction(QIcon::fromTheme(u"view-refresh"_s), tr("Refresh"));
    refresh->setToolTip(tr("Reload the model list"));
    connect(refresh, &QAction::triggered, controller_, &AppController::refreshModels);

    toolbar->addSeparator();
    QAction *newChat = toolbar->addAction(QIcon::fromTheme(u"document-new"_s), tr("New chat"));
    newChat->setShortcut(QKeySequence::New);
    connect(newChat, &QAction::triggered, controller_, &AppController::newConversation);

    QAction *seeScreen = toolbar->addAction(QIcon::fromTheme(u"camera-photo"_s), tr("See screen"));
    seeScreen->setToolTip(tr("Take a screenshot and ask about it (uses the text in the input box as the question)"));
    seeScreen->setShortcut(QKeySequence(u"Ctrl+Shift+S"_s));
    connect(seeScreen, &QAction::triggered, this, [this] { controller_->askAboutScreen(input_->toPlainText()); });

    actionLog_ = new ActionLogDock(this);
    addDockWidget(Qt::RightDockWidgetArea, actionLog_);
    resizeDocks({actionLog_}, {420}, Qt::Horizontal);
    QAction *showActions = actionLog_->toggleViewAction();
    showActions->setIcon(QIcon::fromTheme(u"view-list-details"_s));
    showActions->setShortcut(QKeySequence(u"Ctrl+L"_s));
    toolbar->addAction(showActions);

    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    QAction *settings = toolbar->addAction(QIcon::fromTheme(u"preferences-system"_s), tr("Settings"));
    settings->setShortcut(QKeySequence(u"Ctrl+,"_s));
    connect(settings, &QAction::triggered, this, &MainWindow::openSettings);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    chatView_ = new ChatView(central);
    layout->addWidget(chatView_, 1);

    auto *inputRow = new QHBoxLayout;
    input_ = new QPlainTextEdit(central);
    input_->setPlaceholderText(tr("Ask Nexa…  (Enter to send, Shift+Enter for a new line)"));
    input_->setTabChangesFocus(true);
    input_->setFixedHeight(input_->fontMetrics().lineSpacing() * 3 + 16);
    input_->installEventFilter(this);
    inputRow->addWidget(input_, 1);

    if (VoicePipeline *voice = controller_->voice()) {
        micButton_ = new MicButton(voice, central);
        micButton_->setFixedHeight(input_->height());
        inputRow->addWidget(micButton_);
        auto *talk = new QAction(this);
        talk->setShortcut(QKeySequence(u"Ctrl+M"_s));
        connect(talk, &QAction::triggered, voice, &VoicePipeline::toggleListening);
        addAction(talk);
    }

    sendButton_ = new QPushButton(tr("Send"), central);
    sendButton_->setMinimumWidth(88);
    sendButton_->setFixedHeight(input_->height());
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::sendOrStop);
    inputRow->addWidget(sendButton_);
    layout->addLayout(inputRow);
    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(statusLabel_, 1);
    voiceLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(voiceLabel_);

    auto *stop = new QAction(this);
    stop->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(stop, &QAction::triggered, controller_, &AppController::cancel);
    addAction(stop);

    // Panic: stop the reply, every tool and every child process. GNOME may keep Ctrl+Alt+Esc
    // for itself, so Ctrl+Shift+Esc works too.
    auto *panic = new QAction(this);
    panic->setShortcuts({QKeySequence(u"Ctrl+Alt+Esc"_s), QKeySequence(u"Ctrl+Shift+Esc"_s)});
    panic->setShortcutContext(Qt::ApplicationShortcut);
    connect(panic, &QAction::triggered, controller_, &AppController::panic);
    addAction(panic);

    input_->setFocus();
}

void MainWindow::connectController() {
    connect(controller_, &AppController::providerChanged, this, [this](const QString &name) {
        providerLabel_->setText(name.isEmpty() ? tr("No provider") : name);
    });
    connect(controller_, &AppController::modelsChanged, this, &MainWindow::showModels);
    connect(controller_, &AppController::statusChanged, this, &MainWindow::showStatus);
    // Queued: the prompt is modal and must not run inside a network callback.
    connect(controller_, &AppController::apiKeyRequired, this, &MainWindow::promptForApiKey, Qt::QueuedConnection);

    connect(controller_, &AppController::userMessageAccepted, this, [this](const QString &text) {
        chatView_->addUserMessage(text);
        input_->clear();
    });
    connect(controller_, &AppController::replyStarted, chatView_, &ChatView::beginAssistantMessage);
    connect(controller_, &AppController::replyDelta, chatView_, &ChatView::appendAssistantDelta);
    connect(controller_, &AppController::replyFinished, this, [this](FinishReason reason, bool empty) {
        chatView_->endAssistantMessage();
        if (reason == FinishReason::Cancelled) {
            chatView_->addNotice(tr("Stopped."));
        } else if (reason == FinishReason::Length) {
            chatView_->addNotice(tr("The reply was cut off (token limit)."));
        } else if (reason == FinishReason::ContentFilter) {
            chatView_->addNotice(tr("The provider filtered this reply."));
        } else if (empty) {
            chatView_->addNotice(tr("(empty reply)"));
        }
    });
    connect(controller_, &AppController::replyFailed, this, [this](const QString &message) {
        chatView_->endAssistantMessage();
        chatView_->addError(message);
    });
    connect(controller_, &AppController::replyStep, chatView_, &ChatView::addStep);
    connect(controller_, &AppController::panicked, this, [this] { chatView_->addNotice(tr("Stopped everything.")); });
    connect(controller_, &AppController::busyChanged, this, &MainWindow::setBusy);
    connect(controller_, &AppController::conversationCleared, chatView_, &ChatView::clearTranscript);

    connect(executor_, &ToolExecutor::actionStarted, actionLog_, &ActionLogDock::actionStarted);
    connect(executor_, &ToolExecutor::actionFinished, actionLog_, &ActionLogDock::actionFinished);
    controller_->setConfirmer([this](const QString &question, std::function<void(bool)> answer) {
        confirm(question, std::move(answer));
    });
}

void MainWindow::connectVoice() {
    VoicePipeline *voice = controller_->voice();
    if (!voice) return;
    connect(voice, &VoicePipeline::stateChanged, this, &MainWindow::showVoiceState);
    connect(voice, &VoicePipeline::notice, this, &MainWindow::showStatus);
    connect(voice, &VoicePipeline::settingsChanged, this, [this] { showVoiceState(controller_->voice()->state()); });
    showVoiceState(voice->state());

    if (!shortcuts_) return;
    // Hold the granted hotkey to talk from any window; a tap listens until the speaker pauses.
    connect(shortcuts_, &GlobalShortcuts::pressed, this, [this, voice](const QString &id) {
        if (id == kTalkShortcut) {
            voice->startListening(true);
        } else if (id == kPanicShortcut) {
            controller_->panic();
        }
    });
    connect(shortcuts_, &GlobalShortcuts::released, this, [voice](const QString &id) {
        if (id == kTalkShortcut) voice->releaseHold();
    });
    connect(shortcuts_, &GlobalShortcuts::registered, this, &MainWindow::showHotkeys);
}

void MainWindow::showHotkeys() {
    if (!micButton_) return;
    const QString talk = shortcuts_->trigger(kTalkShortcut);
    if (talk.isEmpty()) {
        // Registered with the desktop, but no key assigned to it yet.
        micButton_->setToolTip(tr("Click to speak a request (it ends when you pause), or hold to talk.\n"
                                  "In this window: Ctrl+M. For a key that works anywhere, assign one to "
                                  "Nexa Assistant in Settings → Keyboard."));
        showStatus(tr("To talk to Nexa from any window, assign a key to it in Settings → Keyboard."), false);
        return;
    }
    micButton_->setToolTip(tr("Click to speak a request (it ends when you pause), or hold to talk.\n"
                              "From any window: hold %1. In this window: Ctrl+M.")
                               .arg(talk));
    showStatus(tr("Hold %1 anywhere to talk to Nexa.").arg(talk), false);
}

void MainWindow::showVoiceState(VoiceState state) {
    const VoicePipeline *voice = controller_->voice();
    QString text;
    switch (state) {
    case VoiceState::Listening: text = tr("Listening…"); break;
    case VoiceState::Transcribing: text = tr("Recognising speech…"); break;
    case VoiceState::Thinking: text = tr("Thinking…"); break;
    case VoiceState::Speaking: text = tr("Speaking…"); break;
    case VoiceState::Idle:
        if (voice->wakeWordActive()) text = tr("Say “Hey Nexa”");
        break;
    }
    voiceLabel_->setText(text);
}

void MainWindow::confirm(const QString &question, std::function<void(bool)> answer) {
    // Non-blocking: the action waits for the answer without a nested event loop.
    auto *box = new QMessageBox(QMessageBox::Question, tr("Allow this action?"), question,
                                QMessageBox::Yes | QMessageBox::No, this);
    box->setDefaultButton(QMessageBox::No);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [answer = std::move(answer)](int result) {
        answer(result == QMessageBox::Yes);
    });
    box->open();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (enter && !(key->modifiers() & Qt::ShiftModifier)) {
            if (!controller_->isBusy()) controller_->submit(input_->toPlainText());
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::sendOrStop() {
    if (controller_->isBusy()) {
        controller_->cancel();
    } else {
        controller_->submit(input_->toPlainText());
    }
}

void MainWindow::openSettings() {
    SettingsDialog dialog(controller_->config(), registry_, http_, this);
    if (dialog.exec() == QDialog::Accepted) controller_->updateConfig(dialog.config());
}

void MainWindow::promptForApiKey(const QString &providerName) {
    if (promptingForKey_) return;
    promptingForKey_ = true;
    bool ok = false;
    const QString key = QInputDialog::getText(
        this, tr("API key"),
        tr("Enter the API key for %1.\nIt is stored in ~/.config/nexa/config.json, readable only by you.")
            .arg(providerName),
        QLineEdit::Password, {}, &ok);
    promptingForKey_ = false;
    if (ok && !key.trimmed().isEmpty()) {
        controller_->setApiKey(key);
    } else {
        showStatus(tr("No API key set — add one in Settings → Providers."), true);
    }
}

void MainWindow::showModels(const QStringList &models, const QString &current) {
    const QSignalBlocker blocker(modelCombo_);
    modelCombo_->clear();
    modelCombo_->addItems(models);
    const int index = modelCombo_->findText(current);
    if (index >= 0) {
        modelCombo_->setCurrentIndex(index);
    } else {
        modelCombo_->setEditText(current);
    }
}

void MainWindow::showStatus(const QString &message, bool isError) {
    statusLabel_->setText(message);
    statusLabel_->setStyleSheet(isError ? u"color: #e5534b;"_s : QString());
}

void MainWindow::setBusy(bool busy) {
    sendButton_->setText(busy ? tr("Stop") : tr("Send"));
    modelCombo_->setEnabled(!busy);
    if (!busy) input_->setFocus();
}

} // namespace nexa
