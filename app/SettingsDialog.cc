#include "app/SettingsDialog.h"

#include "core/ai/IAiProvider.h"
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
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
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

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::accept() {
    storeProvider();
    config_.activeProvider = activeCombo_->currentData().toString();
    config_.systemPrompt = systemPromptEdit_->toPlainText().trimmed();
    config_.policyMode = policyCombo_->currentData().toString();
    storeVoice();
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
    status_.helper->isAuthorized([this](Result<bool> authorized) {
        if (!authorized) {
            authState_->setText(authorized.error().message);
            return;
        }
        authState_->setText(authorized.value()
                                ? tr("Given for this login — nothing will ask again until you log out.")
                                : tr("Asked for once, the next time Nexa needs the helper."));
    });
}

void SettingsDialog::testHelper() {
    if (!status_.helper) return;
    helperTestButton_->setEnabled(false);
    helperState_->setStyleSheet({});
    helperState_->setText(tr("Starting the helper…"));
    status_.helper->probe([this](Result<QString> version) {
        helperTestButton_->setEnabled(true);
        if (version) {
            helperState_->setText(tr("Working — helper version %1.").arg(version.value()));
            helperState_->setStyleSheet(u"color: #2bb3a0;"_s);
        } else {
            helperState_->setText(version.error().message);
            helperState_->setStyleSheet(u"color: #e5534b;"_s);
        }
        if (status_.inputSummary) showHelperStatus();
    });
}

void SettingsDialog::forgetAuthorization() {
    if (!status_.helper) return;
    helperForgetButton_->setEnabled(false);
    status_.helper->forgetAuthorization([this](Result<void> done) {
        helperForgetButton_->setEnabled(true);
        if (!done) {
            authState_->setText(done.error().message);
            return;
        }
        authState_->setText(tr("Forgotten — the next action asks for your password again."));
    });
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

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("System prompt (sent at the start of every conversation):"), page));
    layout->addWidget(systemPromptEdit_, 1);
    layout->addWidget(restore, 0, Qt::AlignRight);
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
