#pragma once

#include <QMainWindow>

#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace nexa {

class ActionLogDock;
class AppController;
class ChatView;
class GlobalShortcuts;
class HttpClient;
class ProviderRegistry;
class MicButton;
class ToolExecutor;
enum class VoiceState;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppController *controller, ProviderRegistry *registry, ToolExecutor *executor, HttpClient *http,
               GlobalShortcuts *shortcuts, QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void connectController();
    void connectVoice();
    void showVoiceState(VoiceState state);
    void showHotkeys();
    void sendOrStop();
    void openSettings();
    void promptForApiKey(const QString &providerName);
    void confirm(const QString &question, std::function<void(bool)> answer);
    void showModels(const QStringList &models, const QString &current);
    void showStatus(const QString &message, bool isError);
    void setBusy(bool busy);

    AppController *controller_;
    ProviderRegistry *registry_;
    ToolExecutor *executor_;
    HttpClient *http_;
    GlobalShortcuts *shortcuts_;
    ChatView *chatView_{nullptr};
    ActionLogDock *actionLog_{nullptr};
    MicButton *micButton_{nullptr};
    QPlainTextEdit *input_{nullptr};
    QPushButton *sendButton_{nullptr};
    QComboBox *modelCombo_{nullptr};
    QLabel *providerLabel_{nullptr};
    QLabel *statusLabel_{nullptr};
    QLabel *voiceLabel_{nullptr};
    bool promptingForKey_{false};
};

} // namespace nexa
