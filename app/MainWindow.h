#pragma once

#include "app/SystemStatus.h"
#include "ui/NexaCoreSpec.h"
#include "ui/NexaDesigns.h"
#include "ui/NexaIcons.h"
#include "ui/NexaTheme.h"
#include "ui/NexaThemes.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QAction;
class QBoxLayout;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;

namespace nexa {

class ActionLogDock;
class ActivityStream;
class AgentsRail;
class AppController;
class CoreCard;
class MissionBoard;
class ProcessStats;
class TaskListModel;
class TaskStrip;
class TaskTranscriptDrawer;
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
// live) and a one-line status bar in monospace. In the Mission Control design the middle is the
// control room instead: the agents rail, the core card with the metric tiles, the mission board (or
// a task's transcript), the live activity with its approval cards, and the top bar says the core is
// online, how long Nexa has run and how many slots are taken. Everything it shows comes from
// AppController's and the board's signals; nothing here decides.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppController *controller, ProviderRegistry *registry, ToolExecutor *executor, HttpClient *http,
               GlobalShortcuts *shortcuts, SystemStatus status, const QString &auditPath, QWidget *parent = nullptr);
    ~MainWindow() override;

    void openSettings(const QString &tab = {});
    // Opens Settings without blocking, saves a picture of each tab into `directory`, closes it.
    void snapshotSettings(const QString &directory, std::function<void()> done = {});
    // Wears a palette and a design for this run, without touching the settings (--theme, --design).
    void wear(const QString &palette, const QString &design);
    // The Hardcore switch: on asks first (and names the ways out), unless `ask` is false (--hardcore).
    void setHardcore(bool on, bool ask = true);
    // Asked when the window is closed: true means "hidden to the tray, keep running".
    void setCloseToTray(std::function<bool()> hide) { closeToTray_ = std::move(hide); }
    NexaCoreWidget *presence() const { return presence_; }    // the core beside the chat (the tray copies it)
    ThemeScheme scheme() const { return pack_.scheme; }
    const ThemePack &theme() const { return pack_; }
    const DesignSpec &design() const { return design_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildToolbar();
    void buildCentral();
    void buildStatusBar();
    void connectController();
    void connectVoice();
    QWidget *makePill(const QString &label, QWidget *value);
    QToolButton *makeIconButton(NexaIcons::Kind kind, const QString &objectName, QAction *action);
    void showCoreMode(CoreMode mode);
    void showHardcore();                    // the switch and the banner, from HardcoreMode's state
    // Responsive: Mission Control keeps as many columns as the width holds (the agents go first, then the live
    // activity), the toolbar drops its least needed pills before a button is hidden, the status bar its least needed
    // labels before the message is cut.
    static int missionColumnsFor(int width);
    void fitToWidth();
    void fitToolbar();
    void fitStatusBar();
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
    void buildMission();                // the Mission Control pieces, built once
    bool inMission() const { return design_.layout == WindowLayout::MissionControl; }
    void submitInput();                 // the input box's words as a task (in Mission Control, opened)
    void openTask(quint64 id);          // its transcript: in the board's place, or a window of its own
    void openAddTasks();
    void showMetrics();
    void showVoiceCard();
    void showMicLine(double level);
    void refreshMissionBar();
    void sampleProcesses();

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
    QPushButton *stopAllButton_{nullptr};
    QAction *themeAction_{nullptr};
    QAction *shareScreenAction_{nullptr};
    QAction *hardcoreAction_{nullptr};      // the Hardcore switch beside Share screen
    QLabel *hardcoreBanner_{nullptr};       // the countdown, the lock and its ways out, at the top of every design
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

    // Mission Control.
    TaskListModel *tasks_{nullptr};
    AgentsRail *rail_{nullptr};
    CoreCard *coreCard_{nullptr};
    MissionBoard *board_{nullptr};
    TaskTranscriptDrawer *drawer_{nullptr};
    QStackedWidget *centreStack_{nullptr};
    ActivityStream *stream_{nullptr};
    TaskStrip *taskStrip_{nullptr};
    QLabel *onlineChip_{nullptr};
    QLabel *uptimeLabel_{nullptr};
    QLabel *slotsLabel_{nullptr};
    QList<QAction *> missionBar_;       // the top bar's pills of Mission Control
    QList<QAction *> chatBar_;          // the toolbar buttons only the chat designs use
    QToolBar *toolbar_{nullptr};
    QAction *uptimeAction_{nullptr};    // the first pills to go when the toolbar runs out of room
    QAction *slotsAction_{nullptr};
    QAction *refreshAction_{nullptr};
    QAction *phonePanelAction_{nullptr};   // the toolbar's Phone panel button (Ctrl+P works without it)
    bool compactBar_{false};            // short words on the chip, the policy and Stop all: a narrow window
    int missionColumns_{3};             // agents | centre | activity; 2 without the agents; 1, the centre alone
    QTimer fitTimer_;                   // the arrangement follows a resize once it settles
    QElapsedTimer uptime_;
    QTimer clock_;                      // the uptime, once a second; the CPU and memory every two
    std::unique_ptr<ProcessStats> processStats_;
    int processTick_{0};
    double micLevel_{0};
};

} // namespace nexa
