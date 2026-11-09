#pragma once

#include "app/SystemStatus.h"
#include "ui/NexaCoreSpec.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaIcons.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include <QMainWindow>

#include <functional>
#include <utility>
#include <vector>

class QAction;
class QBoxLayout;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace nexa {

class ActionLogDock;
class AppController;
class ChatView;
class GlobalShortcuts;
class HttpClient;
class ProviderRegistry;
class MicButton;
class NexaCoreWidget;
class PhonePanel;
class ToolExecutor;

// The desktop window: a toolbar of pills (provider, model, policy, Stop all, the theme), the Nexa
// core with the state's name and a hint beside it, the transcript, the input bar with Share screen
// (a switch: off, the model never looks at the screen), See phone, Send and Talk (the core again),
// the Actions panel on the right, the Phone panel beside it while a phone is connected (its screen,
// live) and a one-line status bar in monospace. Everything it shows comes from AppController's
// signals; nothing here decides.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppController *controller, ProviderRegistry *registry, ToolExecutor *executor, HttpClient *http,
               GlobalShortcuts *shortcuts, SystemStatus status, const QString &auditPath, QWidget *parent = nullptr);

    void openSettings(const QString &tab = {});
    // Opens Settings without blocking, saves a picture of each tab into `directory`, closes it.
    void snapshotSettings(const QString &directory, std::function<void()> done = {});
    // Wears a palette and a design for this run, without touching the settings (--theme, --design).
    void wear(const QString &palette, const QString &design);
    // Asked when the window is closed: true means "hidden to the tray, keep running".
    void setCloseToTray(std::function<bool()> hide) { closeToTray_ = std::move(hide); }
    NexaCoreWidget *presence() const { return presence_; }    // the core beside the chat (the tray copies it)
    ThemeScheme scheme() const { return pack_.scheme; }
    const ThemePack &theme() const { return pack_; }
    const DesignSpec &design() const { return design_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildToolbar();
    void buildCentral();
    void buildStatusBar();
    void connectController();
    void connectVoice();
    QWidget *makePill(const QString &label, QWidget *value);
    QToolButton *makeIconButton(NexaIcons::Kind kind, const QString &objectName, QAction *action);
    void showCoreMode(CoreMode mode);
    QString titleFor(CoreMode mode) const;
    QString hintFor(CoreMode mode) const;
    void showHotkeys();
    void sendOrStop();
    void toggleTheme();
    void applyPack(const ThemePack &pack);
    void applyDesign(const DesignSpec &design);     // arranges the pieces as the design says
    void rebuildCentral();
    void refreshTheme();
    void promptForApiKey(const QString &providerName);
    void showSignInRequired(const QString &providerName);
    void confirm(const QString &question, std::function<void(bool)> answer);
    void showModels(const QStringList &models, const QString &current);
    void showStatus(const QString &message, bool isError);
    void showMicrophoneTrouble(const QString &notice);  // the notice, with "Lower it" beside it
    void showPolicy();
    void showVoiceEngines();
    void showHelper();
    void showPhone(const QString &description);
    void showRemote();
    void showScreenSharing(bool shared);
    void showBrowser();
    void showPhonePanel(bool connected);
    void showAppContext(const QString &name, const QString &hint);
    void refreshSendButton();
    void askApproval(quint64 task, quint64 actionId);

    AppController *controller_;
    ProviderRegistry *registry_;
    ToolExecutor *executor_;
    HttpClient *http_;
    GlobalShortcuts *shortcuts_;
    SystemStatus status_;
    QString auditPath_;
    ThemePack pack_;                // the palette on: colours, scheme and the core's motion style
    DesignSpec design_;             // the design on: layout, core shape, corners, type
    ChatView *chatView_{nullptr};
    ActionLogDock *actionLog_{nullptr};
    PhonePanel *phonePanel_{nullptr};
    NexaCoreWidget *presence_{nullptr};
    MicButton *micButton_{nullptr};
    QWidget *header_{nullptr};      // the core with the state's name: beside the chat, or stacked
    QBoxLayout *headerLayout_{nullptr};
    QWidget *titles_{nullptr};
    QWidget *inputBar_{nullptr};
    NexaCoreWidget *statusCore_{nullptr};   // Console: the core in the status line
    QLabel *statusState_{nullptr};
    QLabel *stateTitle_{nullptr};
    QLabel *stateHint_{nullptr};
    QPlainTextEdit *input_{nullptr};
    QPushButton *sendButton_{nullptr};
    QComboBox *modelCombo_{nullptr};
    QLabel *providerLabel_{nullptr};
    QToolButton *policyButton_{nullptr};
    QAction *themeAction_{nullptr};
    QAction *shareScreenAction_{nullptr};
    QLabel *statusLabel_{nullptr};
    QToolButton *lowerMicButton_{nullptr};
    QToolButton *signInButton_{nullptr};    // in the status bar while a program provider needs its sign-in
    QLabel *screenLabel_{nullptr};
    QLabel *browserLabel_{nullptr};
    QLabel *appLabel_{nullptr};     // "In: GNOME Terminal" while Nexa knows where the user is
    QString appHint_;
    QLabel *helperLabel_{nullptr};
    QLabel *sttLabel_{nullptr};
    QLabel *ttsLabel_{nullptr};
    QLabel *phoneLabel_{nullptr};
    QLabel *remoteLabel_{nullptr};
    std::vector<std::pair<QAction *, NexaIcons::Kind>> themedActions_;   // icons repainted for the scheme
    QString currentTool_;           // the tool running, for the hint beside the core
    QString statusMessage_;         // the status bar's message in full; the label shows it elided
    bool promptingForKey_{false};
    std::function<bool()> closeToTray_;
};

} // namespace nexa
