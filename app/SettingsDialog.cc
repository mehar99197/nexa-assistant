#include "app/SettingsDialog.h"

#include "core/ai/IAiProvider.h"
#include "app/GlobalShortcuts.h"
#include "remote/PairingManager.h"
#include "remote/QrCode.h"
#include "remote/RemoteServer.h"
#include "system/android/AdbDiscovery.h"
#include "system/android/AndroidDevice.h"
#include "system/helper/HelperClient.h"
#include "voice/VoiceBackends.h"
#include "voice/tts/ITextToSpeech.h"
#include "voice/tts/SpeechdTtsBackend.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QSignalBlocker>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

void selectData(QComboBox *combo, const QString &value) {
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QStringList installedWhisperModels() {
    QStringList models;
    const QDir dir(VoiceBackends::dataDir() + u"/models"_s);
    for (const QString &file : dir.entryList({u"ggml-*.bin"_s}, QDir::Files, QDir::Name)) {
        models.append(file.mid(5, file.size() - 9));    // ggml-<name>.bin
    }
    return models;
}

} // namespace

SettingsDialog::SettingsDialog(Config config, ProviderRegistry *registry, HttpClient *http, SystemStatus status,
                               QWidget *parent)
    : QDialog(parent),
      config_(std::move(config)),
      registry_(registry),
      http_(http),
      status_(std::move(status)) {
    setWindowTitle(tr("Settings"));
    resize(820, 580);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildProvidersTab(), tr("Providers"));
    tabs->addTab(buildVoiceTab(), tr("Voice"));
    tabs->addTab(buildHelperTab(), tr("Helper"));
    tabs->addTab(buildAndroidTab(), tr("Android"));
    tabs->addTab(buildRemoteTab(), tr("Remote"));
    tabs->addTab(buildPolicyTab(), tr("Policy"));
    tabs->addTab(buildGeneralTab(), tr("General"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    populateProviders();
}

SettingsDialog::~SettingsDialog() {
    // Discovery polls often only while someone is looking at the result.
    if (status_.discovery) status_.discovery->setWatched(false);
    // A pairing code lives only as long as it is on screen.
    if (status_.pairing) status_.pairing->stopPairing();
}

void SettingsDialog::accept() {
    storeProvider();
    config_.activeProvider = activeCombo_->currentData().toString();
    config_.systemPrompt = systemPromptEdit_->toPlainText().trimmed();
    config_.policyMode = policyCombo_->currentData().toString();
    storeVoice();
    config_.android.autoReconnect = phoneReconnect_->isChecked();
    config_.remote.enabled = remoteEnabled_->isChecked();
    config_.remote.port = remotePort_->value();
    QDialog::accept();
}

QWidget *SettingsDialog::buildProvidersTab() {
    auto *page = new QWidget(this);

    activeCombo_ = new QComboBox(page);
    auto *activeRow = new QFormLayout;
    activeRow->addRow(tr("Active provider:"), activeCombo_);

    providerList_ = new QListWidget(page);
    providerList_->setMinimumWidth(210);
    connect(providerList_, &QListWidget::currentRowChanged, this, &SettingsDialog::showProvider);

    auto *addButton = new QToolButton(page);
    addButton->setText(tr("&Add"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *presetMenu = new QMenu(addButton);
    for (const ProviderPreset &preset : registry_->availablePresets()) {
        presetMenu->addAction(preset.name, this, [this, preset] { addProvider(preset); });
    }
    addButton->setMenu(presetMenu);
    auto *removeButton = new QPushButton(tr("&Remove"), page);
    connect(removeButton, &QPushButton::clicked, this, &SettingsDialog::removeProvider);

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(addButton);
    listButtons->addWidget(removeButton);
    listButtons->addStretch();

    auto *left = new QVBoxLayout;
    left->addWidget(providerList_, 1);
    left->addLayout(listButtons);

    providerForm_ = new QWidget(page);
    kindLabel_ = new QLabel(providerForm_);
    nameEdit_ = new QLineEdit(providerForm_);
    baseUrlEdit_ = new QLineEdit(providerForm_);
    baseUrlEdit_->setPlaceholderText(u"http://localhost:20128/v1"_s);
    apiKeyEdit_ = new QLineEdit(providerForm_);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setPlaceholderText(tr("not set"));
    showKeyCheck_ = new QCheckBox(tr("&Show"), providerForm_);
    connect(showKeyCheck_, &QCheckBox::toggled, this, [this](bool show) {
        apiKeyEdit_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(apiKeyEdit_, 1);
    keyRow->addWidget(showKeyCheck_);
    modelEdit_ = new QLineEdit(providerForm_);
    modelEdit_->setPlaceholderText(tr("pick it from the model list in the toolbar"));
    sttModelEdit_ = new QLineEdit(providerForm_);
    sttModelEdit_->setPlaceholderText(tr("none — e.g. whisper-1, or an audio-capable chat model"));
    sttModelEdit_->setToolTip(tr("Transcription models (whisper…) use /audio/transcriptions; any other model gets "
                                 "the recording as chat input."));
    ttsModelEdit_ = new QLineEdit(providerForm_);
    ttsModelEdit_->setPlaceholderText(tr("none — e.g. tts-1"));
    ttsVoiceEdit_ = new QLineEdit(providerForm_);
    ttsVoiceEdit_->setPlaceholderText(tr("e.g. alloy (a language code such as en for gtts/default)"));
    textToolsCheck_ = new QCheckBox(tr("This model cannot call tools — describe them in the prompt instead"),
                                    providerForm_);
    textToolsCheck_->setToolTip(tr("For models without function calling, such as many small local ones: Nexa "
                                   "lists the tools in the system prompt and reads the calls out of the reply."));
    testButton_ = new QPushButton(tr("&Test connection"), providerForm_);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testConnection);
    testResult_ = new QLabel(providerForm_);
    testResult_->setWordWrap(true);

    auto *form = new QFormLayout(providerForm_);
    form->addRow(tr("Type:"), kindLabel_);
    form->addRow(tr("Name:"), nameEdit_);
    form->addRow(tr("Base URL:"), baseUrlEdit_);
    form->addRow(tr("API key:"), keyRow);
    form->addRow(tr("Model:"), modelEdit_);
    form->addRow(tr("Speech recognition:"), sttModelEdit_);
    form->addRow(tr("Speech output:"), ttsModelEdit_);
    form->addRow(tr("Voice:"), ttsVoiceEdit_);
    form->addRow(QString(), textToolsCheck_);
    form->addRow(testButton_, testResult_);

    connect(nameEdit_, &QLineEdit::textEdited, this, [this](const QString &name) {
        if (QListWidgetItem *item = providerList_->currentItem()) item->setText(name);
        storeProvider();
        refreshActiveCombo();
    });

    auto *columns = new QHBoxLayout;
    columns->addLayout(left);
    columns->addWidget(providerForm_, 1);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(activeRow);
    layout->addLayout(columns, 1);
    return page;
}

QWidget *SettingsDialog::buildVoiceTab() {
    auto *page = new QWidget(this);
    const VoiceConfig &voice = config_.voice;

    sttCombo_ = new QComboBox(page);
    sttCombo_->addItem(tr("Automatic — the provider's model, local whisper.cpp if it fails"), u"auto"_s);
    sttCombo_->addItem(tr("Cloud only — the active provider's speech recognition model"), u"cloud"_s);
    sttCombo_->addItem(tr("Local only — whisper.cpp, offline"), u"whisper"_s);
    selectData(sttCombo_, voice.sttBackend);
    whisperCombo_ = new QComboBox(page);
    whisperCombo_->setEditable(true);
    QStringList models = installedWhisperModels();
    for (const QString &known : {u"tiny.en"_s, u"base.en"_s, u"small.en"_s}) {
        if (!models.contains(known)) models.append(known);
    }
    whisperCombo_->addItems(models);
    whisperCombo_->setCurrentText(voice.whisperModel);
    connect(whisperCombo_, &QComboBox::currentTextChanged, this, &SettingsDialog::updateVoiceStatus);
    whisperStatus_ = new QLabel(page);
    whisperStatus_->setWordWrap(true);
    whisperStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    languageEdit_ = new QLineEdit(voice.language, page);
    languageEdit_->setPlaceholderText(tr("en, de, ur, … or auto"));

    ttsCombo_ = new QComboBox(page);
    ttsCombo_->addItem(tr("Automatic — the provider's voice, Piper if it fails"), u"auto"_s);
    ttsCombo_->addItem(tr("Cloud only — the active provider's speech model"), u"cloud"_s);
    ttsCombo_->addItem(tr("Piper — local neural voice"), u"piper"_s);
    ttsCombo_->addItem(tr("Speech Dispatcher — local, robotic"), u"speechd"_s);
    ttsCombo_->addItem(tr("Off — never speak"), u"off"_s);
    selectData(ttsCombo_, voice.ttsBackend);
    piperCombo_ = new QComboBox(page);
    piperCombo_->addItems(VoiceBackends::piperVoices());
    if (!voice.piperVoice.isEmpty()) piperCombo_->setCurrentText(voice.piperVoice);
    speechdCombo_ = new QComboBox(page);
    speechdCombo_->addItem(tr("default"), QString());
    for (const QString &name : SpeechdTtsBackend::voiceNames()) speechdCombo_->addItem(name, name);
    selectData(speechdCombo_, voice.speechdVoice);
    rateSlider_ = new QSlider(Qt::Horizontal, page);
    rateSlider_->setRange(-100, 100);
    rateSlider_->setValue(int(voice.ttsRate * 100));
    rateSlider_->setToolTip(tr("Speaking rate"));
    speakCombo_ = new QComboBox(page);
    speakCombo_->addItem(tr("Replies to spoken requests"), u"voice"_s);
    speakCombo_->addItem(tr("Every reply"), u"always"_s);
    speakCombo_->addItem(tr("Never"), u"never"_s);
    selectData(speakCombo_, voice.speakReplies);
    auto *testVoiceButton = new QPushButton(tr("Test &voice"), page);
    connect(testVoiceButton, &QPushButton::clicked, this, &SettingsDialog::testVoice);
    voiceTestResult_ = new QLabel(page);
    voiceTestResult_->setWordWrap(true);

    wakeCheck_ = new QCheckBox(tr("Listen for “Hey Nexa” — the microphone stays on; only local recognition hears it"),
                               page);
    wakeCheck_->setChecked(voice.wakeWord);
    bargeInCheck_ = new QCheckBox(tr("Stop speaking when I start talking (use headphones, or Nexa hears itself)"), page);
    bargeInCheck_->setChecked(voice.bargeIn);

    auto *listening = new QFormLayout;
    listening->addRow(tr("Speech recognition:"), sttCombo_);
    listening->addRow(tr("Local model:"), whisperCombo_);
    listening->addRow(QString(), whisperStatus_);
    listening->addRow(tr("Language:"), languageEdit_);
    auto *speaking = new QFormLayout;
    speaking->addRow(tr("Speech output:"), ttsCombo_);
    speaking->addRow(tr("Piper voice:"), piperCombo_);
    speaking->addRow(tr("Speech Dispatcher voice:"), speechdCombo_);
    speaking->addRow(tr("Rate:"), rateSlider_);
    speaking->addRow(tr("Speak:"), speakCombo_);
    speaking->addRow(testVoiceButton, voiceTestResult_);

    auto *note = new QLabel(tr("Talk: click the Talk button (the request ends when you pause), hold it to talk, or "
                               "press Ctrl+M. Esc stops the speech. Cloud models are set per provider on the "
                               "Providers tab."),
                            page);
    note->setWordWrap(true);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(listening);
    layout->addSpacing(8);
    layout->addLayout(speaking);
    layout->addSpacing(8);
    layout->addWidget(wakeCheck_);
    layout->addWidget(bargeInCheck_);
    layout->addStretch();
    layout->addWidget(note);
    updateVoiceStatus();
    return page;
}

void SettingsDialog::updateVoiceStatus() {
    if (!VoiceBackends::whisperBuilt()) {
        whisperStatus_->setText(tr("This build has no local recognition (configure with -DNEXA_BUILD_WHISPER=ON)."));
        wakeCheck_->setEnabled(false);
        return;
    }
    const QString model = whisperCombo_->currentText().trimmed();
    if (QFileInfo::exists(VoiceBackends::whisperModelPath(model))) {
        whisperStatus_->setText(tr("Installed."));
    } else {
        whisperStatus_->setText(tr("Not installed — run: scripts/download-whisper-model.sh %1").arg(model));
    }
}

void SettingsDialog::storeVoice() {
    VoiceConfig &voice = config_.voice;
    voice.sttBackend = sttCombo_->currentData().toString();
    voice.whisperModel = whisperCombo_->currentText().trimmed().isEmpty() ? VoiceConfig{}.whisperModel
                                                                         : whisperCombo_->currentText().trimmed();
    voice.language = languageEdit_->text().trimmed().isEmpty() ? u"auto"_s : languageEdit_->text().trimmed();
    voice.ttsBackend = ttsCombo_->currentData().toString();
    voice.piperVoice = piperCombo_->currentText();
    voice.speechdVoice = speechdCombo_->currentData().toString();
    voice.ttsRate = rateSlider_->value() / 100.0;
    voice.speakReplies = speakCombo_->currentData().toString();
    voice.wakeWord = wakeCheck_->isChecked();
    voice.bargeIn = bargeInCheck_->isChecked();
}

void SettingsDialog::testVoice() {
    storeProvider();
    storeVoice();
    if (testSpeech_) testSpeech_->stop();
    const ProviderConfig *active = config_.provider(activeCombo_->currentData().toString());
    Result<std::unique_ptr<ITextToSpeech>> speech = VoiceBackends::createTextToSpeech(config_.voice, active, http_);
    if (!speech || !speech.value()) {
        voiceTestResult_->setText(speech ? tr("Speech output is off.") : speech.error().message);
        return;
    }
    testSpeech_ = std::move(speech).value();
    connect(testSpeech_.get(), &ITextToSpeech::failed, this, [this](const Error &error) {
        voiceTestResult_->setText(error.message);
    });
    connect(testSpeech_.get(), &ITextToSpeech::finished, this, [this] { voiceTestResult_->setText(tr("Done.")); });
    voiceTestResult_->setText(tr("Speaking with %1…").arg(testSpeech_->name()));
    testSpeech_->speak(tr("Hello! This is how Nexa sounds."));
}

// Settings → Helper: whether the root-privileged part is installed, what it is needed for, and
// how to add or remove it. Also why typing and clicking do or do not work, which is otherwise
// invisible — on Wayland without the helper they silently go nowhere.
QWidget *SettingsDialog::buildHelperTab() {
    auto *page = new QWidget(this);

    helperState_ = new QLabel(page);
    helperState_->setWordWrap(true);
    helperState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inputState_ = new QLabel(page);
    inputState_->setWordWrap(true);
    inputState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    authState_ = new QLabel(page);
    authState_->setWordWrap(true);
    authState_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    helperTestButton_ = new QPushButton(tr("&Test the helper"), page);
    helperTestButton_->setToolTip(tr("Starts the helper and asks for its version. This needs no password."));
    connect(helperTestButton_, &QPushButton::clicked, this, &SettingsDialog::testHelper);

    helperForgetButton_ = new QPushButton(tr("&Forget my password"), page);
    helperForgetButton_->setToolTip(tr("Nexa asks for your password again the next time it needs the helper."));
    connect(helperForgetButton_, &QPushButton::clicked, this, &SettingsDialog::forgetAuthorization);

    auto *explanation = new QLabel(
        tr("The helper is a small program that runs as root, started only when it is needed and stopping again "
           "after ten idle minutes. Nexa uses it for two things: running commands as an administrator, and "
           "typing and clicking for you (on Wayland nothing else can reach other windows).\n\n"
           "The first of those actions in a login asks polkit, so you type your own password once — into the "
           "desktop's own prompt, never into Nexa. All that is then remembered, until you log out or press "
           "Forget, is that you said yes. Every action is written to the Actions panel, to "
           "~/.local/share/nexa/audit.jsonl and to the system journal. The helper cannot read your keyboard or "
           "your screen: it only sends events.\n\n"
           "Install it with:  sudo ./packaging/install-helper.sh\n"
           "Remove it with:   sudo ./packaging/uninstall-helper.sh\n"
           "See what it did:  journalctl -u nexa-helper"),
        page);
    explanation->setWordWrap(true);
    explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *form = new QFormLayout;
    form->addRow(tr("Root helper:"), helperState_);
    form->addRow(tr("Typing and clicking:"), inputState_);
    form->addRow(tr("Password:"), authState_);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(helperTestButton_);
    buttons->addWidget(helperForgetButton_);
    buttons->addStretch();

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addSpacing(8);
    layout->addWidget(explanation);
    layout->addStretch();
    showHelperStatus();
    return page;
}

void SettingsDialog::showHelperStatus() {
    if (HelperClient::isInstalled()) {
        helperState_->setText(tr("Installed."));
        helperState_->setStyleSheet(u"color: #2bb3a0;"_s);
    } else {
        helperState_->setText(tr("Not installed — Nexa cannot run commands as root, and on Wayland it cannot "
                                 "type or click."));
        helperState_->setStyleSheet(u"color: #e09b2d;"_s);
    }
    QStringList lines;
    if (status_.inputSummary) lines.append(status_.inputSummary());
    if (status_.inputNotes) lines += status_.inputNotes();
    inputState_->setText(lines.isEmpty() ? tr("unknown") : lines.join(u'\n'));
    showAuthorizationState();
}

// Whether the password has already been given for this login. Only asked of a helper that is
// already running: opening this tab is no reason to start root-privileged code.
void SettingsDialog::showAuthorizationState() {
    const bool installed = HelperClient::isInstalled();
    helperForgetButton_->setEnabled(installed && status_.helper != nullptr);
    if (!installed) {
        authState_->setText(tr("—"));
        return;
    }
    if (status_.helper == nullptr || !status_.helper->isReady()) {
        authState_->setText(tr("Asked for once per login, by the desktop's own prompt."));
        return;
    }
    // Every answer below can arrive after the dialog is gone.
    const QPointer<SettingsDialog> self(this);
    status_.helper->isAuthorized([self](Result<bool> authorized) {
        if (!self) return;
        if (!authorized) {
            self->authState_->setText(authorized.error().message);
            return;
        }
        self->authState_->setText(authorized.value()
                                      ? tr("Given for this login — nothing will ask again until you log out.")
                                      : tr("Asked for once, the next time Nexa needs the helper."));
    });
}

void SettingsDialog::testHelper() {
    if (!status_.helper) return;
    helperTestButton_->setEnabled(false);
    helperState_->setStyleSheet({});
    helperState_->setText(tr("Starting the helper…"));
    const QPointer<SettingsDialog> self(this);
    status_.helper->probe([self](Result<QString> version) {
        if (!self) return;
        self->helperTestButton_->setEnabled(true);
        if (version) {
            self->helperState_->setText(tr("Working — helper version %1.").arg(version.value()));
            self->helperState_->setStyleSheet(u"color: #2bb3a0;"_s);
        } else {
            self->helperState_->setText(version.error().message);
            self->helperState_->setStyleSheet(u"color: #e5534b;"_s);
        }
        if (self->status_.inputSummary) self->showHelperStatus();
    });
}

void SettingsDialog::forgetAuthorization() {
    if (!status_.helper) return;
    helperForgetButton_->setEnabled(false);
    const QPointer<SettingsDialog> self(this);
    status_.helper->forgetAuthorization([self](Result<void> done) {
        if (!self) return;
        self->helperForgetButton_->setEnabled(true);
        if (!done) {
            self->authState_->setText(done.error().message);
            return;
        }
        self->authState_->setText(tr("Forgotten — the next action asks for your password again."));
    });
}

// Settings → Android: which phone Nexa uses, and how to connect one. The steps follow the phone's
// own wording (Developer options → Wireless debugging → Pair device with pairing code), and what
// the phone advertises on the network fills the addresses in, so nothing has to be copied by hand.
QWidget *SettingsDialog::buildAndroidTab() {
    auto *page = new QWidget(this);

    phoneState_ = new QLabel(page);
    phoneState_->setWordWrap(true);
    phoneState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *refreshButton = new QPushButton(tr("&Refresh"), page);
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        if (status_.discovery) status_.discovery->refresh();
        showPhone();
    });

    phoneRemembered_ = new QLabel(page);
    phoneRemembered_->setWordWrap(true);
    phoneRememberButton_ = new QPushButton(page);
    connect(phoneRememberButton_, &QPushButton::clicked, this, &SettingsDialog::rememberPhone);
    phoneReconnect_ = new QCheckBox(tr("Reconnect to it by itself whenever it offers wireless debugging"), page);
    phoneReconnect_->setChecked(config_.android.autoReconnect);

    phoneList_ = new QListWidget(page);
    phoneList_->setMinimumHeight(110);
    // Picking what the phone advertises fills in the matching address.
    connect(phoneList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item) return;
        const QString address = item->data(Qt::UserRole).toString();
        if (address.isEmpty()) return;
        if (item->data(Qt::UserRole + 1).toInt() == int(MdnsService::Kind::Pairing)) {
            pairAddress_->setText(address);
            pairCode_->setFocus();
        } else {
            connectAddress_->setText(address);
        }
    });

    pairAddress_ = new QLineEdit(page);
    pairAddress_->setPlaceholderText(tr("192.168.1.20:41247 — under \"Pair device with pairing code\""));
    pairCode_ = new QLineEdit(page);
    pairCode_->setPlaceholderText(tr("6-digit code"));
    pairCode_->setMaxLength(6);
    auto *pairButton = new QPushButton(tr("&Pair"), page);
    connect(pairButton, &QPushButton::clicked, this, &SettingsDialog::pairPhone);

    connectAddress_ = new QLineEdit(page);
    connectAddress_->setPlaceholderText(tr("192.168.1.20:37135 — at the top of Wireless debugging"));
    auto *connectButton = new QPushButton(tr("&Connect"), page);
    connect(connectButton, &QPushButton::clicked, this, &SettingsDialog::connectPhone);
    auto *disconnectButton = new QPushButton(tr("&Disconnect"), page);
    disconnectButton->setToolTip(tr("Disconnects every phone connected over Wi-Fi"));
    connect(disconnectButton, &QPushButton::clicked, this, &SettingsDialog::disconnectPhones);

    phoneResult_ = new QLabel(page);
    phoneResult_->setWordWrap(true);
    phoneResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *help = new QLabel(
        tr("On the phone: Settings → About phone → tap Build number seven times to unlock Developer options. "
           "Then Developer options → Wireless debugging → turn it on → \"Pair device with pairing code\". The phone "
           "and this PC must be on the same Wi-Fi. Pick the phone in the list above (or type the address it shows), "
           "enter the code, press Pair, then Connect.\n\n"
           "Over USB nothing needs pairing: turn on USB debugging and tap Allow on the phone.\n\n"
           "Nexa can then open apps, tap, type, take screenshots, read notifications and texts, start calls and "
           "messages, and copy files. Every action goes to the Actions panel and the audit log."),
        page);
    help->setWordWrap(true);

    auto *stateRow = new QHBoxLayout;
    stateRow->addWidget(phoneState_, 1);
    stateRow->addWidget(refreshButton);
    auto *rememberRow = new QHBoxLayout;
    rememberRow->addWidget(phoneRemembered_, 1);
    rememberRow->addWidget(phoneRememberButton_);
    auto *pairRow = new QHBoxLayout;
    pairRow->addWidget(pairAddress_, 2);
    pairRow->addWidget(pairCode_, 1);
    pairRow->addWidget(pairButton);
    auto *connectRow = new QHBoxLayout;
    connectRow->addWidget(connectAddress_, 1);
    connectRow->addWidget(connectButton);
    connectRow->addWidget(disconnectButton);

    auto *form = new QFormLayout;
    form->addRow(tr("Phone:"), stateRow);
    form->addRow(tr("Remembered:"), rememberRow);
    form->addRow(QString(), phoneReconnect_);
    form->addRow(tr("Seen by adb:"), phoneList_);
    form->addRow(tr("Pair:"), pairRow);
    form->addRow(tr("Connect:"), connectRow);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(phoneResult_);
    layout->addSpacing(8);
    layout->addWidget(help);
    layout->addStretch();

    if (status_.discovery) {
        connect(status_.discovery, &AdbDiscovery::changed, this, &SettingsDialog::showDiscovered);
        status_.discovery->setWatched(true);
    }
    if (status_.phone) connect(status_.phone, &AndroidDevice::deviceChanged, this, &SettingsDialog::showPhone);
    showDiscovered();
    showPhone();
    return page;
}

void SettingsDialog::showPhone() {
    const QString remembered = config_.android.serial;
    phoneRemembered_->setText(remembered.isEmpty()
                                  ? tr("None — Nexa uses whichever single phone is connected.")
                                  : tr("The phone with serial number %1.").arg(remembered));
    phoneRememberButton_->setText(remembered.isEmpty() ? tr("Re&member this phone") : tr("For&get it"));
    phoneState_->setStyleSheet({});
    if (!status_.phone) {
        phoneState_->setText(tr("unavailable"));
        return;
    }
    if (!AdbClient::isInstalled()) {
        phoneState_->setText(tr("adb is not installed. Install it with: sudo apt install adb"));
        phoneState_->setStyleSheet(u"color: #e09b2d;"_s);
        return;
    }
    phoneState_->setText(tr("Looking…"));
    // The answer can arrive after the dialog is gone.
    const QPointer<SettingsDialog> self(this);
    status_.phone->info([self](Result<AndroidDevice::Info> info) {
        if (!self) return;
        if (!info) {
            self->phoneState_->setText(info.error().message);
            self->phoneState_->setStyleSheet(u"color: #e09b2d;"_s);
            return;
        }
        self->phoneState_->setText(info.value().summary());
        self->phoneState_->setStyleSheet(u"color: #2bb3a0;"_s);
    });
}

void SettingsDialog::showDiscovered() {
    if (!status_.discovery) return;
    const QListWidgetItem *current = phoneList_->currentItem();
    const QString selected = current ? current->data(Qt::UserRole).toString() : QString();
    const QSignalBlocker blocker(phoneList_);       // rebuilding is not the user picking something
    phoneList_->clear();

    for (const AdbDevice &device : status_.discovery->devices()) {
        const QString how = !device.isReady()     ? device.state
                            : device.isWireless() ? tr("connected over Wi-Fi")
                                                  : tr("connected over USB");
        new QListWidgetItem(tr("%1 — %2 (%3)").arg(device.displayModel(), how, device.serial), phoneList_);
    }
    for (const MdnsService &service : status_.discovery->services()) {
        if (service.kind == MdnsService::Kind::Other) continue;
        const QString serial = adb::serialFromServiceName(service.name);
        const QString name = serial.isEmpty() ? service.name : serial;
        auto *item = new QListWidgetItem(service.kind == MdnsService::Kind::Pairing
                                             ? tr("%1 — ready to pair at %2").arg(name, service.address())
                                             : tr("%1 — wireless debugging at %2").arg(name, service.address()),
                                         phoneList_);
        item->setData(Qt::UserRole, service.address());
        item->setData(Qt::UserRole + 1, int(service.kind));
        if (service.address() == selected) phoneList_->setCurrentItem(item);
    }
    if (phoneList_->count() == 0) {
        auto *item = new QListWidgetItem(tr("Nothing yet — turn on Wireless debugging on the phone, or plug it in."),
                                         phoneList_);
        item->setFlags(Qt::NoItemFlags);
    }
}

void SettingsDialog::rememberPhone() {
    if (!config_.android.serial.isEmpty()) {
        config_.android.serial.clear();
        showPhoneResult(tr("Forgotten. Press OK to keep it that way."), true);
        showPhone();
        return;
    }
    if (!status_.phone) return;
    phoneRememberButton_->setEnabled(false);
    const QPointer<SettingsDialog> self(this);
    status_.phone->info([self](Result<AndroidDevice::Info> info) {
        if (!self) return;
        self->phoneRememberButton_->setEnabled(true);
        if (!info) {
            self->showPhoneResult(info.error().message, false);
            return;
        }
        if (info.value().hardwareSerial.isEmpty()) {
            self->showPhoneResult(tr("The phone did not say its serial number, so it cannot be remembered."), false);
            return;
        }
        self->config_.android.serial = info.value().hardwareSerial;
        self->showPhoneResult(tr("Remembered %1. Press OK to save it.").arg(info.value().model), true);
        self->showPhone();
    });
}

void SettingsDialog::pairPhone() {
    if (!status_.phone) return;
    const QString address = pairAddress_->text().trimmed();
    const QString code = pairCode_->text().trimmed();
    static const QRegularExpression sixDigits(u"^\\d{6}$"_s);
    if (address.isEmpty() || !sixDigits.match(code).hasMatch()) {
        showPhoneResult(tr("Enter the address and the six-digit code the phone shows under \"Pair device with "
                           "pairing code\"."),
                        false);
        return;
    }
    showPhoneResult(tr("Pairing…"), true);
    const QPointer<SettingsDialog> self(this);
    status_.phone->pair(address, code, [self](Result<QString> result) {
        if (!self) return;
        if (!result) {
            self->showPhoneResult(result.error().message, false);
            return;
        }
        self->pairCode_->clear();
        self->showPhoneResult(result.value() + u' '
                                  + tr("Now connect: pick its wireless debugging address in the list, or type the "
                                       "one at the top of the phone's Wireless debugging screen."),
                              true);
        if (self->status_.discovery) self->status_.discovery->refresh();
    });
}

void SettingsDialog::connectPhone() {
    if (!status_.phone) return;
    const QString address = connectAddress_->text().trimmed();
    if (address.isEmpty()) {
        showPhoneResult(tr("Enter the address at the top of the phone's Wireless debugging screen."), false);
        return;
    }
    showPhoneResult(tr("Connecting…"), true);
    const QPointer<SettingsDialog> self(this);
    status_.phone->connectTo(address, [self](Result<QString> result) {
        if (!self) return;
        if (!result) {
            self->showPhoneResult(result.error().message, false);
            return;
        }
        self->showPhoneResult(result.value(), true);
        if (self->status_.discovery) self->status_.discovery->refresh();
        self->showPhone();
    });
}

void SettingsDialog::disconnectPhones() {
    if (!status_.phone) return;
    const QPointer<SettingsDialog> self(this);
    status_.phone->disconnectAll([self](Result<QString> result) {
        if (!self) return;
        self->showPhoneResult(result ? result.value() : result.error().message, result.isOk());
        if (self->status_.discovery) self->status_.discovery->refresh();
        self->showPhone();
    });
}

void SettingsDialog::showPhoneResult(const QString &text, bool ok) {
    phoneResult_->setText(text);
    phoneResult_->setStyleSheet(ok ? u"color: #2bb3a0;"_s : u"color: #e5534b;"_s);
}

// Settings → Remote: the web remote, so the phone's browser can talk to Nexa. Off by default, and
// a phone gets in only by pairing with a code shown here.
QWidget *SettingsDialog::buildRemoteTab() {
    auto *page = new QWidget(this);

    remoteEnabled_ = new QCheckBox(tr("Let my phone use Nexa through its web browser"), page);
    remoteEnabled_->setChecked(config_.remote.enabled);
    remotePort_ = new QSpinBox(page);
    remotePort_->setRange(1024, 65534);
    remotePort_->setValue(config_.remote.port);
    remotePort_->setToolTip(tr("HTTPS on the network; the next port up serves the phone over adb"));

    remoteState_ = new QLabel(page);
    remoteState_->setWordWrap(true);
    remoteState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    remoteFingerprint_ = new QLabel(page);
    remoteFingerprint_->setWordWrap(true);
    remoteFingerprint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    remoteFingerprint_->setToolTip(tr("The phone's browser shows this certificate as untrusted, because Nexa made it "
                                      "itself. Its details there should show this same SHA-256 fingerprint."));

    pairButton_ = new QPushButton(tr("&Pair a phone"), page);
    connect(pairButton_, &QPushButton::clicked, this, &SettingsDialog::startPairing);
    qrLabel_ = new QLabel(page);
    qrLabel_->setAlignment(Qt::AlignCenter);
    pairCodeLabel_ = new QLabel(page);
    pairCodeLabel_->setWordWrap(true);
    pairCodeLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pairCountdown_ = new QTimer(this);
    pairCountdown_->setInterval(1000);
    connect(pairCountdown_, &QTimer::timeout, this, &SettingsDialog::showPairing);

    pairedList_ = new QListWidget(page);
    pairedList_->setMinimumHeight(90);
    revokeButton_ = new QPushButton(tr("Re&voke"), page);
    revokeButton_->setToolTip(tr("Shuts that phone out at once; it has to pair again to come back."));
    connect(revokeButton_, &QPushButton::clicked, this, [this] {
        const QListWidgetItem *item = pairedList_->currentItem();
        if (status_.pairing && item) status_.pairing->revoke(item->data(Qt::UserRole).toString());
    });

    auto *help = new QLabel(
        tr("With this on, Nexa runs a small web server. Pair a phone once and its browser can send requests — typed or "
           "spoken — and sees the whole conversation; everything still goes through the Actions panel, the audit log "
           "and the policy, exactly as if it had been typed here.\n\n"
           "On the same Wi-Fi the phone opens the https address. The first time, its browser warns that the "
           "connection is not private, because Nexa signed its own certificate: compare the fingerprint above with "
           "the one the browser shows, then continue. A phone connected with adb (Settings → Android) can open "
           "http://localhost:7777 instead, with no warning.\n\n"
           "If the phone cannot connect, the PC's firewall may be blocking the port."),
        page);
    help->setWordWrap(true);

    auto *enableRow = new QHBoxLayout;
    enableRow->addWidget(remoteEnabled_, 1);
    enableRow->addWidget(new QLabel(tr("Port:"), page));
    enableRow->addWidget(remotePort_);
    auto *pairRow = new QHBoxLayout;
    pairRow->addWidget(qrLabel_);
    pairRow->addWidget(pairCodeLabel_, 1);
    auto *pairedRow = new QHBoxLayout;
    pairedRow->addWidget(pairedList_, 1);
    pairedRow->addWidget(revokeButton_, 0, Qt::AlignTop);

    auto *form = new QFormLayout;
    form->addRow(tr("Status:"), remoteState_);
    form->addRow(tr("Certificate:"), remoteFingerprint_);
    form->addRow(tr("Paired phones:"), pairedRow);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(enableRow);
    layout->addLayout(form);
    layout->addWidget(pairButton_, 0, Qt::AlignLeft);
    layout->addLayout(pairRow);
    layout->addSpacing(8);
    layout->addWidget(help);
    layout->addStretch();

    if (status_.remote) {
        connect(status_.remote, &RemoteServer::runningChanged, this, &SettingsDialog::showRemote);
        connect(status_.remote, &RemoteServer::clientsChanged, this, &SettingsDialog::showRemote);
    }
    if (status_.pairing) {
        connect(status_.pairing, &PairingManager::devicesChanged, this, &SettingsDialog::showPairedDevices);
        connect(status_.pairing, &PairingManager::codeChanged, this, &SettingsDialog::showPairing);
    }
    showRemote();
    showPairedDevices();
    showPairing();
    return page;
}

void SettingsDialog::showRemote() {
    RemoteServer *remote = status_.remote;
    const bool running = remote && remote->isRunning();
    pairButton_->setEnabled(running && status_.pairing);
    remoteState_->setStyleSheet({});
    if (!running) {
        remoteState_->setText(config_.remote.enabled && remote && !remote->problem().isEmpty()
                                  ? remote->problem()
                                  : tr("Off. Tick the box above and press OK to start it."));
        remoteFingerprint_->setText(tr("—"));
        return;
    }
    QStringList lines;
    if (const QString lan = remote->lanUrl(); !lan.isEmpty()) lines.append(tr("On the Wi-Fi: %1").arg(lan));
    lines.append(tr("Over adb: %1").arg(remote->localUrl()));
    const int clients = remote->clientCount();
    lines.append(clients == 0 ? tr("No phone connected right now.") : tr("%n phone(s) connected.", nullptr, clients));
    if (!remote->problem().isEmpty()) lines.append(remote->problem());
    remoteState_->setText(lines.join(u'\n'));
    remoteState_->setStyleSheet(u"color: #2bb3a0;"_s);
    const QString fingerprint = remote->fingerprint();
    remoteFingerprint_->setText(fingerprint.isEmpty() ? tr("none — HTTPS is not running") : u"SHA-256 "_s + fingerprint);
}

void SettingsDialog::startPairing() {
    if (!status_.pairing) return;
    status_.pairing->startPairing();
    pairCountdown_->start();
}

void SettingsDialog::showPairing() {
    PairingManager *pairing = status_.pairing;
    const QString code = pairing ? pairing->code() : QString();
    if (code.isEmpty()) {
        pairCountdown_->stop();
        qrLabel_->clear();
        qrLabel_->hide();
        pairCodeLabel_->setText(pairing && !pairing->devices().isEmpty()
                                    ? QString()
                                    : tr("No phone is paired yet."));
        return;
    }
    if (!status_.remote) return;
    // The network address when HTTPS runs; otherwise the one a phone on adb opens.
    const QString base = !status_.remote->lanUrl().isEmpty() ? status_.remote->lanUrl() : status_.remote->localUrl();
    const QString link = base + u"/#pair="_s + code;
    if (qrLabel_->isHidden() || qrLabel_->property("link").toString() != link) {
        qrLabel_->setPixmap(QPixmap::fromImage(qrCode(link, 5)));
        qrLabel_->setProperty("link", link);
        qrLabel_->show();
    }
    const qint64 left = std::max<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(pairing->codeExpires()));
    pairCodeLabel_->setText(tr("Scan this with the phone's camera, or open %1 on the phone and enter\n\n"
                               "%2 %3\n\nThe code works once, for %4:%5 more.")
                                .arg(base, code.left(3), code.mid(3))
                                .arg(left / 60)
                                .arg(left % 60, 2, 10, u'0'));
}

void SettingsDialog::showPairedDevices() {
    pairedList_->clear();
    const QList<PairedDevice> devices = status_.pairing ? status_.pairing->devices() : QList<PairedDevice>{};
    for (const PairedDevice &device : devices) {
        auto *item = new QListWidgetItem(tr("%1 — paired %2, last seen %3")
                                             .arg(device.name,
                                                  device.paired.toLocalTime().toString(u"d MMM yyyy"_s),
                                                  device.lastSeen.toLocalTime().toString(u"d MMM HH:mm"_s)),
                                         pairedList_);
        item->setData(Qt::UserRole, device.id);
    }
    revokeButton_->setEnabled(!devices.isEmpty());
    if (devices.isEmpty()) {
        auto *item = new QListWidgetItem(tr("None"), pairedList_);
        item->setFlags(Qt::NoItemFlags);
    }
}

QWidget *SettingsDialog::buildPolicyTab() {
    auto *page = new QWidget(this);
    policyCombo_ = new QComboBox(page);
    policyCombo_->addItem(tr("Fully autonomous — never ask (default)"), u"autonomous"_s);
    policyCombo_->addItem(tr("Ask before root and destructive actions"), u"confirm-risky"_s);
    policyCombo_->addItem(tr("Ask before every action that changes something"), u"confirm-all"_s);
    const int index = policyCombo_->findData(config_.policyMode);
    policyCombo_->setCurrentIndex(index >= 0 ? index : 0);

    auto *explanation = new QLabel(
        tr("Every action is recorded in ~/.local/share/nexa/audit.jsonl and shown in the Actions panel. "
           "Destructive commands (rm -r, mkfs, dd of=, shutdown, git reset --hard, …) are flagged there in "
           "every mode.\n\nStop everything at any time with Esc (current reply) or Ctrl+Alt+Esc / "
           "Ctrl+Shift+Esc (panic: replies, tools and the processes they started)."),
        page);
    explanation->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("When Nexa acts:"), policyCombo_);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(explanation);
    layout->addStretch();
    return page;
}

QWidget *SettingsDialog::buildGeneralTab() {
    auto *page = new QWidget(this);
    systemPromptEdit_ = new QPlainTextEdit(page);
    systemPromptEdit_->setPlainText(config_.systemPrompt);
    auto *restore = new QPushButton(tr("Restore default"), page);
    connect(restore, &QPushButton::clicked, this, [this] { systemPromptEdit_->setPlainText(defaultSystemPrompt()); });

    // The keys the desktop hands Nexa. GNOME lets the user choose them, so they are shown, not set.
    QString hotkeys;
    if (status_.shortcuts && status_.shortcuts->isRegistered()) {
        const auto describe = [this](const QString &id) {
            const QString key = status_.shortcuts->trigger(id);
            return key.isEmpty() ? tr("no key yet") : key;
        };
        hotkeys = tr("Talk: %1 · Stop everything: %2").arg(describe(u"talk"_s), describe(u"panic"_s));
    } else {
        hotkeys = tr("not available — the desktop did not grant global hotkeys");
    }
    auto *hotkeyLabel = new QLabel(
        tr("Hotkeys that work from any window: %1.\nChange them in GNOME Settings → Keyboard → Keyboard Shortcuts "
           "→ Nexa Assistant. Inside Nexa: Ctrl+M talks, Esc stops the reply, Ctrl+Alt+Esc stops everything.")
            .arg(hotkeys),
        page);
    hotkeyLabel->setWordWrap(true);
    hotkeyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("System prompt (sent at the start of every conversation):"), page));
    layout->addWidget(systemPromptEdit_, 1);
    layout->addWidget(restore, 0, Qt::AlignRight);
    layout->addSpacing(8);
    layout->addWidget(hotkeyLabel);
    return page;
}

void SettingsDialog::populateProviders() {
    {
        const QSignalBlocker blocker(providerList_);
        providerList_->clear();
        for (const ProviderConfig &provider : config_.providers) providerList_->addItem(provider.name);
    }
    refreshActiveCombo();

    int activeRow = -1;
    if (const ProviderConfig *active = config_.active()) {
        for (int row = 0; row < config_.providers.size(); ++row) {
            if (config_.providers.at(row).id == active->id) activeRow = row;
        }
    }
    {
        const QSignalBlocker blocker(providerList_);
        providerList_->setCurrentRow(activeRow);
    }
    showProvider(activeRow);
}

void SettingsDialog::showProvider(int row) {
    storeProvider();  // keep the edits of the provider shown so far
    currentRow_ = row;
    testProvider_.reset();
    testButton_->setEnabled(true);
    testResult_->clear();

    const bool valid = row >= 0 && row < config_.providers.size();
    providerForm_->setEnabled(valid);
    const ProviderConfig provider = valid ? config_.providers.at(row) : ProviderConfig{};
    kindLabel_->setText(provider.kind);
    nameEdit_->setText(provider.name);
    baseUrlEdit_->setText(provider.baseUrl);
    apiKeyEdit_->setText(provider.apiKey);
    modelEdit_->setText(provider.defaultModel);
    sttModelEdit_->setText(provider.sttModel);
    ttsModelEdit_->setText(provider.ttsModel);
    ttsVoiceEdit_->setText(provider.ttsVoice);
    textToolsCheck_->setChecked(provider.textTools);
    showKeyCheck_->setChecked(false);
}

void SettingsDialog::storeProvider() {
    if (currentRow_ < 0 || currentRow_ >= config_.providers.size()) return;
    ProviderConfig &provider = config_.providers[currentRow_];
    const QString name = nameEdit_->text().trimmed();
    provider.name = name.isEmpty() ? provider.id : name;
    provider.baseUrl = baseUrlEdit_->text().trimmed();
    provider.apiKey = apiKeyEdit_->text().trimmed();
    provider.defaultModel = modelEdit_->text().trimmed();
    provider.sttModel = sttModelEdit_->text().trimmed();
    provider.ttsModel = ttsModelEdit_->text().trimmed();
    provider.ttsVoice = ttsVoiceEdit_->text().trimmed();
    provider.textTools = textToolsCheck_->isChecked();
}

void SettingsDialog::addProvider(const ProviderPreset &preset) {
    storeProvider();
    ProviderConfig provider = ProviderRegistry::fromPreset(preset);
    provider.id = uniqueId(preset.id);
    if (provider.id != preset.id) provider.name = u"%1 (%2)"_s.arg(preset.name, provider.id);
    config_.providers.append(provider);
    {
        const QSignalBlocker blocker(providerList_);
        providerList_->addItem(provider.name);
    }
    refreshActiveCombo();
    providerList_->setCurrentRow(int(config_.providers.size()) - 1);
}

void SettingsDialog::removeProvider() {
    const int row = providerList_->currentRow();
    if (row < 0 || row >= config_.providers.size()) return;
    currentRow_ = -1;  // the form shows the provider being removed: do not store it back
    config_.providers.removeAt(row);
    const int next = std::min(row, int(config_.providers.size()) - 1);
    {
        const QSignalBlocker blocker(providerList_);
        delete providerList_->takeItem(row);
        providerList_->setCurrentRow(next);
    }
    refreshActiveCombo();
    showProvider(next);
}

void SettingsDialog::refreshActiveCombo() {
    const QString selected = activeCombo_->count() > 0 ? activeCombo_->currentData().toString()
                                                       : config_.activeProvider;
    const QSignalBlocker blocker(activeCombo_);
    activeCombo_->clear();
    for (const ProviderConfig &provider : config_.providers) activeCombo_->addItem(provider.name, provider.id);
    const int index = activeCombo_->findData(selected);
    activeCombo_->setCurrentIndex(index >= 0 ? index : 0);
}

void SettingsDialog::testConnection() {
    storeProvider();
    if (currentRow_ < 0 || currentRow_ >= config_.providers.size()) return;
    Result<std::unique_ptr<IAiProvider>> created = registry_->create(config_.providers.at(currentRow_));
    if (!created) {
        showTestResult(created.error().message, false);
        return;
    }
    testProvider_ = std::move(created).value();
    testButton_->setEnabled(false);
    testResult_->setStyleSheet({});
    testResult_->setText(tr("Testing…"));
    // Dropped if the dialog or the tested provider goes away first.
    testProvider_->listModels([this](Result<QStringList> models) {
        testButton_->setEnabled(true);
        if (models) {
            showTestResult(tr("Connected — %1 models available.").arg(models.value().size()), true);
        } else {
            showTestResult(models.error().message, false);
        }
    });
}

void SettingsDialog::showTestResult(const QString &text, bool ok) {
    testResult_->setText(text);
    testResult_->setStyleSheet(ok ? u"color: #2bb3a0;"_s : u"color: #e5534b;"_s);
}

QString SettingsDialog::uniqueId(const QString &base) const {
    QString id = base;
    for (int n = 2; config_.provider(id); ++n) id = u"%1-%2"_s.arg(base).arg(n);
    return id;
}

} // namespace nexa
