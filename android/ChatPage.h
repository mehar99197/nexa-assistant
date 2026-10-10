#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace nexa {

// The conversation, as the PC and every paired phone see it, with a line to type in and a button
// to hold while talking. Text from Nexa is shown as text, never interpreted.
class ChatPage : public QWidget {
    Q_OBJECT
public:
    enum class Kind { User, FromPc, Nexa, Step, StepFailed, Heard, Error };
    enum class Status { Ok, Warn, Recording, Plain };

    explicit ChatPage(QWidget *parent = nullptr);

    void setStatus(const QString &text, Status status);
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

    QLabel *status_{nullptr};
    QListWidget *messages_{nullptr};
    QLineEdit *input_{nullptr};
    QPushButton *send_{nullptr};
    QPushButton *talk_{nullptr};
    QListWidgetItem *reply_{nullptr};
    bool busy_{false};
};

} // namespace nexa
