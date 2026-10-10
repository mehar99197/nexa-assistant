#pragma once

#include "ui/NexaCoreSpec.h"
#include "ui/NexaTheme.h"

#include <QString>
#include <QWidget>

class QBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QToolButton;

namespace nexa {

class NexaCoreButton;
class NexaCoreWidget;

// The conversation, as the PC and every paired phone see it: the Nexa core with the state's name
// (large while the conversation is empty, a row at the top once it is not), the messages, a line
// to type in, and the core again as the Talk button. Text from Nexa is shown as text, never
// interpreted. It shows what it is told; PhoneWindow decides.
class ChatPage : public QWidget {
    Q_OBJECT
public:
    enum class Kind { User, FromPc, Nexa, Step, StepFailed, Heard, Error };
    enum class Status { Ok, Warn, Recording, Plain };

    explicit ChatPage(QWidget *parent = nullptr);

    void setScheme(ThemeScheme scheme);
    void setStatus(const QString &text, Status status);
    // The core's state, the satellites lit, and the words under the state's name.
    void setCore(CoreMode mode, CoreNodes nodes, const QString &hint);
    void setLevel(double level);
    void addMessage(Kind kind, const QString &text);
    void appendReply(const QString &text);      // more of the reply being written
    void endReply();
    void setBusy(bool busy);
    void setVoiceAvailable(bool available);
    void setRecording(bool recording);
    void clear();

signals:
    void askRequested(const QString &text);
    void cancelRequested();
    void talkPressed();
    void talkReleased();
    void forgetRequested();

private:
    void submit();
    void layoutPresence();
    void refreshIcons();

    ThemeTokens tokens_;
    QLabel *status_{nullptr};
    NexaCoreWidget *brandCore_{nullptr};
    QWidget *presence_{nullptr};
    QBoxLayout *presenceLayout_{nullptr};
    NexaCoreWidget *core_{nullptr};
    QWidget *presenceText_{nullptr};
    QLabel *title_{nullptr};
    QLabel *hint_{nullptr};
    QListWidget *messages_{nullptr};
    QLineEdit *input_{nullptr};
    QToolButton *send_{nullptr};
    QToolButton *more_{nullptr};
    QWidget *talkRow_{nullptr};
    NexaCoreButton *talk_{nullptr};
    QLabel *talkHint_{nullptr};
    QListWidgetItem *reply_{nullptr};
    CoreMode mode_{CoreMode::Idle};
    bool busy_{false};
    bool compact_{false};
};

} // namespace nexa
