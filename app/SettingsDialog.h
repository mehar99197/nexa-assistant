#pragma once

#include "app/SystemStatus.h"
#include "core/ai/ProviderRegistry.h"
#include "core/config/Config.h"

#include <QDialog>

#include <memory>
#include <optional>

class QButtonGroup;
class QCheckBox;
class QFormLayout;
class QHBoxLayout;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace nexa {

class HttpClient;
class IAiProvider;
class ITextToSpeech;

// Settings → Providers (endpoints, API keys, chat and speech models, active provider), Voice
// (recognition and speech engines, wake word), Policy (when to ask before acting) and General
// (system prompt). Edits a copy of the config; the caller applies config() after accept().
class BrowserSettingsPage;
class MemorySettingsPage;
class MissionSettingsPage;
class ProviderSignInBlock;
class SkillsSettingsPage;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Config config, ProviderRegistry *registry, HttpClient *http, SystemStatus status,
                   QWidget *parent = nullptr);
    ~SettingsDialog() override;

    Config config() const { return config_; }
    void showTab(const QString &title);         // e.g. "Policy": open on that page
    // A picture of every tab into `directory` (settings-<tab>.png), for checking the look.
    void snapshotTabs(const QString &directory);

    void accept() override;
    void done(int result) override;     // lets go of a running sign-in

private:
    void addPage(QWidget *page, const QString &title);     // in a scroll area of its own
    QWidget *tabOf(QWidget *page) const;                    // the tab that shows `page`
    void fitToScreen();
    QWidget *buildProvidersTab();
    QWidget *buildVoiceTab();
    QWidget *buildHelperTab();
    void showHelperStatus();
    void showAuthorizationState();
    void testHelper();
    void forgetAuthorization();
    QWidget *buildAndroidTab();
    void showPhone();
    void showDiscovered();
    void rememberPhone();
    void pairPhone();
    void connectPhone();
    void disconnectPhones();
    void showPhoneResult(const QString &text, bool ok);
    QWidget *buildRemoteTab();
    void showRemote();
    void showAwayFromHome();      // Tailscale, Cloudflare and ngrok, as RemoteAccess finds them
    void startPairing();
    void showPairing();
    void showPairedDevices();
    QWidget *buildConnectorsTab();
    void showConnector(int row);
    void storeConnector();
    void addConnector();
    void removeConnector();
    void testConnector();
    void showConnectorStates();
    QWidget *buildPolicyTab();
    QWidget *buildGeneralTab();
    void fillThemeGallery();
    void showTheme(const QString &setting);     // the tile of a theme, its category shown if the chips hid it
    void fillDesignGallery();
    void populateProviders();
    void showProvider(int row);
    void storeProvider();
    void addProvider(const ProviderPreset &preset);
    void removeProvider();
    void fillAddMenu(QMenu *menu);          // the presets by category, then Browse all…
    void browseProviders();                 // the gallery of every preset
    void showSignIn(const ProviderConfig &provider);    // the sign-in block of a program's form, or none
    void dropSignIn();
    void checkSignIn();                     // a status check, when the Providers tab is the one in front
    void moveProvider(int by);              // ▲ / ▼: the order the chain follows after the first choice
    void showRowsFor(const QString &kind);  // the fields a provider of this kind uses
    void refreshActiveCombo();
    void testConnection();
    void showTestResult(const QString &text, bool ok);
    void storeVoice();
    void refreshMyVoice();
    void refreshOwnVoice();         // "Speak in my voice": the sample, and whether ZipVoice and the phonemizer are there
    void hearOwnVoice();            // a sentence in the owner's voice, from the sample
    void refreshVoiceProviders();       // the provider lists, from the providers as they are now
    void showCloudProvider(bool recognition);
    void keepCloudModels();             // the shown provider's model (and voice) back into its settings
    void listCloudModels(bool recognition);
    void updateUsingLines();
    void updateVoiceStatus();
    void testVoice();
    QString uniqueId(const QString &base) const;

    Config config_;
    ProviderRegistry *registry_;
    HttpClient *http_;
    SystemStatus status_;
    std::unique_ptr<IAiProvider> testProvider_;
    std::unique_ptr<ITextToSpeech> testSpeech_;
    int currentRow_{-1};
    bool snapshotting_{false};          // pictures: the login link is not opened and nothing is told to the controller

    QTabWidget *tabs_{nullptr};
    QListWidget *providerList_{nullptr};
    QComboBox *activeCombo_{nullptr};
    QWidget *providerForm_{nullptr};
    QLabel *kindLabel_{nullptr};
    QLineEdit *nameEdit_{nullptr};
    QLineEdit *baseUrlEdit_{nullptr};
    QLineEdit *apiKeyEdit_{nullptr};
    QCheckBox *showKeyCheck_{nullptr};
    QCheckBox *fallbackCheck_{nullptr};
    QFormLayout *providerFormLayout_{nullptr};
    QWidget *providersPage_{nullptr};
    QWidget *signInHolder_{nullptr};    // the form's row for the sign-in block of the program shown
    QVBoxLayout *signInLayout_{nullptr};
    ProviderSignInBlock *signIn_{nullptr};
    QString signInSource_;              // the program path and server password the sign-in shown was made for
    QHBoxLayout *keyRow_{nullptr};
    QLineEdit *modelEdit_{nullptr};
    QCheckBox *textToolsCheck_{nullptr};
    QPushButton *testButton_{nullptr};
    QLabel *testResult_{nullptr};
    QComboBox *sttCombo_{nullptr};
    QComboBox *whisperCombo_{nullptr};
    QLineEdit *languageEdit_{nullptr};
    QLabel *whisperStatus_{nullptr};
    QComboBox *ttsCombo_{nullptr};
    QComboBox *piperCombo_{nullptr};
    QComboBox *speechdCombo_{nullptr};
    QSlider *rateSlider_{nullptr};
    QComboBox *speakCombo_{nullptr};
    QCheckBox *wakeCheck_{nullptr};
    QComboBox *bargeInCombo_{nullptr};
    QCheckBox *conversationCheck_{nullptr};
    QComboBox *announceCombo_{nullptr};
    QComboBox *listenToCombo_{nullptr};
    QComboBox *strictnessCombo_{nullptr};
    QLabel *myVoiceStatus_{nullptr};
    QPushButton *enrolButton_{nullptr};
    QPushButton *forgetVoiceButton_{nullptr};
    QCheckBox *ownVoiceCheck_{nullptr};
    QLabel *ownVoiceStatus_{nullptr};
    QPushButton *recordSampleButton_{nullptr};
    QPushButton *hearSampleButton_{nullptr};
    QPushButton *forgetSampleButton_{nullptr};
    // Settings → Voice: the cloud voice, chosen from the providers there are.
    QComboBox *sttProviderCombo_{nullptr};
    QComboBox *sttModelCombo_{nullptr};
    QLabel *sttUsing_{nullptr};
    QComboBox *ttsProviderCombo_{nullptr};
    QComboBox *ttsModelCombo_{nullptr};
    QComboBox *ttsVoiceCombo_{nullptr};
    QLabel *ttsUsing_{nullptr};
    QString sttShownFor_;               // the provider whose recognition model the Model list shows
    QString ttsShownFor_;
    std::unique_ptr<IAiProvider> sttLister_;
    std::unique_ptr<IAiProvider> ttsLister_;
    // …the wake name and how Nexa listens.
    QWidget *voicePage_{nullptr};
    QLineEdit *wakeNameEdit_{nullptr};
    QLabel *wakeNameError_{nullptr};
    QComboBox *sensitivityCombo_{nullptr};
    QComboBox *endPauseCombo_{nullptr};
    QCheckBox *followUpsCheck_{nullptr};
    QCheckBox *noiseCheck_{nullptr};
    QLabel *spotterStatus_{nullptr};
    QLabel *micStatus_{nullptr};
    QLabel *voiceTestResult_{nullptr};
    QLabel *helperState_{nullptr};
    QLabel *inputState_{nullptr};
    QLabel *authState_{nullptr};
    QPushButton *helperTestButton_{nullptr};
    QPushButton *helperForgetButton_{nullptr};
    QLabel *phoneState_{nullptr};
    QLabel *phoneRemembered_{nullptr};
    QPushButton *phoneRememberButton_{nullptr};
    QCheckBox *exposeContentsCheck_{nullptr};   // General → Screen as text: GNOME's accessibility switch
    bool exposeContentsWas_{false};
    QCheckBox *phoneReconnect_{nullptr};
    QListWidget *phoneList_{nullptr};
    QLineEdit *pairAddress_{nullptr};
    QLineEdit *pairCode_{nullptr};
    QLineEdit *connectAddress_{nullptr};
    QLabel *phoneResult_{nullptr};
    QCheckBox *remoteEnabled_{nullptr};
    QCheckBox *remoteAnnounce_{nullptr};
    // Away from home.
    QCheckBox *tailscaleCheck_{nullptr};
    QLabel *tailscaleState_{nullptr};
    QPushButton *tailscaleSignIn_{nullptr};
    QComboBox *cloudflareMode_{nullptr};
    QLineEdit *cloudflareHostname_{nullptr};
    QLabel *cloudflareState_{nullptr};
    QCheckBox *ngrokCheck_{nullptr};
    QLineEdit *ngrokDomain_{nullptr};
    QLabel *ngrokState_{nullptr};
    QSpinBox *remotePort_{nullptr};
    QLabel *remoteState_{nullptr};
    QLabel *remoteFingerprint_{nullptr};
    QPushButton *pairButton_{nullptr};
    QLabel *qrLabel_{nullptr};
    QLabel *pairCodeLabel_{nullptr};
    QListWidget *pairedList_{nullptr};
    QPushButton *revokeButton_{nullptr};
    QTimer *pairCountdown_{nullptr};
    QComboBox *policyCombo_{nullptr};
    QListWidget *themeGallery_{nullptr};
    QButtonGroup *themeChips_{nullptr};         // the categories; "" is every pack
    QString themeCategory_;                     // the chip pressed
    QString themeChoice_;                       // the pack chosen, kept while a chip hides its tile
    QListWidget *designGallery_{nullptr};
    QPlainTextEdit *systemPromptEdit_{nullptr};
    QListWidget *mcpList_{nullptr};
    QWidget *mcpForm_{nullptr};
    QLineEdit *mcpName_{nullptr};
    QCheckBox *mcpEnabled_{nullptr};
    QComboBox *mcpTransport_{nullptr};
    QLineEdit *mcpCommand_{nullptr};
    QLineEdit *mcpArguments_{nullptr};
    QPlainTextEdit *mcpEnvironment_{nullptr};
    QLineEdit *mcpUrl_{nullptr};
    QPlainTextEdit *mcpHeaders_{nullptr};
    QLabel *mcpResult_{nullptr};
    QLabel *mcpStates_{nullptr};
    int mcpRow_{-1};
    bool loadingConnector_{false};      // the form is being filled, not edited

    BrowserSettingsPage *browserPage_{nullptr};
    MemorySettingsPage *memoryPage_{nullptr};
    MissionSettingsPage *missionPage_{nullptr};
    SkillsSettingsPage *skillsPage_{nullptr};
};

} // namespace nexa
