#pragma once

#include "app/SystemStatus.h"
#include "core/ai/ProviderRegistry.h"
#include "core/config/Config.h"

#include <QDialog>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;

namespace nexa {

class HttpClient;
class IAiProvider;
class ITextToSpeech;

// Settings → Providers (endpoints, API keys, chat and speech models, active provider), Voice
// (recognition and speech engines, wake word), Policy (when to ask before acting) and General
// (system prompt). Edits a copy of the config; the caller applies config() after accept().
class BrowserSettingsPage;
class MemorySettingsPage;
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

private:
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
    void fillDesignGallery();
    void populateProviders();
    void showProvider(int row);
    void storeProvider();
    void addProvider(const ProviderPreset &preset);
    void removeProvider();
    void refreshActiveCombo();
    void testConnection();
    void showTestResult(const QString &text, bool ok);
    void storeVoice();
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

    QTabWidget *tabs_{nullptr};
    QListWidget *providerList_{nullptr};
    QComboBox *activeCombo_{nullptr};
    QWidget *providerForm_{nullptr};
    QLabel *kindLabel_{nullptr};
    QLineEdit *nameEdit_{nullptr};
    QLineEdit *baseUrlEdit_{nullptr};
    QLineEdit *apiKeyEdit_{nullptr};
    QCheckBox *showKeyCheck_{nullptr};
    QLineEdit *modelEdit_{nullptr};
    QLineEdit *sttModelEdit_{nullptr};
    QLineEdit *ttsModelEdit_{nullptr};
    QLineEdit *ttsVoiceEdit_{nullptr};
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
    QCheckBox *bargeInCheck_{nullptr};
    QLabel *voiceTestResult_{nullptr};
    QLabel *helperState_{nullptr};
    QLabel *inputState_{nullptr};
    QLabel *authState_{nullptr};
    QPushButton *helperTestButton_{nullptr};
    QPushButton *helperForgetButton_{nullptr};
    QLabel *phoneState_{nullptr};
    QLabel *phoneRemembered_{nullptr};
    QPushButton *phoneRememberButton_{nullptr};
    QCheckBox *phoneReconnect_{nullptr};
    QListWidget *phoneList_{nullptr};
    QLineEdit *pairAddress_{nullptr};
    QLineEdit *pairCode_{nullptr};
    QLineEdit *connectAddress_{nullptr};
    QLabel *phoneResult_{nullptr};
    QCheckBox *remoteEnabled_{nullptr};
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
    SkillsSettingsPage *skillsPage_{nullptr};
};

} // namespace nexa
