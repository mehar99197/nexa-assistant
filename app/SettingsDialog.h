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

namespace nexa {

class HttpClient;
class IAiProvider;
class ITextToSpeech;

// Settings → Providers (endpoints, API keys, chat and speech models, active provider), Voice
// (recognition and speech engines, wake word), Policy (when to ask before acting) and General
// (system prompt). Edits a copy of the config; the caller applies config() after accept().
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Config config, ProviderRegistry *registry, HttpClient *http, SystemStatus status,
                   QWidget *parent = nullptr);
    ~SettingsDialog() override;

    Config config() const { return config_; }

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
    QWidget *buildPolicyTab();
    QWidget *buildGeneralTab();
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
    QComboBox *policyCombo_{nullptr};
    QPlainTextEdit *systemPromptEdit_{nullptr};
};

} // namespace nexa
