#include "app/MainWindow.h"

#include "app/ActionLogDock.h"
#include "app/AppController.h"
#include "app/ChatView.h"
#include "app/GlobalShortcuts.h"
#include "app/MicButton.h"
#include "app/PhonePanel.h"
#include "app/SettingsDialog.h"
#include "core/tools/ToolExecutor.h"
#include "remote/RemoteServer.h"
#include "system/android/AndroidDevice.h"
#include "system/browser/BrowserService.h"
#include "system/helper/HelperClient.h"
#include "ui/NexaCoreWidget.h"
#include "voice/pipeline/VoicePipeline.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
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
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <initializer_list>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// Ids of the portal hotkeys (see app/main.cc).
const QLatin1StringView kTalkShortcut{"talk"};
const QLatin1StringView kPanicShortcut{"panic"};

constexpr int kInputHeight = 50;

QString policyWords(const QString &mode) {
    if (mode == u"confirm-risky"_s) return QObject::tr("Confirm risky");
    if (mode == u"confirm-all"_s) return QObject::tr("Confirm all");
    return QObject::tr("Autonomous");
}

// A dynamic property the stylesheet selects on ([busy="true"]) takes effect after a repolish.
void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

// "OpenRouter anyapi/google/gemini-2.5-flash-lite → whisper.cpp (base.en)" becomes
// "gemini-2.5-flash-lite, then whisper.cpp base.en": the status bar has one line for all of it.
QString shortEngineNames(const QString &chain, const QString &provider) {
    QStringList links;
    for (QString link : chain.split(u" → "_s, Qt::SkipEmptyParts)) {
        if (!provider.isEmpty() && link.startsWith(provider + u' ')) link.remove(0, provider.size() + 1);
        const QStringList segments = link.split(u'/');
        if (segments.size() >= 3) link = segments.last();
        // "Piper (en_US-lessac-medium)": the engine is the news, the voice is in the tooltip.
        if (link.startsWith(u"Piper"_s)) link = u"Piper"_s;
        link.remove(u'(').remove(u')');
        links.append(link.trimmed());
    }
    return links.join(u", then "_s);
}

} // namespace

MainWindow::MainWindow(AppController *controller, ProviderRegistry *registry, ToolExecutor *executor,
                       HttpClient *http, GlobalShortcuts *shortcuts, SystemStatus status, const QString &auditPath,
                       QWidget *parent)
    : QMainWindow(parent),
      controller_(controller),
      registry_(registry),
      executor_(executor),
      http_(http),
      shortcuts_(shortcuts),
      status_(std::move(status)),
      auditPath_(auditPath),
      pack_(NexaThemes::resolve(controller->config().ui.theme)),
      design_(NexaDesigns::resolve(controller->config().ui.design)) {
    setWindowTitle(tr("Nexa"));
    resize(1280, 800);
    buildToolbar();
    buildCentral();
    buildStatusBar();
    applyDesign(design_);
    connectController();
    connectVoice();
    refreshTheme();
    showPolicy();
}

void MainWindow::buildToolbar() {
    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setObjectName(u"mainToolbar"_s);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setContextMenuPolicy(Qt::PreventContextMenu);

    providerLabel_ = new QLabel(this);
    providerLabel_->setObjectName(u"pillValue"_s);
    toolbar->addWidget(makePill(tr("Provider"), providerLabel_));

    modelCombo_ = new QComboBox(this);
    modelCombo_->setObjectName(u"modelCombo"_s);
    modelCombo_->setEditable(true);
    modelCombo_->setInsertPolicy(QComboBox::NoInsert);
    modelCombo_->setMinimumContentsLength(18);
    modelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    modelCombo_->setToolTip(tr("Model used for replies (type to filter)"));
    modelCombo_->lineEdit()->setPlaceholderText(tr("model"));
    modelCombo_->completer()->setFilterMode(Qt::MatchContains);
    modelCombo_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    connect(modelCombo_, &QComboBox::textActivated, controller_, &AppController::setCurrentModel);
    toolbar->addWidget(makePill(tr("Model"), modelCombo_));

    auto *refresh = new QAction(tr("Reload the model list"), this);
    connect(refresh, &QAction::triggered, controller_, &AppController::refreshModels);
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::Refresh, u"iconButton"_s, refresh));

    auto *newChat = new QAction(tr("New chat"), this);
    newChat->setShortcut(QKeySequence::New);
    connect(newChat, &QAction::triggered, controller_, &AppController::newConversation);
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::NewChat, u"iconButton"_s, newChat));

    actionLog_ = new ActionLogDock(auditPath_, this);
    addDockWidget(Qt::RightDockWidgetArea, actionLog_);
    resizeDocks({actionLog_}, {340}, Qt::Horizontal);
    QAction *showActions = actionLog_->toggleViewAction();
    showActions->setText(tr("Actions panel"));
    showActions->setShortcut(QKeySequence(u"Ctrl+L"_s));
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::Actions, u"iconButton"_s, showActions));

    // The phone beside the chat while one is connected; the button brings it back by hand.
    phonePanel_ = new PhonePanel(status_.phone, this);
    addDockWidget(Qt::RightDockWidgetArea, phonePanel_);
    phonePanel_->hide();
    QAction *showPhonePanel = phonePanel_->toggleViewAction();
    showPhonePanel->setText(tr("Phone panel"));
    showPhonePanel->setToolTip(tr("The phone's screen, live, while one is connected · Ctrl+P"));
    showPhonePanel->setShortcut(QKeySequence(u"Ctrl+P"_s));
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::Phone, u"iconButton"_s, showPhonePanel));

    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    policyButton_ = new QToolButton(this);
    policyButton_->setObjectName(u"pillButton"_s);
    policyButton_->setToolTip(tr("When Nexa asks before acting. Click to change it (Settings → Policy)."));
    policyButton_->setCursor(Qt::PointingHandCursor);
    connect(policyButton_, &QToolButton::clicked, this, [this] { openSettings(tr("Policy")); });
    toolbar->addWidget(policyButton_);

    auto *stopAll = new QPushButton(tr("Stop all · Ctrl+Alt+Esc"), this);
    stopAll->setObjectName(u"stopAll"_s);
    stopAll->setToolTip(tr("Stop the reply, every tool and every child process (also Ctrl+Shift+Esc)"));
    stopAll->setCursor(Qt::PointingHandCursor);
    connect(stopAll, &QPushButton::clicked, controller_, &AppController::panic);
    toolbar->addWidget(stopAll);

    themeAction_ = new QAction(tr("Switch between dark and light"), this);
    connect(themeAction_, &QAction::triggered, this, &MainWindow::toggleTheme);
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::Sun, u"iconButton"_s, themeAction_));
    themedActions_.pop_back();  // its icon depends on the scheme; refreshTheme() sets it

    auto *settings = new QAction(tr("Settings"), this);
    settings->setShortcut(QKeySequence(u"Ctrl+,"_s));
    connect(settings, &QAction::triggered, this, [this] { openSettings(); });
    toolbar->addWidget(makeIconButton(NexaIcons::Kind::Gear, u"iconButton"_s, settings));
}

void MainWindow::buildCentral() {
    // The pieces; applyDesign() arranges them as the design says.
    // The core, with the state's name beside it — never colour alone.
    header_ = new QWidget(this);
    header_->setObjectName(u"stateHeader"_s);
    headerLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, header_);
    headerLayout_->setContentsMargins(24, 12, 24, 12);
    headerLayout_->setSpacing(18);
    presence_ = new NexaCoreWidget(header_);
    presence_->setDiameter(88);
    presence_->setShowNodes(true);
    headerLayout_->addWidget(presence_);
    titles_ = new QWidget(header_);
    auto *titles = new QVBoxLayout(titles_);
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    stateTitle_ = new QLabel(titles_);
    stateTitle_->setObjectName(u"stateTitle"_s);
    stateHint_ = new QLabel(titles_);
    stateHint_->setObjectName(u"stateHint"_s);
    stateHint_->setWordWrap(true);
    titles->addStretch();
    titles->addWidget(stateTitle_);
    titles->addWidget(stateHint_);
    titles->addStretch();
    headerLayout_->addWidget(titles_, 1);
    headerLayout_->addStretch(0);   // stacked in a column, the words stay under the core

    chatView_ = new ChatView(this);

    QWidget *bar = inputBar_ = new QWidget(this);
    bar->setObjectName(u"inputBar"_s);
    auto *inputRow = new QHBoxLayout(bar);
    inputRow->setContentsMargins(20, 12, 20, 14);
    inputRow->setSpacing(10);
    input_ = new QPlainTextEdit(bar);
    input_->setObjectName(u"input"_s);
    input_->setPlaceholderText(tr("Type a request…  (Enter to send, Shift+Enter for a new line)"));
    input_->setTabChangesFocus(true);
    input_->setFixedHeight(kInputHeight);
    input_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    input_->installEventFilter(this);
    inputRow->addWidget(input_, 1);

    // Share screen: a switch, off at every start. On, the model may take screenshots to see what
    // you see; off, it works through commands and tools and never looks.
    shareScreenAction_ = new QAction(tr("Share screen"), this);
    shareScreenAction_->setCheckable(true);
    shareScreenAction_->setToolTip(tr("Share your screen with Nexa: while this is on, the model may take screenshots "
                                      "to see what you see. Off, it works through commands and tools without looking "
                                      "· Ctrl+Shift+S"));
    shareScreenAction_->setShortcut(QKeySequence(u"Ctrl+Shift+S"_s));
    connect(shareScreenAction_, &QAction::toggled, this, [this](bool on) {
        if (controller_->isScreenShared() == on) return;
        controller_->setScreenShared(on);
        chatView_->addNotice(on ? tr("Screen shared — Nexa may look at it while this is on.")
                                : tr("Screen sharing stopped — Nexa works without looking at the screen."));
    });
    inputRow->addWidget(makeIconButton(NexaIcons::Kind::Pc, u"inputButton"_s, shareScreenAction_));

    auto *seePhone = new QAction(tr("See phone"), this);
    seePhone->setToolTip(tr("Take a screenshot of the phone and ask about it (the text in the box is the question) · "
                            "Ctrl+Shift+P"));
    seePhone->setShortcut(QKeySequence(u"Ctrl+Shift+P"_s));
    connect(seePhone, &QAction::triggered, this, [this] { controller_->askAboutPhone(input_->toPlainText()); });
    inputRow->addWidget(makeIconButton(NexaIcons::Kind::Phone, u"inputButton"_s, seePhone));

    sendButton_ = new QPushButton(tr("Send"), bar);
    sendButton_->setObjectName(u"send"_s);
    sendButton_->setMinimumWidth(84);
    sendButton_->setFixedHeight(kInputHeight);
    sendButton_->setCursor(Qt::PointingHandCursor);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::sendOrStop);
    inputRow->addWidget(sendButton_);

    if (VoicePipeline *voice = controller_->voice()) {
        micButton_ = new MicButton(voice, bar);
        inputRow->addWidget(micButton_);
        auto *talk = new QAction(this);
        talk->setShortcut(QKeySequence(u"Ctrl+M"_s));
        connect(talk, &QAction::triggered, voice, &VoicePipeline::toggleListening);
        addAction(talk);
    }

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

// Where everything goes, for the design on: the header beside the chat or stacked above it, the
// chat full width or a centred column, the Actions panel on a side or along the bottom or kept
// out of sight, the parts as cards. The pieces are built once; only their arrangement changes.
void MainWindow::applyDesign(const DesignSpec &design) {
    design_ = design;
    for (NexaCoreWidget *core : {presence_, statusCore_}) {
        if (core) core->setShape(design.shape);
    }
    if (micButton_) micButton_->setShape(design.shape);
    rebuildCentral();
    showCoreMode(controller_->coreMode());
}

void MainWindow::rebuildCentral() {
    const WindowLayout layout = design_.layout;
    // A piece the new arrangement leaves out (Console has no header) must not go down with the
    // old central widget: every piece is taken back first, and placed again below.
    for (QWidget *piece : std::initializer_list<QWidget *>{header_, chatView_, inputBar_}) {
        if (piece->parentWidget() != this) piece->setParent(this);
    }
    auto *central = new QWidget(this);
    auto *column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    // The header: beside the chat, or stacked and centred; its size by layout.
    const bool stacked = layout == WindowLayout::Focus || layout == WindowLayout::Orb || layout == WindowLayout::Sidebar;
    headerLayout_->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    headerLayout_->setAlignment(presence_, stacked ? Qt::AlignHCenter : Qt::AlignVCenter);
    headerLayout_->setStretchFactor(titles_, stacked ? 0 : 1);
    titles_->setSizePolicy(QSizePolicy::Preferred, stacked ? QSizePolicy::Maximum : QSizePolicy::Preferred);
    headerLayout_->setContentsMargins(stacked ? QMargins(24, 24, 24, 12) : QMargins(24, 12, 24, 12));
    stateTitle_->setAlignment(stacked ? Qt::AlignHCenter : Qt::AlignLeft);
    stateHint_->setAlignment(stacked ? Qt::AlignHCenter : Qt::AlignLeft);
    stateTitle_->setObjectName(layout == WindowLayout::Orb ? u"orbTitle"_s : u"stateTitle"_s);
    repolish(stateTitle_);
    int diameter = 88;
    switch (layout) {
    case WindowLayout::Focus: diameter = 160; break;
    case WindowLayout::Orb: diameter = 240; break;
    case WindowLayout::Sidebar: diameter = 140; break;
    case WindowLayout::Dashboard: diameter = 96; break;
    case WindowLayout::Bottom: diameter = 72; break;
    default: break;
    }
    presence_->setDiameter(diameter);
    header_->setVisible(layout != WindowLayout::Console);
    header_->setMaximumWidth(layout == WindowLayout::Sidebar ? 300 : QWIDGETSIZE_MAX);
    header_->setMinimumWidth(layout == WindowLayout::Sidebar ? 240 : 0);
    header_->setProperty("tile", layout == WindowLayout::Dashboard);
    repolish(header_);

    // Centred layouts keep the conversation to a readable column.
    const int maxWidth = layout == WindowLayout::Focus ? 820 : layout == WindowLayout::Orb ? 760 : QWIDGETSIZE_MAX;
    chatView_->setMaximumWidth(maxWidth);
    inputBar_->setMaximumWidth(maxWidth);
    chatView_->setProperty("tile", layout == WindowLayout::Dashboard);
    repolish(chatView_);
    input_->setPlaceholderText(layout == WindowLayout::Console ? tr("› type a request  (Enter to send)")
                               : maxWidth < 900                ? tr("Type a request…")
                                                               : tr("Type a request…  (Enter to send, Shift+Enter for a new line)"));

    const auto centred = [](QWidget *widget) {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addStretch();
        row->addWidget(widget, 1);
        row->addStretch();
        return row;
    };
    const auto tile = [central](QWidget *widget) {
        auto *frame = new QFrame(central);
        frame->setObjectName(u"tile"_s);
        auto *inside = new QVBoxLayout(frame);
        inside->setContentsMargins(6, 6, 6, 6);
        inside->addWidget(widget);
        return frame;
    };

    switch (layout) {
    case WindowLayout::Studio:
    case WindowLayout::Bottom:
        column->addWidget(header_);
        column->addWidget(chatView_, 1);
        column->addWidget(inputBar_);
        break;
    case WindowLayout::Focus:
    case WindowLayout::Orb:
        column->addWidget(header_);
        column->addLayout(centred(chatView_), 1);
        column->addLayout(centred(inputBar_));
        break;
    case WindowLayout::Console:
        column->addWidget(chatView_, 1);
        column->addWidget(inputBar_);
        break;
    case WindowLayout::Sidebar: {
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        row->addWidget(header_);
        auto *right = new QVBoxLayout;
        right->setContentsMargins(0, 0, 0, 0);
        right->setSpacing(0);
        right->addWidget(chatView_, 1);
        right->addWidget(inputBar_);
        row->addLayout(right, 1);
        column->addLayout(row, 1);
        break;
    }
    case WindowLayout::Dashboard:
        column->setContentsMargins(12, 12, 12, 12);
        column->setSpacing(12);
        column->addWidget(tile(header_));
        column->addWidget(tile(chatView_), 1);
        column->addWidget(tile(inputBar_));
        break;
    }
    // The pieces have moved into the new central widget; the old one, empty now, goes with it.
    setCentralWidget(central);

    // The Actions panel: on a side, along the bottom, or out of sight until asked (Ctrl+L).
    const Qt::DockWidgetArea area = (layout == WindowLayout::Console || layout == WindowLayout::Bottom)
                                        ? Qt::BottomDockWidgetArea
                                        : layout == WindowLayout::Sidebar ? Qt::LeftDockWidgetArea : Qt::RightDockWidgetArea;
    removeDockWidget(actionLog_);
    addDockWidget(area, actionLog_);
    const bool shown = !(layout == WindowLayout::Focus || layout == WindowLayout::Orb);
    actionLog_->setVisible(shown);
    if (area == Qt::BottomDockWidgetArea) {
        resizeDocks({actionLog_}, {180}, Qt::Vertical);
    } else {
        resizeDocks({actionLog_}, {340}, Qt::Horizontal);
    }

    // The Phone panel: on the right, outside the Actions panel when that is there too; in view
    // while a phone is connected.
    removeDockWidget(phonePanel_);
    addDockWidget(Qt::RightDockWidgetArea, phonePanel_);
    if (shown && area == Qt::RightDockWidgetArea) splitDockWidget(actionLog_, phonePanel_, Qt::Horizontal);
    showPhonePanel(phonePanel_->isConnected());

    // Console: the state lives in the status line.
    statusCore_->setVisible(layout == WindowLayout::Console);
    statusState_->setVisible(layout == WindowLayout::Console);
}

void MainWindow::buildStatusBar() {
    statusBar()->setSizeGripEnabled(false);
    // Console keeps the core and the state's name here; the other designs hide them.
    statusCore_ = new NexaCoreWidget(this);
    statusCore_->setDiameter(20);
    statusCore_->setShowNodes(false);
    statusCore_->hide();
    statusBar()->addWidget(statusCore_);
    statusState_ = new QLabel(this);
    statusState_->setObjectName(u"statusState"_s);
    statusState_->hide();
    statusBar()->addWidget(statusState_);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(u"statusMessage"_s);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // It takes what the permanent labels leave and elides (see eventFilter), never widening the window.
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLabel_->installEventFilter(this);
    statusBar()->addWidget(statusLabel_, 1);
    // Shown with the microphone's trouble: steps the input volume down until nothing clips.
    lowerMicButton_ = new QToolButton(this);
    lowerMicButton_->setObjectName(u"lowerMicrophone"_s);
    lowerMicButton_->setText(tr("Lower it"));
    lowerMicButton_->setToolTip(tr("Lower the microphone's input volume until its sound is no longer clipped"));
    lowerMicButton_->hide();
    connect(lowerMicButton_, &QToolButton::clicked, this, [this] {
        lowerMicButton_->hide();
        controller_->lowerMicrophone();
    });
    statusBar()->addWidget(lowerMicButton_);
    // Shown when a program (agy, opencode, Codex) answers that it is not signed in: opens Settings → Providers,
    // where its Sign in button is.
    signInButton_ = new QToolButton(this);
    signInButton_->setObjectName(u"signInRequired"_s);
    signInButton_->setText(tr("Sign in…"));
    signInButton_->setToolTip(tr("Open Settings → Providers, where the provider can be signed in"));
    signInButton_->hide();
    connect(signInButton_, &QToolButton::clicked, this, [this] {
        signInButton_->hide();
        openSettings(tr("Providers"));
    });
    statusBar()->addWidget(signInButton_);

    appLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(appLabel_);
    showAppContext(controller_->appContextName(), controller_->appContextHint());

    screenLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(screenLabel_);
    showScreenSharing(controller_->isScreenShared());

    if (status_.browser) {
        // Where the browser tools work; installing Playwright or adding the extension changes it.
        browserLabel_ = new QLabel(this);
        statusBar()->addPermanentWidget(browserLabel_);
        showBrowser();
        connect(status_.browser, &BrowserService::stateChanged, this, &MainWindow::showBrowser);
    }

    helperLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(helperLabel_);
    showHelper();
    // Installing the helper changes this while Nexa runs.
    if (status_.helper) connect(status_.helper, &HelperClient::readyChanged, this, &MainWindow::showHelper);

    if (controller_->voice()) {
        sttLabel_ = new QLabel(this);
        ttsLabel_ = new QLabel(this);
        statusBar()->addPermanentWidget(sttLabel_);
        statusBar()->addPermanentWidget(ttsLabel_);
        showVoiceEngines();
    }

    if (status_.phone) {
        // Which phone the android_* tools reach; it changes as phones come and go.
        phoneLabel_ = new QLabel(this);
        statusBar()->addPermanentWidget(phoneLabel_);
        showPhone(status_.phone->currentDescription());
        connect(status_.phone, &AndroidDevice::deviceChanged, this, &MainWindow::showPhone);
    }
    if (status_.remote) {
        remoteLabel_ = new QLabel(this);
        statusBar()->addPermanentWidget(remoteLabel_);
        showRemote();
        connect(status_.remote, &RemoteServer::runningChanged, this, &MainWindow::showRemote);
        connect(status_.remote, &RemoteServer::clientsChanged, this, &MainWindow::showRemote);
    }
}

QWidget *MainWindow::makePill(const QString &label, QWidget *value) {
    auto *pill = new QFrame(this);
    pill->setObjectName(u"pill"_s);
    auto *layout = new QHBoxLayout(pill);
    layout->setContentsMargins(10, 5, 10, 5);
    layout->setSpacing(6);
    auto *caption = new QLabel(label, pill);
    caption->setObjectName(u"pillLabel"_s);
    layout->addWidget(caption);
    layout->addWidget(value);
    return pill;
}

QToolButton *MainWindow::makeIconButton(NexaIcons::Kind kind, const QString &objectName, QAction *action) {
    if (action->toolTip() == action->text() && !action->shortcut().isEmpty()) {
        action->setToolTip(u"%1 · %2"_s.arg(action->text(), action->shortcut().toString(QKeySequence::NativeText)));
    }
    auto *button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setCursor(Qt::PointingHandCursor);
    button->setIconSize(objectName == u"inputButton"_s ? QSize(22, 22) : QSize(18, 18));
    themedActions_.push_back({action, kind});
    return button;
}

void MainWindow::connectController() {
    connect(controller_, &AppController::providerChanged, this, [this](const QString &name) {
        providerLabel_->setText(name.isEmpty() ? tr("none") : name);
        if (signInButton_) signInButton_->hide();   // the providers were built again: it is asked again if still not signed in
    });
    connect(controller_, &AppController::modelsChanged, this, &MainWindow::showModels);
    // An empty list while a program lists its models reads as broken; say what is happening instead.
    connect(controller_, &AppController::modelsLoading, this, [this](bool loading) {
        modelCombo_->lineEdit()->setPlaceholderText(loading ? tr("loading models…") : tr("model"));
    });
    connect(controller_, &AppController::statusChanged, this, &MainWindow::showStatus);
    connect(controller_, &AppController::microphoneOverloaded, this, &MainWindow::showMicrophoneTrouble);
    // Queued: the prompt is modal and must not run inside a network callback.
    connect(controller_, &AppController::apiKeyRequired, this, &MainWindow::promptForApiKey, Qt::QueuedConnection);
    connect(controller_, &AppController::signInRequired, this, &MainWindow::showSignInRequired);

    connect(controller_, &AppController::userMessageAccepted, this, [this](const QString &text, const QString &source) {
        chatView_->addUserMessage(text, source);
        input_->clear();
    });
    connect(controller_, &AppController::replyStarted, this, [this] { chatView_->beginAssistantMessage(); });
    connect(controller_, &AppController::replyRoute, chatView_, &ChatView::setReplyRoute);
    connect(controller_, &AppController::replyVia, chatView_, &ChatView::setReplyVia);
    connect(controller_, &AppController::replyDelta, chatView_, &ChatView::appendAssistantDelta);
    connect(controller_, &AppController::replyNotice, chatView_, &ChatView::addNotice);
    connect(controller_, &AppController::replyAction, chatView_, &ChatView::addAction);
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
    connect(controller_, &AppController::panicked, this, [this] { chatView_->addNotice(tr("Stopped everything.")); });
    connect(controller_, &AppController::busyChanged, this, &MainWindow::setBusy);
    connect(controller_, &AppController::conversationCleared, chatView_, &ChatView::clearTranscript);
    connect(controller_, &AppController::coreModeChanged, this, &MainWindow::showCoreMode);
    connect(controller_, &AppController::litNodesChanged, this, [this](unsigned nodes) { presence_->setLitNodes(nodes); });
    connect(controller_, &AppController::screenSharingChanged, this, &MainWindow::showScreenSharing);
    connect(controller_, &AppController::appContextChanged, this, &MainWindow::showAppContext);
    connect(phonePanel_, &PhonePanel::phoneChanged, this, &MainWindow::showPhonePanel);
    connect(controller_, &AppController::themeChanged, this,
            [this](const QString &theme) { applyPack(NexaThemes::resolve(theme)); });
    connect(controller_, &AppController::configChanged, this, [this](const Config &updated) {
        showPolicy();
        if (status_.phone) showPhone(status_.phone->currentDescription());
        // A new design: the pieces move, and the sheet is cut to its corners and type.
        const DesignSpec design = NexaDesigns::resolve(updated.ui.design);
        if (design.id != design_.id) {
            applyDesign(design);
            NexaTheme::apply(qApp, pack_.tokens, pack_.style, design_);
            refreshTheme();
        }
    });

    connect(executor_, &ToolExecutor::actionStarted, this,
            [this](quint64 id, const QString &tool, const QJsonObject &arguments, const QString &source) {
                currentTool_ = tool;
                actionLog_->actionStarted(id, tool, arguments, source);
                const CoreMode mode = controller_->coreMode();
                if (mode == CoreMode::Acting || mode == CoreMode::FastPath) stateHint_->setText(hintFor(mode));
            });
    connect(executor_, &ToolExecutor::actionFinished, actionLog_, &ActionLogDock::actionFinished);
    controller_->setConfirmer([this](const ConfirmRequest &request, std::function<void(bool)> answer) {
        confirm(request.question, std::move(answer));
    });
}

void MainWindow::connectVoice() {
    VoicePipeline *voice = controller_->voice();
    if (!voice) return;
    connect(voice, &VoicePipeline::levelChanged, this, [this](double level) { presence_->setLevel(level); });
    connect(voice, &VoicePipeline::notice, this, &MainWindow::showStatus);
    connect(voice, &VoicePipeline::settingsChanged, this, [this] {
        showVoiceEngines();
        showCoreMode(controller_->coreMode());
    });

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

void MainWindow::closeEvent(QCloseEvent *event) {
    if (closeToTray_ && closeToTray_()) {
        event->ignore();
        hide();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showCoreMode(CoreMode mode) {
    presence_->setMode(mode);
    if (micButton_) micButton_->setMode(mode);
    if (statusCore_) statusCore_->setMode(mode);
    stateTitle_->setText(titleFor(mode));
    stateTitle_->setStyleSheet(u"color: %1;"_s.arg(presence_->colorFor(mode).name()));
    stateHint_->setText(hintFor(mode));
    if (statusState_) {
        statusState_->setText(titleFor(mode));
        statusState_->setStyleSheet(u"color: %1;"_s.arg(presence_->colorFor(mode).name()));
        statusState_->setToolTip(hintFor(mode));
    }
}

QString MainWindow::titleFor(CoreMode mode) const {
    switch (mode) {
    case CoreMode::Idle: return tr("Ready");
    case CoreMode::WakeWord: return tr("Wake word on");
    default: return coreModeName(mode);
    }
}

QString MainWindow::hintFor(CoreMode mode) const {
    const VoicePipeline *voice = controller_->voice();
    const bool canListen = voice && voice->canListen();
    switch (mode) {
    case CoreMode::Idle:
        if (!appHint_.isEmpty()) return appHint_ + (controller_->isScreenShared() ? tr(" Your screen is shared.") : QString());
        return (canListen ? tr("Type a request, or hold Talk and speak.") : tr("Type a request."))
               + (controller_->isScreenShared() ? tr(" Your screen is shared.") : QString());
    case CoreMode::WakeWord:
        // An overloaded microphone hears nothing as speech: say that instead of inviting the name.
        if (const QString trouble = voice ? voice->microphoneTrouble() : QString(); !trouble.isEmpty()) {
            return trouble.section(u". "_s, 0, 0) + u'.';
        }
        return tr("Say “Hey %1” and your request, type one, or hold Talk.").arg(controller_->config().voice.wakeName);
    case CoreMode::Listening:
        return tr("Speak now. Let go of Talk, or pause, to send.");
    case CoreMode::Transcribing:
        return voice ? tr("Recognising what you said with %1…").arg(voice->recognizerName())
                     : tr("Recognising what you said…");
    case CoreMode::Thinking:
        return tr("%1 is writing a reply…").arg(controller_->currentModel());
    case CoreMode::Acting:
        return currentTool_.isEmpty() ? tr("Running a tool…") : tr("Running %1…").arg(currentTool_);
    case CoreMode::Speaking:
        return tr("Reading the reply aloud. Press Talk or Esc to cut it.");
    case CoreMode::FastPath:
        return currentTool_.isEmpty() ? tr("Done directly, without the model.")
                                      : tr("%1, directly — no model.").arg(currentTool_);
    case CoreMode::Offline:
        return controller_->offlineReason().isEmpty() ? tr("The provider cannot be reached.")
                                                      : controller_->offlineReason();
    }
    return {};
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
    if (watched == statusLabel_ && event->type() == QEvent::Resize) {
        statusLabel_->setText(statusLabel_->fontMetrics().elidedText(statusMessage_, Qt::ElideRight,
                                                                     statusLabel_->width() - 4));
        return false;
    }
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

void MainWindow::toggleTheme() {
    // The same family in the other scheme: Ocean · Dark becomes Ocean · Light.
    if (const std::optional<ThemePack> sibling = NexaThemes::find(NexaThemes::siblingId(pack_))) {
        applyPack(*sibling);
        controller_->setTheme(NexaThemes::settingFor(pack_));
    }
}

void MainWindow::applyPack(const ThemePack &pack) {
    if (pack.id == pack_.id) return;
    pack_ = pack;
    NexaTheme::apply(qApp, pack_.tokens, pack_.style, design_);
    refreshTheme();
}

void MainWindow::refreshTheme() {
    const ThemeTokens &tokens = pack_.tokens;
    for (const auto &[action, kind] : themedActions_) action->setIcon(NexaIcons::icon(kind, tokens.text));
    themeAction_->setIcon(NexaIcons::icon(pack_.scheme == ThemeScheme::Dark ? NexaIcons::Kind::Sun : NexaIcons::Kind::Moon,
                                          tokens.text));
    themeAction_->setToolTip(tr("%1 — switch to %2").arg(pack_.name, pack_.scheme == ThemeScheme::Dark ? tr("light") : tr("dark")));
    // The stylesheet has set the core's colours by now; the title takes the state's.
    showCoreMode(controller_->coreMode());
}

void MainWindow::openSettings(const QString &tab) {
    SettingsDialog dialog(controller_->config(), registry_, http_, status_, this);
    if (!tab.isEmpty()) dialog.showTab(tab);
    if (dialog.exec() == QDialog::Accepted) controller_->updateConfig(dialog.config());
}

void MainWindow::wear(const QString &palette, const QString &design) {
    if (!design.isEmpty()) applyDesign(NexaDesigns::resolve(design));
    const ThemePack pack = NexaThemes::resolve(palette.isEmpty() ? NexaThemes::settingFor(pack_) : palette);
    pack_ = pack;
    NexaTheme::apply(qApp, pack_.tokens, pack_.style, design_);
    refreshTheme();
}

void MainWindow::snapshotSettings(const QString &directory, std::function<void()> done) {
    auto *dialog = new SettingsDialog(controller_->config(), registry_, http_, status_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    QTimer::singleShot(600, dialog, [dialog, directory, done = std::move(done)] {
        dialog->snapshotTabs(directory);
        dialog->close();
        if (done) done();
    });
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

void MainWindow::showSignInRequired(const QString &providerName) {
    // Not an API key prompt: a program signs in through its own account. The chat has the sentence already (the
    // controller's note or the failed reply); the status bar gets the way to the button.
    showStatus(tr("%1 is not signed in — Settings → Providers → Sign in.").arg(providerName), true);
    signInButton_->show();
}

void MainWindow::showModels(const QStringList &models, const QString &current) {
    const QSignalBlocker blocker(modelCombo_);
    modelCombo_->clear();
    // Models known to take images wear an eye, so a screenshot can go to a fast one that sees.
    const QStringList seeing = controller_->imageModels();
    const QIcon eye = NexaIcons::icon(NexaIcons::Kind::Eye, palette().color(QPalette::Text), 16);
    for (const QString &model : models) {
        if (seeing.contains(model)) {
            modelCombo_->addItem(eye, model);
            modelCombo_->setItemData(modelCombo_->count() - 1, tr("Takes images"), Qt::ToolTipRole);
        } else {
            modelCombo_->addItem(model);
        }
    }
    const int index = modelCombo_->findText(current);
    if (index >= 0) {
        modelCombo_->setCurrentIndex(index);
    } else {
        modelCombo_->setEditText(current);
    }
}

void MainWindow::showStatus(const QString &message, bool isError) {
    statusMessage_ = message;
    statusLabel_->setText(statusLabel_->fontMetrics().elidedText(message, Qt::ElideRight, statusLabel_->width() - 4));
    statusLabel_->setToolTip(message);
    statusLabel_->setProperty("error", isError);
    repolish(statusLabel_);
}

void MainWindow::showMicrophoneTrouble(const QString &notice) {
    showStatus(notice, true);
    lowerMicButton_->show();
    stateHint_->setText(hintFor(controller_->coreMode()));
}

void MainWindow::showPolicy() {
    policyButton_->setText(tr("Policy: %1").arg(policyWords(controller_->config().policyMode)));
}

void MainWindow::showVoiceEngines() {
    const VoicePipeline *voice = controller_->voice();
    if (!voice || !sttLabel_) return;
    const QString provider = controller_->providerName();
    sttLabel_->setText(tr("STT: %1").arg(voice->canListen() ? shortEngineNames(voice->recognizerName(), provider)
                                                             : tr("off")));
    sttLabel_->setToolTip(tr("Speech recognition, in the order tried (Settings → Voice): %1").arg(voice->recognizerName()));
    ttsLabel_->setText(tr("TTS: %1").arg(voice->canSpeak() ? shortEngineNames(voice->speechName(), provider)
                                                            : tr("off")));
    ttsLabel_->setToolTip(tr("The voice replies are read with, in the order tried (Settings → Voice): %1")
                              .arg(voice->speechName()));
}

void MainWindow::showHelper() {
    const bool installed = HelperClient::isInstalled();
    helperLabel_->setText(installed ? tr("Helper: installed") : tr("Helper: not installed"));
    QStringList notes;
    if (status_.inputSummary) notes.append(status_.inputSummary());
    if (status_.inputNotes) notes.append(status_.inputNotes());
    if (!installed) notes.append(tr("Root commands and typing need it: %1").arg(HelperClient::installCommand()));
    helperLabel_->setToolTip(notes.join(u'\n'));
}

void MainWindow::showPhone(const QString &description) {
    if (!phoneLabel_) return;
    if (description.isEmpty()) {
        const bool remembered = !controller_->config().android.serial.isEmpty();
        phoneLabel_->setText(remembered ? tr("Phone: not connected") : tr("Phone: not paired"));
        phoneLabel_->setToolTip(tr("No phone is connected. Settings → Android shows how to connect one."));
        return;
    }
    phoneLabel_->setText(tr("Phone: %1").arg(description));
    phoneLabel_->setToolTip(tr("The phone Nexa's android_* tools act on."));
}

void MainWindow::showRemote() {
    if (!remoteLabel_) return;
    RemoteServer *remote = status_.remote;
    if (!remote->isRunning()) {
        remoteLabel_->setText(tr("Remote: off"));
        remoteLabel_->setToolTip(tr("Settings → Remote switches the web remote on, so a phone can talk to Nexa."));
        return;
    }
    const int clients = remote->clientCount();
    remoteLabel_->setText(clients == 0 ? tr("Remote: waiting") : tr("Remote: %n phone(s)", nullptr, clients));
    remoteLabel_->setToolTip(tr("The web remote is on: %1").arg(remote->lanUrl().isEmpty() ? remote->localUrl()
                                                                                             : remote->lanUrl()));
}

// The Phone panel comes with the phone and goes with it. Shown, it takes its width from the
// Actions panel when the two share the right side — sized once it is in view, or the split
// halves the Actions panel.
void MainWindow::showPhonePanel(bool connected) {
    phonePanel_->setVisible(connected);
    if (!connected) return;
    if (actionLog_->isVisible() && dockWidgetArea(actionLog_) == Qt::RightDockWidgetArea) {
        resizeDocks({actionLog_, phonePanel_}, {320, 280}, Qt::Horizontal);
    } else {
        resizeDocks({phonePanel_}, {280}, Qt::Horizontal);
    }
}

// Where the user is, as far as Nexa knows: the app it opened last for them.
void MainWindow::showAppContext(const QString &name, const QString &hint) {
    appHint_ = hint;
    appLabel_->setVisible(!name.isEmpty());
    appLabel_->setText(tr("In: %1").arg(name));
    appLabel_->setToolTip(hint.isEmpty() ? QString()
                                         : hint + u'\n' + tr("Opening another app, or closing this one, moves on."));
    showCoreMode(controller_->coreMode());
}

void MainWindow::showBrowser() {
    browserLabel_->setText(tr("Browser: %1").arg(status_.browser->summary()));
    browserLabel_->setToolTip(status_.browser->detail());
}

void MainWindow::showScreenSharing(bool shared) {
    {
        const QSignalBlocker blocker(shareScreenAction_);
        shareScreenAction_->setChecked(shared);
    }
    screenLabel_->setText(shared ? tr("Screen: shared") : tr("Screen: not shared"));
    screenLabel_->setToolTip(shared ? tr("Nexa may take screenshots to see what you see. Share screen (Ctrl+Shift+S) "
                                         "switches it off.")
                                    : tr("Nexa works through commands and tools without looking at your screen. Share "
                                         "screen (Ctrl+Shift+S) lets it look."));
    showCoreMode(controller_->coreMode());  // the hint beside the core says it too
}

void MainWindow::setBusy(bool busy) {
    sendButton_->setText(busy ? tr("Stop") : tr("Send"));
    sendButton_->setProperty("busy", busy);
    repolish(sendButton_);
    modelCombo_->setEnabled(!busy);
    if (!busy) input_->setFocus();
}

} // namespace nexa
