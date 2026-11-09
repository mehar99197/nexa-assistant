#include "app/SettingsDialog.h"

#include "app/BrowserSettingsPage.h"
#include "app/MemorySettingsPage.h"
#include "app/MissionSettingsPage.h"
#include "app/ProviderGalleryDialog.h"
#include "app/ProviderSignInBlock.h"
#include "app/SkillsSettingsPage.h"

#include "core/ai/IAiProvider.h"
#include "app/GlobalShortcuts.h"
#include "remote/PairingManager.h"
#include "remote/QrCode.h"
#include "remote/RemoteServer.h"
#include "system/android/AdbDiscovery.h"
#include "system/android/AndroidDevice.h"
#include "app/PhoneLink.h"
#include "core/mcp/McpConnectors.h"
#include "system/helper/HelperClient.h"
#include "ui/NexaCoreWidget.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaThemes.h"
#include "voice/VoiceBackends.h"
#include "voice/tts/CloudVoices.h"
#include "voice/tts/ITextToSpeech.h"
#include "voice/tts/SpeechdTtsBackend.h"
#include "voice/wake/SherpaRuntime.h"
#include "voice/wake/WakePhrases.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPointer>
#include <QRegularExpression>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFileInfo>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QPixmap>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

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
    resize(960, 600);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildProvidersTab(), tr("Providers"));
    missionPage_ = new MissionSettingsPage(config_, this);
    tabs_->addTab(missionPage_, tr("Mission"));
    voicePage_ = buildVoiceTab();
    tabs_->addTab(voicePage_, tr("Voice"));
    tabs_->addTab(buildHelperTab(), tr("Helper"));
    tabs_->addTab(buildAndroidTab(), tr("Android"));
    tabs_->addTab(buildRemoteTab(), tr("Remote"));
    tabs_->addTab(buildConnectorsTab(), tr("Connectors"));
    browserPage_ = new BrowserSettingsPage(config_.browser, status_.browser, this);
    memoryPage_ = new MemorySettingsPage(config_.memory, status_.memory, this);
    skillsPage_ = new SkillsSettingsPage(config_.skills, status_.skills, this);
    tabs_->addTab(browserPage_, tr("Browser"));
    tabs_->addTab(memoryPage_, tr("Memory"));
    tabs_->addTab(skillsPage_, tr("Skills"));
    tabs_->addTab(buildPolicyTab(), tr("Policy"));
    tabs_->addTab(buildGeneralTab(), tr("General"));

    // Back on Providers: the sign-in shown there is looked at again (not within 20 s of the last look).
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (tabs_->widget(index) == providersPage_) checkSignIn();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs_, 1);
    layout->addWidget(buttons);

    populateProviders();
}

void SettingsDialog::showTab(const QString &title) {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (tabs_->tabText(i) == title) tabs_->setCurrentIndex(i);
    }
}

namespace {

// Lets the event loop run until `done` or `ms` pass: the pictures wait for a program's answer this way.
void waitUntil(const std::function<bool()> &done, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents();
}

} // namespace

void SettingsDialog::snapshotTabs(const QString &directory) {
    // Pictures change nothing outside the dialog: no login link is opened, the controller is told nothing.
    snapshotting_ = true;
    if (signIn_) signIn_->setOpensBrowser(false);
    const auto programRows = [this] {
        QList<int> rows;
        for (int row = 0; row < config_.providers.size(); ++row) {
            if (ProviderRegistry::isProgramKind(config_.providers.at(row).kind)) rows.append(row);
        }
        return rows;
    };
    const auto showRow = [this](int row) {
        tabs_->setCurrentWidget(providersPage_);
        providerList_->setCurrentRow(row);
        if (signIn_) signIn_->setOpensBrowser(false);
        // The check starts on the next turn of the event loop; wait for its answer.
        QCoreApplication::processEvents();
        waitUntil([this] { return !signIn_ || !signIn_->busy(); }, 8000);
    };
    // The Providers tab is taken with a program's form in it, its sign-in status read.
    const QList<int> programs = programRows();
    if (!programs.isEmpty()) showRow(programs.first());

    for (int i = 0; i < tabs_->count(); ++i) {
        tabs_->setCurrentIndex(i);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const QString name = tabs_->tabText(i).toLower().remove(u' ');
        grab().save(u"%1/settings-%2.png"_s.arg(directory, name));
    }

    // Each program's form, and — only when NEXA_SNAPSHOT_SIGN_IN is set — a sign-in under way. Starting one is not
    // harmless: agy opens the browser itself and any program's link would be written into the picture. So a picture
    // taken with the owner's own config (status checks only read) never starts one; the pictures of a sign-in are
    // taken against the fake programs (tests/fake-agy, fake-opencode, fake-codex) on a private config.
    const bool signInPictures = qEnvironmentVariableIsSet("NEXA_SNAPSHOT_SIGN_IN");
    for (const int row : programs) {
        const QString kind = config_.providers.at(row).kind;
        showRow(row);
        grab().save(u"%1/settings-providers-%2.png"_s.arg(directory, kind));
        if (!signInPictures || !signIn_ || !signIn_->view().canSignIn) continue;
        signIn_->startSignIn();
        // Several ways to sign in (agy, opencode): the menu that asks which is a picture too, and its first entry is taken.
        waitUntil([this] { return !signIn_->busy() || signIn_->findChild<QMenu *>() != nullptr; }, 8000);
        if (QMenu *menu = signIn_->findChild<QMenu *>(); menu && !menu->actions().isEmpty()) {
            QCoreApplication::processEvents();
            menu->grab().save(u"%1/settings-providers-%2-methods.png"_s.arg(directory, kind));
            menu->actions().first()->trigger();
        }
        waitUntil([this] { return !signIn_->busy() || !signIn_->view().link.isEmpty() || signIn_->view().showCode; },
                  8000);
        if (signIn_->busy()) {
            grab().save(u"%1/settings-providers-%2-signing-in.png"_s.arg(directory, kind));
            signIn_->cancelSignIn();
            waitUntil([this] { return !signIn_->busy(); }, 4000);
        }
    }

    // The gallery of every preset, as it opens and searched.
    ProviderGalleryDialog gallery(registry_->availablePresets(), ProviderRegistry::presetCategories(), this);
    gallery.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    gallery.grab().save(u"%1/provider-gallery.png"_s.arg(directory));
    gallery.setQuery(u"free"_s);
    QCoreApplication::processEvents();
    gallery.grab().save(u"%1/provider-gallery-free.png"_s.arg(directory));
    gallery.close();
}

SettingsDialog::~SettingsDialog() {
    // Discovery polls often only while someone is looking at the result.
    if (status_.discovery) status_.discovery->setWatched(false);
    // A pairing code lives only as long as it is on screen.
    if (status_.pairing) status_.pairing->stopPairing();
}

void SettingsDialog::done(int result) {
    // A sign-in under way ends with the dialog; the program is let go of.
    dropSignIn();
    QDialog::done(result);
}

void SettingsDialog::accept() {
    storeProvider();
    config_.activeProvider = activeCombo_->currentData().toString();
    config_.systemPrompt = systemPromptEdit_->toPlainText().trimmed();
    config_.policyMode = policyCombo_->currentData().toString();
    if (const QListWidgetItem *theme = themeGallery_->currentItem()) config_.ui.theme = theme->data(Qt::UserRole).toString();
    if (const QListWidgetItem *design = designGallery_->currentItem()) config_.ui.design = design->data(Qt::UserRole).toString();
    storeConnector();
    if (const QString name = wakeNameEdit_->text().trimmed(); !name.isEmpty()) {
        if (const Result<void> checked = WakePhrases::check(name); !checked) {
            tabs_->setCurrentWidget(voicePage_);
            wakeNameError_->setText(checked.error().message);
            wakeNameError_->show();
            wakeNameEdit_->setFocus();
            return;
        }
    }
    storeVoice();
    config_.android.autoReconnect = phoneReconnect_->isChecked();
    config_.remote.enabled = remoteEnabled_->isChecked();
    config_.remote.port = remotePort_->value();
    browserPage_->store(config_.browser);
    memoryPage_->store(config_.memory);
    missionPage_->store(config_.mission);
    skillsPage_->store(config_.skills);
    // The memory's own file, not the config: what the page changed, applied to the memory as it is now.
    if (const Result<void> edited = memoryPage_->applyEdits(); !edited) {
        tabs_->setCurrentWidget(memoryPage_);
        QMessageBox::warning(this, tr("Memory"), edited.error().message);
        return;
    }
    QDialog::accept();
}

QWidget *SettingsDialog::buildProvidersTab() {
    auto *page = new QWidget(this);
    providersPage_ = page;

    activeCombo_ = new QComboBox(page);
    auto *activeRow = new QFormLayout;
    activeRow->addRow(tr("First choice:"), activeCombo_);
    activeCombo_->setToolTip(tr("Nexa asks this provider first. When it cannot answer — not running, not signed in, "
                                "out of quota — the others that allow it take over, in the order of the list."));

    providerList_ = new QListWidget(page);
    providerList_->setObjectName(u"providerList"_s);
    providerList_->setMinimumWidth(210);
    connect(providerList_, &QListWidget::currentRowChanged, this, &SettingsDialog::showProvider);

    auto *addButton = new QToolButton(page);
    addButton->setObjectName(u"providerAdd"_s);
    addButton->setText(tr("&Add"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *presetMenu = new QMenu(addButton);
    fillAddMenu(presetMenu);
    addButton->setMenu(presetMenu);
    auto *removeButton = new QPushButton(tr("&Remove"), page);
    removeButton->setObjectName(u"providerRemove"_s);
    // Not pressed by Enter: it keeps the focus and moves to the next row, so Enter removed provider after provider.
    removeButton->setAutoDefault(false);
    connect(removeButton, &QPushButton::clicked, this, &SettingsDialog::removeProvider);
    auto *upButton = new QToolButton(page);
    upButton->setText(u"▲"_s);
    upButton->setToolTip(tr("Ask this provider earlier when the first choice cannot answer"));
    connect(upButton, &QToolButton::clicked, this, [this] { moveProvider(-1); });
    auto *downButton = new QToolButton(page);
    downButton->setText(u"▼"_s);
    downButton->setToolTip(tr("Ask this provider later when the first choice cannot answer"));
    connect(downButton, &QToolButton::clicked, this, [this] { moveProvider(1); });

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(addButton);
    listButtons->addWidget(removeButton);
    listButtons->addWidget(upButton);
    listButtons->addWidget(downButton);
    listButtons->addStretch();

    auto *left = new QVBoxLayout;
    left->addWidget(providerList_, 1);
    left->addLayout(listButtons);

    providerForm_ = new QWidget(page);
    kindLabel_ = new QLabel(providerForm_);
    nameEdit_ = new QLineEdit(providerForm_);
    baseUrlEdit_ = new QLineEdit(providerForm_);
    baseUrlEdit_->setObjectName(u"providerBaseUrl"_s);
    baseUrlEdit_->setPlaceholderText(u"https://api.example.com/v1"_s);
    apiKeyEdit_ = new QLineEdit(providerForm_);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setPlaceholderText(tr("not set"));
    showKeyCheck_ = new QCheckBox(tr("&Show"), providerForm_);
    connect(showKeyCheck_, &QCheckBox::toggled, this, [this](bool show) {
        apiKeyEdit_->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *keyRow = new QHBoxLayout;
    keyRow_ = keyRow;
    keyRow->addWidget(apiKeyEdit_, 1);
    keyRow->addWidget(showKeyCheck_);
    modelEdit_ = new QLineEdit(providerForm_);
    modelEdit_->setPlaceholderText(tr("pick it from the model list in the toolbar"));
    textToolsCheck_ = new QCheckBox(tr("This model cannot call tools — describe them in the prompt instead"),
                                    providerForm_);
    textToolsCheck_->setToolTip(tr("For models without function calling, such as many small local ones: Nexa "
                                   "lists the tools in the system prompt and reads the calls out of the reply."));
    fallbackCheck_ = new QCheckBox(tr("Use when the providers above cannot answer"), providerForm_);
    fallbackCheck_->setToolTip(tr("When the first choice is not running, not signed in or out of quota, Nexa asks "
                                  "the next provider in the list that allows it, with that provider's own model."));
    testButton_ = new QPushButton(tr("&Test connection"), providerForm_);
    connect(testButton_, &QPushButton::clicked, this, &SettingsDialog::testConnection);
    testResult_ = new QLabel(providerForm_);
    testResult_->setWordWrap(true);

    auto *form = new QFormLayout(providerForm_);
    providerFormLayout_ = form;
    form->addRow(tr("Type:"), kindLabel_);
    form->addRow(tr("Name:"), nameEdit_);
    form->addRow(tr("Base URL:"), baseUrlEdit_);
    form->addRow(tr("API key:"), keyRow);
    form->addRow(tr("Model:"), modelEdit_);
    form->addRow(QString(), textToolsCheck_);
    form->addRow(QString(), fallbackCheck_);
    form->addRow(testButton_, testResult_);
    // The program's own sign-in, under the other rows; a row of its own that only a program fills.
    signInHolder_ = new QWidget(providerForm_);
    signInLayout_ = new QVBoxLayout(signInHolder_);
    signInLayout_->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Sign-in:"), signInHolder_);
    form->setRowVisible(signInHolder_, false);
    // A program named in another place: its sign-in is looked up there.
    // (editingFinished also comes when focus merely passes through the field: only a change starts it afresh.)
    const auto programChanged = [this] {
        if (!signIn_ || signIn_->busy() || currentRow_ < 0 || currentRow_ >= config_.providers.size()) return;
        if (baseUrlEdit_->text().trimmed() + u'\n' + apiKeyEdit_->text().trimmed() == signInSource_) return;
        storeProvider();
        showSignIn(config_.providers.at(currentRow_));
    };
    connect(baseUrlEdit_, &QLineEdit::editingFinished, this, programChanged);
    connect(apiKeyEdit_, &QLineEdit::editingFinished, this, programChanged);

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

    // Recognition: the mode, then the cloud part — a provider that has it, its model.
    sttCombo_ = new QComboBox(page);
    sttCombo_->addItem(tr("Automatic — the cloud provider below, local whisper.cpp if it fails"), u"auto"_s);
    sttCombo_->addItem(tr("Cloud only — the cloud provider below"), u"cloud"_s);
    sttCombo_->addItem(tr("Local only — whisper.cpp, offline"), u"whisper"_s);
    selectData(sttCombo_, voice.sttBackend);
    sttProviderCombo_ = new QComboBox(page);
    sttModelCombo_ = new QComboBox(page);
    sttModelCombo_->setEditable(true);
    sttModelCombo_->setToolTip(tr("Transcription models (whisper…) use /audio/transcriptions; any other model gets "
                                  "the recording as chat input."));
    sttUsing_ = new QLabel(page);
    sttUsing_->setWordWrap(true);
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
    languageEdit_->setPlaceholderText(tr("en, ur, … or auto (Urdu and English)"));

    // Speech output: the mode, the cloud provider, its model and voice, and the local voices.
    ttsCombo_ = new QComboBox(page);
    ttsCombo_->addItem(tr("Automatic — the cloud voice below, Piper if it fails"), u"auto"_s);
    ttsCombo_->addItem(tr("Cloud only — the cloud voice below"), u"cloud"_s);
    ttsCombo_->addItem(tr("Piper — local neural voice"), u"piper"_s);
    ttsCombo_->addItem(tr("Speech Dispatcher — local, robotic"), u"speechd"_s);
    ttsCombo_->addItem(tr("Off — never speak"), u"off"_s);
    selectData(ttsCombo_, voice.ttsBackend);
    ttsProviderCombo_ = new QComboBox(page);
    ttsModelCombo_ = new QComboBox(page);
    ttsModelCombo_->setEditable(true);
    ttsVoiceCombo_ = new QComboBox(page);
    ttsVoiceCombo_->setEditable(true);
    ttsVoiceCombo_->setToolTip(tr("The voice's name; for gtts/default a language code such as en"));
    ttsUsing_ = new QLabel(page);
    ttsUsing_->setWordWrap(true);
    connect(ttsModelCombo_, &QComboBox::currentTextChanged, this, [this](const QString &model) {
        const QString voiceName = ttsVoiceCombo_->currentText();
        const QSignalBlocker blocker(ttsVoiceCombo_);
        ttsVoiceCombo_->clear();
        ttsVoiceCombo_->addItems(CloudVoices::voicesFor(model));
        ttsVoiceCombo_->setEditText(voiceName);
    });
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

    // The wake name and how Nexa listens.
    wakeCheck_ = new QCheckBox(tr("Listen for my wake name — the microphone stays on; only offline recognition hears "
                                  "the room"), page);
    wakeCheck_->setChecked(voice.wakeWord);
    wakeNameEdit_ = new QLineEdit(voice.wakeName, page);
    wakeNameEdit_->setPlaceholderText(u"Nexa"_s);
    wakeNameEdit_->setMaxLength(24);
    auto *wakeNameHint = new QLabel(tr("A distinctive name of two or three syllables is heard best."), page);
    wakeNameHint->setWordWrap(true);
    wakeNameError_ = new QLabel(page);
    wakeNameError_->setObjectName(u"settingsError"_s);
    wakeNameError_->setStyleSheet(u"color: #e5534b;"_s);
    wakeNameError_->hide();
    connect(wakeNameEdit_, &QLineEdit::textEdited, wakeNameError_, &QLabel::hide);
    sensitivityCombo_ = new QComboBox(page);
    sensitivityCombo_->addItem(tr("Low — wakes only when sure"), u"low"_s);
    sensitivityCombo_->addItem(tr("Normal"), u"normal"_s);
    sensitivityCombo_->addItem(tr("High — wakes more easily"), u"high"_s);
    selectData(sensitivityCombo_, voice.wakeSensitivity);
    endPauseCombo_ = new QComboBox(page);
    endPauseCombo_->addItem(tr("Short — 0.9 s"), u"short"_s);
    endPauseCombo_->addItem(tr("Normal — 1.4 s"), u"normal"_s);
    endPauseCombo_->addItem(tr("Long — 2 s"), u"long"_s);
    selectData(endPauseCombo_, voice.endPause);
    followUpsCheck_ = new QCheckBox(tr("Keep listening for a follow-up after a spoken reply"), page);
    followUpsCheck_->setChecked(voice.followUps);
    noiseCheck_ = new QCheckBox(tr("Reduce background noise (uses about a fifth of one CPU core while listening)"), page);
    noiseCheck_->setChecked(voice.noiseReduction);
    spotterStatus_ = new QLabel(page);
    spotterStatus_->setWordWrap(true);
    spotterStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (status_.sherpa && status_.sherpa->load()) {
        spotterStatus_->setText(tr("Instant spotting: installed"));
    } else {
        spotterStatus_->setText(tr("Instant spotting: not installed — run scripts/download-sherpa.sh"));
        if (status_.sherpa) spotterStatus_->setToolTip(status_.sherpa->problem());
    }
    micStatus_ = new QLabel(page);
    micStatus_->setWordWrap(true);
    micStatus_->setText(tr("Microphone: %1").arg(status_.microphoneSummary ? status_.microphoneSummary() : tr("fine")));
    auto *lowerMicButton = new QPushButton(tr("Lower it"), page);
    lowerMicButton->setToolTip(tr("Lower the microphone's input volume until its sound is no longer clipped"));
    lowerMicButton->setEnabled(bool(status_.lowerMicrophone));
    connect(lowerMicButton, &QPushButton::clicked, this, [this] {
        if (status_.lowerMicrophone) status_.lowerMicrophone();
        micStatus_->setText(tr("Microphone: lowering its input volume — the result appears in the status bar."));
    });
    bargeInCheck_ = new QCheckBox(tr("Stop speaking when I start talking (use headphones, or Nexa hears itself)"), page);
    bargeInCheck_->setChecked(voice.bargeIn);

    auto *listening = new QFormLayout;
    listening->addRow(tr("Speech recognition:"), sttCombo_);
    listening->addRow(tr("Cloud provider:"), sttProviderCombo_);
    listening->addRow(tr("Model:"), sttModelCombo_);
    listening->addRow(QString(), sttUsing_);
    listening->addRow(tr("Local model:"), whisperCombo_);
    listening->addRow(QString(), whisperStatus_);
    listening->addRow(tr("Language:"), languageEdit_);
    auto *speaking = new QFormLayout;
    speaking->addRow(tr("Speech output:"), ttsCombo_);
    speaking->addRow(tr("Cloud provider:"), ttsProviderCombo_);
    speaking->addRow(tr("Model:"), ttsModelCombo_);
    speaking->addRow(tr("Voice:"), ttsVoiceCombo_);
    speaking->addRow(QString(), ttsUsing_);
    speaking->addRow(tr("Piper voice:"), piperCombo_);
    speaking->addRow(tr("Speech Dispatcher voice:"), speechdCombo_);
    speaking->addRow(tr("Rate:"), rateSlider_);
    speaking->addRow(tr("Speak:"), speakCombo_);
    speaking->addRow(testVoiceButton, voiceTestResult_);
    auto *micRow = new QHBoxLayout;
    micRow->addWidget(micStatus_);
    micRow->addSpacing(12);
    micRow->addWidget(lowerMicButton);
    micRow->addStretch(1);
    auto *waking = new QFormLayout;
    waking->addRow(QString(), wakeCheck_);
    waking->addRow(tr("Wake name:"), wakeNameEdit_);
    waking->addRow(QString(), wakeNameError_);
    waking->addRow(QString(), wakeNameHint);
    waking->addRow(tr("Sensitivity:"), sensitivityCombo_);
    waking->addRow(tr("Pause before I'm done:"), endPauseCombo_);
    waking->addRow(QString(), followUpsCheck_);
    waking->addRow(QString(), noiseCheck_);
    waking->addRow(QString(), spotterStatus_);
    waking->addRow(micRow);
    waking->addRow(QString(), bargeInCheck_);

    auto *note = new QLabel(tr("Talk: click the Talk button (the request ends when you pause), hold it to talk, or "
                               "press Ctrl+M. Esc stops the speech. The cloud voice can be any provider with an "
                               "OpenAI-style audio API and its key."),
                            page);
    note->setWordWrap(true);

    auto *layout = new QVBoxLayout(page);
    layout->addLayout(waking);
    layout->addSpacing(8);
    layout->addLayout(listening);
    layout->addSpacing(8);
    layout->addLayout(speaking);
    layout->addStretch();
    layout->addWidget(note);

    connect(sttProviderCombo_, &QComboBox::currentIndexChanged, this, [this] { showCloudProvider(true); });
    connect(ttsProviderCombo_, &QComboBox::currentIndexChanged, this, [this] { showCloudProvider(false); });
    connect(sttModelCombo_, &QComboBox::currentTextChanged, this, &SettingsDialog::updateUsingLines);
    connect(ttsModelCombo_, &QComboBox::currentTextChanged, this, &SettingsDialog::updateUsingLines);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (tabs_->widget(index) == voicePage_) refreshVoiceProviders();
    });
    refreshVoiceProviders();
    updateVoiceStatus();

    // The tab is taller than the dialog: it scrolls.
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

void SettingsDialog::refreshVoiceProviders() {
    keepCloudModels();
    const auto fill = [](QComboBox *combo, const QList<VoiceProviderChoice> &choices, const QString &selected) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(tr("Automatic — the first provider that has it"), QString());
        auto *items = qobject_cast<QStandardItemModel *>(combo->model());
        for (const VoiceProviderChoice &choice : choices) {
            combo->addItem(choice.usable ? choice.name : tr("%1 — %2").arg(choice.name, choice.reason), choice.id);
            if (!choice.usable && items) items->item(combo->count() - 1)->setEnabled(false);
        }
        const int index = combo->findData(selected);
        combo->setCurrentIndex(index >= 0 ? index : 0);
    };
    const QString stt = sttProviderCombo_->count() > 0 ? sttProviderCombo_->currentData().toString() : config_.voice.sttProvider;
    const QString tts = ttsProviderCombo_->count() > 0 ? ttsProviderCombo_->currentData().toString() : config_.voice.ttsProvider;
    fill(sttProviderCombo_, VoiceBackends::recognitionChoices(config_), stt);
    fill(ttsProviderCombo_, VoiceBackends::speechChoices(config_), tts);
    sttShownFor_.clear();
    ttsShownFor_.clear();
    showCloudProvider(true);
    showCloudProvider(false);
}

void SettingsDialog::showCloudProvider(bool recognition) {
    keepCloudModels();
    QComboBox *providers = recognition ? sttProviderCombo_ : ttsProviderCombo_;
    QComboBox *models = recognition ? sttModelCombo_ : ttsModelCombo_;
    const QString id = providers->currentData().toString();
    (recognition ? sttShownFor_ : ttsShownFor_) = id;
    const ProviderConfig *provider = config_.provider(id);
    // Automatic shows what it picks below instead.
    models->setEnabled(provider != nullptr);
    if (!recognition) ttsVoiceCombo_->setEnabled(provider != nullptr);
    {
        const QSignalBlocker blocker(models);
        models->clear();
        models->setEditText(provider ? (recognition ? provider->sttModel : provider->ttsModel) : QString());
    }
    if (!recognition) {
        const QSignalBlocker blocker(ttsVoiceCombo_);
        ttsVoiceCombo_->clear();
        ttsVoiceCombo_->addItems(CloudVoices::voicesFor(ttsModelCombo_->currentText()));
        ttsVoiceCombo_->setEditText(provider ? provider->ttsVoice : QString());
    }
    if (provider) listCloudModels(recognition);
    updateUsingLines();
}

void SettingsDialog::keepCloudModels() {
    if (ProviderConfig *provider = config_.provider(sttShownFor_)) provider->sttModel = sttModelCombo_->currentText().trimmed();
    if (ProviderConfig *provider = config_.provider(ttsShownFor_)) {
        provider->ttsModel = ttsModelCombo_->currentText().trimmed();
        provider->ttsVoice = ttsVoiceCombo_->currentText().trimmed();
    }
}

void SettingsDialog::listCloudModels(bool recognition) {
    const QString id = (recognition ? sttProviderCombo_ : ttsProviderCombo_)->currentData().toString();
    const ProviderConfig *provider = config_.provider(id);
    if (!provider) return;
    Result<std::unique_ptr<IAiProvider>> created = registry_->create(*provider);
    if (!created) return;
    std::unique_ptr<IAiProvider> &lister = recognition ? sttLister_ : ttsLister_;
    lister = std::move(created).value();
    // Dropped if the dialog or this lister goes away first.
    lister->listModels([this, recognition, id](Result<QStringList> listed) {
        if (!listed || (recognition ? sttShownFor_ : ttsShownFor_) != id) return;
        QComboBox *models = recognition ? sttModelCombo_ : ttsModelCombo_;
        const QString chosen = models->currentText();
        const QSignalBlocker blocker(models);
        models->clear();
        for (const QString &model : listed.value()) {
            if (recognition ? CloudVoices::isRecognitionModel(model) : CloudVoices::isSpeechModel(model)) models->addItem(model);
        }
        models->setEditText(chosen);
    });
}

void SettingsDialog::updateUsingLines() {
    // What Automatic picks with the dialog as it stands.
    Config trial = config_;
    trial.activeProvider = activeCombo_ ? activeCombo_->currentData().toString() : config_.activeProvider;
    if (ProviderConfig *provider = trial.provider(sttShownFor_)) provider->sttModel = sttModelCombo_->currentText().trimmed();
    if (ProviderConfig *provider = trial.provider(ttsShownFor_)) provider->ttsModel = ttsModelCombo_->currentText().trimmed();
    trial.voice.sttProvider.clear();
    trial.voice.ttsProvider.clear();
    const QList<const ProviderConfig *> recognisers = VoiceBackends::recognitionProviders(trial);
    sttUsing_->setText(recognisers.isEmpty()
                           ? tr("No provider has a recognition model: local whisper.cpp is used.")
                           : tr("Using %1 — %2").arg(recognisers.first()->name, recognisers.first()->sttModel));
    sttUsing_->setVisible(sttProviderCombo_->currentData().toString().isEmpty());
    const ProviderConfig *voice = VoiceBackends::speechProvider(trial);
    ttsUsing_->setText(voice ? tr("Using %1 — %2").arg(voice->name, voice->ttsModel)
                             : tr("No provider has a speech model: Piper speaks."));
    ttsUsing_->setVisible(ttsProviderCombo_->currentData().toString().isEmpty());
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
    keepCloudModels();
    voice.sttBackend = sttCombo_->currentData().toString();
    voice.sttProvider = sttProviderCombo_->currentData().toString();
    voice.whisperModel = whisperCombo_->currentText().trimmed().isEmpty() ? VoiceConfig{}.whisperModel
                                                                         : whisperCombo_->currentText().trimmed();
    voice.language = languageEdit_->text().trimmed().isEmpty() ? u"auto"_s : languageEdit_->text().trimmed();
    voice.ttsBackend = ttsCombo_->currentData().toString();
    voice.ttsProvider = ttsProviderCombo_->currentData().toString();
    voice.piperVoice = piperCombo_->currentText();
    voice.speechdVoice = speechdCombo_->currentData().toString();
    voice.ttsRate = rateSlider_->value() / 100.0;
    voice.speakReplies = speakCombo_->currentData().toString();
    voice.wakeWord = wakeCheck_->isChecked();
    voice.bargeIn = bargeInCheck_->isChecked();
    voice.wakeName = wakeNameEdit_->text().trimmed().isEmpty() ? VoiceConfig{}.wakeName : wakeNameEdit_->text().trimmed();
    voice.wakeSensitivity = sensitivityCombo_->currentData().toString();
    voice.endPause = endPauseCombo_->currentData().toString();
    voice.followUps = followUpsCheck_->isChecked();
    voice.noiseReduction = noiseCheck_->isChecked();
}

void SettingsDialog::testVoice() {
    storeProvider();
    storeVoice();
    if (testSpeech_) testSpeech_->stop();
    // As the dialog stands now, the first choice included.
    Config trial = config_;
    trial.activeProvider = activeCombo_->currentData().toString();
    Result<std::unique_ptr<ITextToSpeech>> speech = VoiceBackends::createTextToSpeech(trial, http_);
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

    // Nexa Phone, the app, paired without a finger on either side: the code travels over adb.
    auto *linkButton = new QPushButton(tr("Pair Nexa Phone over adb"), page);
    linkButton->setToolTip(tr("Starts a pairing code here and hands it to the Nexa Phone app on the connected "
                              "phone, which pairs by itself (the web remote is switched on if it is off)."));
    linkButton->setEnabled(status_.phoneLink != nullptr);
    connect(linkButton, &QPushButton::clicked, this, [this, linkButton] {
        if (!status_.phoneLink) return;
        if (!config_.remote.enabled) {
            showPhoneResult(tr("Switch the web remote on (Remote tab) and press OK first; then pair Nexa Phone."), false);
            return;
        }
        linkButton->setEnabled(false);
        const QPointer<SettingsDialog> self(this);
        const QMetaObject::Connection watch =
            connect(status_.phoneLink, &PhoneLink::progress, this, [self](const QString &message) {
                if (self) self->showPhoneResult(message, true);
            });
        status_.phoneLink->pair([self, linkButton, watch](Result<QString> result) {
            if (!self) return;
            QObject::disconnect(watch);
            linkButton->setEnabled(true);
            self->showPhoneResult(result ? result.value() : result.error().message, result.isOk());
        });
    });
    auto *linkRow = new QHBoxLayout;
    linkRow->addWidget(linkButton);
    linkRow->addStretch();

    auto *form = new QFormLayout;
    form->addRow(tr("Phone:"), stateRow);
    form->addRow(tr("Remembered:"), rememberRow);
    form->addRow(QString(), phoneReconnect_);
    form->addRow(tr("Seen by adb:"), phoneList_);
    form->addRow(tr("Pair:"), pairRow);
    form->addRow(tr("Connect:"), connectRow);
    form->addRow(tr("Nexa Phone:"), linkRow);

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

    // The theme gallery: Nexa's own pair following the desktop, and every other pack — a colour
    // family in dark or light, each with its motion style. The toolbar's sun/moon button swaps
    // the scheme within the family.
    themeGallery_ = new QListWidget(page);
    themeGallery_->setViewMode(QListView::IconMode);
    themeGallery_->setIconSize(QSize(96, 64));
    themeGallery_->setGridSize(QSize(142, 112));
    themeGallery_->setSpacing(4);
    themeGallery_->setResizeMode(QListView::Adjust);
    themeGallery_->setMovement(QListView::Static);
    themeGallery_->setWordWrap(true);
    themeGallery_->setUniformItemSizes(true);
    themeGallery_->setMinimumHeight(240);
    themeGallery_->setToolTip(tr("Every theme is the same design in other colours; its motion style is named under it."));
    fillThemeGallery();

    // The designs: each a different arrangement, core and type; choosing one brings the palette
    // it was drawn in, which the palette gallery below may then change.
    designGallery_ = new QListWidget(page);
    designGallery_->setViewMode(QListView::IconMode);
    designGallery_->setIconSize(QSize(112, 72));
    designGallery_->setGridSize(QSize(136, 120));
    designGallery_->setSpacing(4);
    designGallery_->setResizeMode(QListView::Adjust);
    designGallery_->setMovement(QListView::Static);
    designGallery_->setWordWrap(true);
    designGallery_->setUniformItemSizes(true);
    designGallery_->setFixedHeight(258);    // two rows of them
    designGallery_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fillDesignGallery();
    connect(designGallery_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *) {
        if (!current) return;
        const DesignSpec design = NexaDesigns::resolve(current->data(Qt::UserRole).toString());
        for (int i = 0; i < themeGallery_->count(); ++i) {
            if (themeGallery_->item(i)->data(Qt::UserRole).toString() == design.palette) themeGallery_->setCurrentRow(i);
        }
    });

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Design (%n, each its own arrangement and core):", nullptr, int(NexaDesigns::all().size())), page));
    layout->addWidget(designGallery_);
    layout->addSpacing(6);
    layout->addWidget(new QLabel(tr("Palette (%n of them, in eight motion styles):", nullptr, int(NexaThemes::all().size()) + 1), page));
    layout->addWidget(themeGallery_, 2);
    layout->addSpacing(8);
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
    textToolsCheck_->setChecked(provider.textTools);
    fallbackCheck_->setChecked(provider.fallback);
    showKeyCheck_->setChecked(false);
    showRowsFor(provider.kind);
    if (valid) {
        showSignIn(provider);
    } else {
        dropSignIn();
    }
}

void SettingsDialog::storeProvider() {
    if (currentRow_ < 0 || currentRow_ >= config_.providers.size()) return;
    ProviderConfig &provider = config_.providers[currentRow_];
    const QString name = nameEdit_->text().trimmed();
    provider.name = name.isEmpty() ? provider.id : name;
    provider.baseUrl = baseUrlEdit_->text().trimmed();
    provider.apiKey = apiKeyEdit_->text().trimmed();
    provider.defaultModel = modelEdit_->text().trimmed();
    provider.textTools = textToolsCheck_->isChecked();
    provider.fallback = fallbackCheck_->isChecked();
}

void SettingsDialog::showRowsFor(const QString &kind) {
    const bool http = ProviderRegistry::isHttpKind(kind) || kind.isEmpty();
    const bool agy = kind == kKindAgy;
    const bool codex = kind == kKindCodex;
    QFormLayout *form = providerFormLayout_;
    if (auto *label = qobject_cast<QLabel *>(form->labelForField(baseUrlEdit_))) {
        label->setText(http ? tr("Base URL:") : (agy || codex) ? tr("Program:") : tr("Server:"));
    }
    baseUrlEdit_->setPlaceholderText(http   ? u"https://api.example.com/v1"_s
                                     : agy   ? tr("agy on PATH or in ~/.local/bin")
                                     : codex ? tr("codex on PATH or in ~/.npm-global/bin")
                                             : tr("Nexa starts its own"));
    // agy and Codex sign in by themselves (Sign-in below); a server of opencode's that the user runs has a password.
    form->setRowVisible(keyRow_, !agy && !codex);
    if (auto *label = qobject_cast<QLabel *>(form->labelForField(keyRow_))) {
        label->setText(http ? tr("API key:") : tr("Server password:"));
    }
    apiKeyEdit_->setPlaceholderText(http ? tr("not set") : tr("only for a server you run yourself"));
    // The programs take Nexa's tools as text whatever the box says.
    form->setRowVisible(textToolsCheck_, http);
    if (!ProviderRegistry::isProgramKind(kind)) form->setRowVisible(signInHolder_, false);
}

void SettingsDialog::dropSignIn() {
    if (!signIn_) return;
    signInLayout_->removeWidget(signIn_);
    signIn_->shutdown();    // cancels what runs, lets go of the program, deletes itself later
    signIn_ = nullptr;
}

void SettingsDialog::showSignIn(const ProviderConfig &provider) {
    dropSignIn();
    signInSource_ = provider.baseUrl.trimmed() + u'\n' + provider.apiKey.trimmed();
    std::unique_ptr<ISignIn> signIn = registry_->createSignIn(provider);
    providerFormLayout_->setRowVisible(signInHolder_, signIn != nullptr);
    if (!signIn) return;
    signIn_ = new ProviderSignInBlock(provider.kind, std::move(signIn), status_.replyRunning, signInHolder_);
    signIn_->setOpensBrowser(!snapshotting_);
    signInLayout_->addWidget(signIn_);
    // After a sign-in or sign-out the chat takes up the new state: the providers are built again.
    connect(signIn_, &ProviderSignInBlock::providersChanged, this, [this] {
        if (!snapshotting_ && status_.providersChanged) status_.providersChanged();
    });
    // Not inside the constructor or a row change: the tab in front is known by then.
    QPointer<ProviderSignInBlock> shown = signIn_;
    QTimer::singleShot(0, this, [this, shown] {
        if (shown && shown == signIn_) checkSignIn();
    });
}

void SettingsDialog::checkSignIn() {
    if (signIn_ && tabs_ && tabs_->currentWidget() == providersPage_) signIn_->requestCheck();
}

void SettingsDialog::moveProvider(int by) {
    const int row = providerList_->currentRow();
    const int target = row + by;
    if (row < 0 || target < 0 || target >= config_.providers.size()) return;
    storeProvider();
    config_.providers.move(row, target);
    {
        const QSignalBlocker blocker(providerList_);
        QListWidgetItem *item = providerList_->takeItem(row);
        providerList_->insertItem(target, item);
        providerList_->setCurrentRow(target);
    }
    currentRow_ = target;
    refreshActiveCombo();
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

void SettingsDialog::fillAddMenu(QMenu *menu) {
    // By category, the programs first; a category none of whose presets is available is left out, and a preset
    // of a category nobody listed goes under "Other" at the end (the grouping is ProviderRegistry's, tested).
    const QList<ProviderPreset> available = registry_->availablePresets();
    for (const PresetGroup &group : ProviderRegistry::groupPresets(available, ProviderRegistry::presetCategories())) {
        QMenu *submenu = menu->addMenu(group.title == u"Other" ? tr("Other") : group.title);
        for (const int index : group.indices) {
            const ProviderPreset &preset = available.at(index);
            submenu->addAction(preset.name, this, [this, preset] { addProvider(preset); });
        }
    }
    menu->addSeparator();
    menu->addAction(tr("Browse all…"), this, &SettingsDialog::browseProviders);
}

void SettingsDialog::browseProviders() {
    ProviderGalleryDialog gallery(registry_->availablePresets(), ProviderRegistry::presetCategories(), this);
    if (gallery.exec() != QDialog::Accepted) return;
    if (const std::optional<ProviderPreset> chosen = gallery.selectedPreset()) addProvider(*chosen);
}

void SettingsDialog::removeProvider() {
    const int row = providerList_->currentRow();
    if (row < 0 || row >= config_.providers.size()) return;
    const ProviderConfig &provider = config_.providers.at(row);
    QString question = tr("Remove %1 from Nexa?").arg(provider.name);
    if (ProviderRegistry::isProgramKind(provider.kind)) {
        question += u"\n\n"_s + tr("%1 itself stays installed and signed in; Add → Programs puts it back.").arg(provider.name);
    } else if (!provider.apiKey.isEmpty()) {
        question += u"\n\n"_s + tr("Its API key is removed with it.");
    }
    if (QMessageBox::question(this, tr("Remove provider"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
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

// ------------------------------------------------------------------ Connectors

namespace {

QStringList lines(const QString &text) {
    QStringList kept;
    for (const QString &line : text.split(u'\n')) {
        if (!line.trimmed().isEmpty()) kept.append(line.trimmed());
    }
    return kept;
}

// A theme as a picture: its core at rest on its ground, with four of its state colours beside.
QPixmap themeSwatch(const ThemePack &pack) {
    constexpr int kWidth = 96;
    constexpr int kHeight = 64;
    const qreal ratio = 2.0;
    QPixmap pixmap(QSize(kWidth, kHeight) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(pack.tokens.line, 1));
    painter.setBrush(pack.tokens.ground);
    painter.drawRoundedRect(QRectF(0.5, 0.5, kWidth - 1, kHeight - 1), 10, 10);

    NexaCoreWidget core;
    core.setAnimated(false);
    core.setStyle(pack.style);
    core.setIdleColor(pack.tokens.idle);
    core.setWakeColor(pack.tokens.wake);
    core.setListenColor(pack.tokens.listen);
    core.setSttColor(pack.tokens.stt);
    core.setThinkColor(pack.tokens.think);
    core.setActColor(pack.tokens.act);
    core.setSpeakColor(pack.tokens.speak);
    core.setFastColor(pack.tokens.fast);
    core.setErrorColor(pack.tokens.error);
    core.setPanelColor(pack.tokens.panel);
    core.setLineColor(pack.tokens.line);
    core.setMutedColor(pack.tokens.muted);
    core.setTextColor(pack.tokens.text);
    core.setGroundColor(pack.tokens.ground);
    painter.drawPixmap(QRectF(6, 6, 52, 52), core.snapshot(CoreMode::Idle, 52, true, ratio), QRectF(0, 0, 52 * ratio, 52 * ratio));

    painter.setPen(Qt::NoPen);
    const QColor states[] = {pack.tokens.listen, pack.tokens.think, pack.tokens.act, pack.tokens.error};
    for (int i = 0; i < 4; ++i) {
        painter.setBrush(states[i]);
        painter.drawEllipse(QRectF(72, 10 + i * 12, 9, 9));
    }
    painter.setBrush(pack.tokens.text);
    painter.drawRoundedRect(QRectF(66, 54, 22, 3), 1.5, 1.5);
    return pixmap;
}

} // namespace

namespace {

// A design as a picture: the arrangement in panels on its own palette's ground, its core in place.
QPixmap designSwatch(const DesignSpec &design) {
    constexpr int kWidth = 112;
    constexpr int kHeight = 72;
    const qreal ratio = 2.0;
    const ThemePack pack = NexaThemes::resolve(design.palette);
    QPixmap pixmap(QSize(kWidth, kHeight) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double corner = std::clamp(design.radius / 2.0, 0.0, 8.0);
    painter.setPen(QPen(pack.tokens.line, 1));
    painter.setBrush(pack.tokens.ground);
    painter.drawRoundedRect(QRectF(0.5, 0.5, kWidth - 1, kHeight - 1), corner + 2, corner + 2);

    const auto panel = [&painter, &pack, corner](const QRectF &rect) {
        painter.setPen(QPen(pack.tokens.line, 1));
        painter.setBrush(pack.tokens.panel);
        painter.drawRoundedRect(rect, corner, corner);
    };
    const auto lines = [&painter, &pack](const QRectF &rect, int count) {
        painter.setPen(QPen(pack.tokens.muted, 1.2));
        for (int i = 0; i < count; ++i) {
            const double y = rect.top() + 4 + i * 5.0;
            painter.drawLine(QPointF(rect.left() + 4, y), QPointF(rect.left() + 4 + (i % 2 ? rect.width() * 0.5 : rect.width() * 0.75), y));
        }
    };
    QPointF corePlace(16, 18);
    double coreSize = 22;
    switch (design.layout) {
    case WindowLayout::Studio:
        panel(QRectF(4, 4, 104, 8));
        panel(QRectF(4, 16, 74, 52));
        lines(QRectF(4, 30, 74, 36), 5);
        panel(QRectF(82, 16, 26, 52));
        corePlace = QPointF(16, 24);
        coreSize = 16;
        break;
    case WindowLayout::Focus:
        panel(QRectF(30, 34, 52, 34));
        lines(QRectF(30, 34, 52, 34), 4);
        corePlace = QPointF(56, 18);
        coreSize = 26;
        break;
    case WindowLayout::Console:
        panel(QRectF(4, 4, 104, 46));
        lines(QRectF(4, 4, 104, 46), 7);
        panel(QRectF(4, 54, 104, 14));
        corePlace = QPointF(12, 61);
        coreSize = 10;
        break;
    case WindowLayout::Sidebar:
        panel(QRectF(4, 4, 30, 64));
        panel(QRectF(38, 4, 70, 64));
        lines(QRectF(38, 4, 70, 64), 7);
        corePlace = QPointF(19, 18);
        coreSize = 22;
        break;
    case WindowLayout::Orb:
        panel(QRectF(34, 56, 44, 10));
        corePlace = QPointF(56, 28);
        coreSize = 44;
        break;
    case WindowLayout::Dashboard:
        panel(QRectF(4, 4, 50, 30));
        panel(QRectF(58, 4, 50, 30));
        panel(QRectF(4, 38, 50, 30));
        panel(QRectF(58, 38, 50, 30));
        lines(QRectF(58, 4, 50, 30), 4);
        lines(QRectF(4, 38, 50, 30), 4);
        corePlace = QPointF(29, 19);
        coreSize = 22;
        break;
    case WindowLayout::Bottom:
        panel(QRectF(4, 4, 104, 14));
        panel(QRectF(4, 22, 104, 30));
        lines(QRectF(4, 22, 104, 30), 4);
        panel(QRectF(4, 56, 104, 12));
        corePlace = QPointF(14, 11);
        coreSize = 12;
        break;
    case WindowLayout::MissionControl:
        // The agents' rail, the core and the board in the middle, the live activity on the right.
        panel(QRectF(4, 4, 24, 64));
        lines(QRectF(4, 6, 24, 60), 6);
        panel(QRectF(32, 4, 48, 20));
        panel(QRectF(32, 28, 48, 40));
        lines(QRectF(32, 30, 48, 36), 6);
        panel(QRectF(84, 4, 24, 64));
        lines(QRectF(84, 18, 24, 50), 7);
        corePlace = QPointF(42, 14);
        coreSize = 14;
        break;
    }

    NexaCoreWidget core;
    core.setAnimated(false);
    core.setStyle(pack.style);
    core.setShape(design.shape);
    core.setIdleColor(pack.tokens.idle);
    core.setWakeColor(pack.tokens.wake);
    core.setListenColor(pack.tokens.listen);
    core.setSttColor(pack.tokens.stt);
    core.setThinkColor(pack.tokens.think);
    core.setActColor(pack.tokens.act);
    core.setSpeakColor(pack.tokens.speak);
    core.setFastColor(pack.tokens.fast);
    core.setErrorColor(pack.tokens.error);
    core.setPanelColor(pack.tokens.panel);
    core.setLineColor(pack.tokens.line);
    core.setMutedColor(pack.tokens.muted);
    core.setTextColor(pack.tokens.text);
    core.setGroundColor(pack.tokens.ground);
    const QRectF where(corePlace.x() - coreSize / 2, corePlace.y() - coreSize / 2, coreSize, coreSize);
    painter.drawPixmap(where, core.snapshot(CoreMode::Idle, int(coreSize), coreSize >= 20, ratio),
                       QRectF(0, 0, coreSize * ratio, coreSize * ratio));
    return pixmap;
}

} // namespace

void SettingsDialog::fillDesignGallery() {
    designGallery_->clear();
    for (const DesignSpec &design : NexaDesigns::all()) {
        auto *item = new QListWidgetItem(design.name);
        item->setData(Qt::UserRole, design.id);
        item->setToolTip(design.blurb);
        item->setIcon(designSwatch(design));
        designGallery_->addItem(item);
        if (design.id == config_.ui.design) designGallery_->setCurrentItem(item);
    }
    if (!designGallery_->currentItem()) designGallery_->setCurrentRow(0);
}

void SettingsDialog::fillThemeGallery() {
    themeGallery_->clear();
    const QString current = config_.ui.theme;
    auto *system = new QListWidgetItem(tr("Follow the system\nNexa · Classic"));
    system->setData(Qt::UserRole, u"system"_s);
    system->setToolTip(tr("Nexa's own dark or light look, as the desktop is set."));
    system->setIcon(themeSwatch(NexaThemes::resolve(u"system"_s)));
    themeGallery_->addItem(system);
    if (current == u"system"_s) themeGallery_->setCurrentItem(system);
    for (const ThemePack &pack : NexaThemes::all()) {
        auto *item = new QListWidgetItem(u"%1\n%2"_s.arg(pack.name, coreStyleName(pack.style)));
        item->setData(Qt::UserRole, NexaThemes::settingFor(pack));
        item->setToolTip(coreStyleBlurb(pack.style));
        item->setIcon(themeSwatch(pack));
        themeGallery_->addItem(item);
        if (NexaThemes::settingFor(pack) == current) themeGallery_->setCurrentItem(item);
    }
    if (!themeGallery_->currentItem()) themeGallery_->setCurrentRow(0);
}

// Settings → Connectors: MCP servers whose tools the model may call. The form edits config_.mcp
// in place as it is typed, so OK keeps whatever it shows.
QWidget *SettingsDialog::buildConnectorsTab() {
    auto *page = new QWidget(this);

    mcpList_ = new QListWidget(page);
    mcpList_->setMinimumWidth(180);
    for (const McpServerConfig &server : config_.mcp.servers) {
        mcpList_->addItem(server.name.isEmpty() ? server.id : server.name);
    }
    auto *addButton = new QPushButton(tr("&Add"), page);
    auto *removeButton = new QPushButton(tr("Re&move"), page);
    removeButton->setObjectName(u"connectorRemove"_s);
    removeButton->setAutoDefault(false);    // Enter is OK, never Remove (it moves on to the next connector)
    connect(addButton, &QPushButton::clicked, this, &SettingsDialog::addConnector);
    connect(removeButton, &QPushButton::clicked, this, &SettingsDialog::removeConnector);

    mcpForm_ = new QWidget(page);
    mcpName_ = new QLineEdit(mcpForm_);
    mcpEnabled_ = new QCheckBox(tr("On"), mcpForm_);
    mcpTransport_ = new QComboBox(mcpForm_);
    mcpTransport_->addItem(tr("A program on this PC (stdio)"), u"stdio"_s);
    mcpTransport_->addItem(tr("A server at a URL (HTTP)"), u"http"_s);
    mcpCommand_ = new QLineEdit(mcpForm_);
    mcpCommand_->setPlaceholderText(tr("npx, uvx, python3, or a path"));
    mcpArguments_ = new QLineEdit(mcpForm_);
    mcpArguments_->setPlaceholderText(tr("-y @modelcontextprotocol/server-filesystem /home/me"));
    mcpEnvironment_ = new QPlainTextEdit(mcpForm_);
    mcpEnvironment_->setPlaceholderText(tr("KEY=value — one per line, for API keys the server needs"));
    mcpEnvironment_->setMaximumHeight(64);
    mcpUrl_ = new QLineEdit(mcpForm_);
    mcpUrl_->setPlaceholderText(tr("https://example.com/mcp"));
    mcpHeaders_ = new QPlainTextEdit(mcpForm_);
    mcpHeaders_->setPlaceholderText(tr("Authorization: Bearer … — one per line"));
    mcpHeaders_->setMaximumHeight(64);
    auto *testButton = new QPushButton(tr("&Test"), mcpForm_);
    testButton->setToolTip(tr("Connects, lists the server's tools, and disconnects."));
    connect(testButton, &QPushButton::clicked, this, &SettingsDialog::testConnector);
    mcpResult_ = new QLabel(mcpForm_);
    mcpResult_->setWordWrap(true);
    mcpResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(mcpName_, 1);
    nameRow->addWidget(mcpEnabled_);
    auto *form = new QFormLayout(mcpForm_);
    form->addRow(tr("Name:"), nameRow);
    form->addRow(tr("Reached as:"), mcpTransport_);
    form->addRow(tr("Program:"), mcpCommand_);
    form->addRow(tr("Arguments:"), mcpArguments_);
    form->addRow(tr("Environment:"), mcpEnvironment_);
    form->addRow(tr("URL:"), mcpUrl_);
    form->addRow(tr("Headers:"), mcpHeaders_);
    form->addRow(testButton, mcpResult_);

    connect(mcpName_, &QLineEdit::textEdited, this, [this](const QString &name) {
        if (QListWidgetItem *item = mcpList_->currentItem()) item->setText(name);
        storeConnector();
    });
    for (QLineEdit *field : {mcpCommand_, mcpArguments_, mcpUrl_}) {
        connect(field, &QLineEdit::textChanged, this, &SettingsDialog::storeConnector);
    }
    for (QPlainTextEdit *field : {mcpEnvironment_, mcpHeaders_}) {
        connect(field, &QPlainTextEdit::textChanged, this, &SettingsDialog::storeConnector);
    }
    connect(mcpTransport_, &QComboBox::currentIndexChanged, this, &SettingsDialog::storeConnector);
    connect(mcpEnabled_, &QCheckBox::toggled, this, &SettingsDialog::storeConnector);
    connect(mcpList_, &QListWidget::currentRowChanged, this, &SettingsDialog::showConnector);

    mcpStates_ = new QLabel(page);
    mcpStates_->setWordWrap(true);
    mcpStates_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    showConnectorStates();
    if (status_.mcp) connect(status_.mcp, &McpConnectors::statusChanged, this, &SettingsDialog::showConnectorStates);

    auto *help = new QLabel(
        tr("A connector is an MCP server (Model Context Protocol): a program Nexa starts and talks to on its own, "
           "or a server at a URL. Its tools join Nexa's as mcp_<name>_<tool> and the model calls them like any "
           "other — through the policy, the Actions panel and the audit log. In confirm-risky mode Nexa asks "
           "before each call. Changes take effect when you press OK."),
        page);
    help->setWordWrap(true);

    auto *left = new QVBoxLayout;
    left->addWidget(mcpList_, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(addButton);
    buttons->addWidget(removeButton);
    left->addLayout(buttons);
    auto *columns = new QHBoxLayout;
    columns->addLayout(left);
    columns->addWidget(mcpForm_, 1);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(columns, 1);
    layout->addWidget(mcpStates_);
    layout->addSpacing(6);
    layout->addWidget(help);

    showConnector(config_.mcp.servers.isEmpty() ? -1 : 0);
    if (!config_.mcp.servers.isEmpty()) mcpList_->setCurrentRow(0);
    return page;
}

void SettingsDialog::showConnector(int row) {
    mcpRow_ = row;
    const bool have = row >= 0 && row < config_.mcp.servers.size();
    mcpForm_->setEnabled(have);
    loadingConnector_ = true;
    const McpServerConfig server = have ? config_.mcp.servers.at(row) : McpServerConfig{};
    mcpName_->setText(server.name);
    mcpEnabled_->setChecked(server.enabled);
    mcpTransport_->setCurrentIndex(std::max(0, mcpTransport_->findData(server.transport)));
    mcpCommand_->setText(server.command);
    mcpArguments_->setText(server.arguments.join(u' '));
    mcpEnvironment_->setPlainText(server.environment.join(u'\n'));
    mcpUrl_->setText(server.url);
    mcpHeaders_->setPlainText(server.headers.join(u'\n'));
    mcpResult_->clear();
    loadingConnector_ = false;
}

void SettingsDialog::storeConnector() {
    if (loadingConnector_ || mcpRow_ < 0 || mcpRow_ >= config_.mcp.servers.size()) return;
    McpServerConfig &server = config_.mcp.servers[mcpRow_];
    server.name = mcpName_->text().trimmed();
    server.enabled = mcpEnabled_->isChecked();
    server.transport = mcpTransport_->currentData().toString();
    server.command = mcpCommand_->text().trimmed();
    server.arguments = QProcess::splitCommand(mcpArguments_->text());
    server.environment = lines(mcpEnvironment_->toPlainText());
    server.url = mcpUrl_->text().trimmed();
    server.headers = lines(mcpHeaders_->toPlainText());
}

void SettingsDialog::addConnector() {
    McpServerConfig server;
    int n = config_.mcp.servers.size() + 1;
    const auto taken = [this](const QString &id) {
        return std::any_of(config_.mcp.servers.cbegin(), config_.mcp.servers.cend(),
                           [&id](const McpServerConfig &s) { return s.id == id; });
    };
    server.id = u"connector-%1"_s.arg(n);
    while (taken(server.id)) server.id = u"connector-%1"_s.arg(++n);
    server.name = tr("New connector");
    config_.mcp.servers.append(server);
    mcpList_->addItem(server.name);
    mcpList_->setCurrentRow(mcpList_->count() - 1);
    mcpName_->setFocus();
    mcpName_->selectAll();
}

void SettingsDialog::removeConnector() {
    const int row = mcpList_->currentRow();
    if (row < 0 || row >= config_.mcp.servers.size()) return;
    const McpServerConfig &server = config_.mcp.servers.at(row);
    if (QMessageBox::question(this, tr("Remove connector"),
                              tr("Remove the connector %1?").arg(server.name.isEmpty() ? server.id : server.name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    mcpRow_ = -1;
    config_.mcp.servers.removeAt(row);
    delete mcpList_->takeItem(row);
    showConnector(mcpList_->currentRow());
}

void SettingsDialog::testConnector() {
    storeConnector();
    if (mcpRow_ < 0 || mcpRow_ >= config_.mcp.servers.size()) return;
    const McpServerConfig server = config_.mcp.servers.at(mcpRow_);
    mcpResult_->setText(tr("Connecting…"));
    const QPointer<SettingsDialog> self(this);
    McpConnectors::probe(server, http_, this, [self](Result<QStringList> tools) {
        if (!self) return;
        if (!tools) {
            self->mcpResult_->setText(tools.error().message);
            self->mcpResult_->setStyleSheet(u"color: %1;"_s.arg(NexaTheme::tokens(ThemeScheme::Dark).error.name()));
            return;
        }
        self->mcpResult_->setStyleSheet({});
        self->mcpResult_->setText(tools.value().isEmpty()
                                      ? tr("Connected; the server offers no tools.")
                                      : tr("Connected. %n tool(s): %1", nullptr, int(tools.value().size()))
                                            .arg(tools.value().join(u", "_s)));
    });
}

void SettingsDialog::showConnectorStates() {
    if (!status_.mcp) {
        mcpStates_->clear();
        return;
    }
    QStringList report;
    for (const McpConnectors::Status &status : status_.mcp->statuses()) {
        QString state = status.state;
        if (state == u"ready"_s) state = tr("connected");
        if (state == u"failed"_s) state = tr("failed");
        if (state == u"connecting"_s) state = tr("connecting");
        if (state == u"off"_s) state = tr("off");
        report.append(u"• %1 — %2%3"_s.arg(status.name.isEmpty() ? status.id : status.name, state,
                                             status.detail.isEmpty() ? QString() : u": "_s + status.detail));
    }
    mcpStates_->setText(report.isEmpty() ? tr("No connector is running.") : report.join(u'\n'));
}

} // namespace nexa
