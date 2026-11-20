#pragma once

#include "core/Result.h"
#include "core/ai/ChatRequest.h"
#include "core/ai/ClaudeCodeEvents.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace nexa {

// One warm Claude Code process (`claude -p --input-format stream-json --output-format stream-json`), which keeps the
// conversation between turns, so a turn costs the model's time and not the program's start. One turn at a time; the
// process lives in its own process group, in an empty folder of Nexa's own (`<dir>/work`), and goes after ten idle
// minutes. A session ends with its process: a new one is a new session. Nexa's system prompt and the model are start
// flags (`--system-prompt`, `--model`), so a session is for one of each.
//
// It is the unmodified binary as published, on the owner's own Claude Code sign-in (Anthropic's terms let an end user
// sign in to the unmodified Claude Code with their own subscription; a third-party app may not offer the login itself
// or touch the credentials): nothing of its configuration is pointed elsewhere — no CLAUDE_CONFIG_DIR of Nexa's, no
// `--bare` — so the sign-in is the one the owner's terminal has, and Nexa never reads it. Claude Code's own tools are
// off (`--tools ""`, `--strict-mcp-config`, `--setting-sources ""`, `--disable-slash-commands`), nothing can prompt
// (`--permission-mode dontAsk --permission-prompts none`), nothing is saved (`--no-session-persistence`). CLAUDECODE
// and CLAUDE_CODE_ENTRYPOINT are removed from its environment: Nexa may itself have been started from a Claude Code
// terminal, and a nested run can be refused.
class ClaudeCodeSession : public QObject {
    Q_OBJECT
public:
    ClaudeCodeSession(QString program, QString model, QString systemPrompt, QString workingDir,
                      QObject *parent = nullptr);
    ~ClaudeCodeSession() override;

    void start();                       // idempotent
    void send(const QString &text, const QList<ImageAttachment> &images = {});  // one turn
    void stop();
    bool isUsable() const { return !broken_; }
    bool isBusy() const { return busy_; }
    int turnsTaken() const { return turns_; }
    const QString &model() const { return model_; }
    const QString &systemPrompt() const { return systemPrompt_; }

    // The command line after the program (pure): the flags above, `--model` only when one is named and
    // `--system-prompt` only when there is one.
    static QStringList arguments(const QString &model, const QString &systemPrompt);
    // The environment Claude Code runs in: Nexa's own with the program's folder first on the PATH (an npm install's
    // node beside it), without the two variables that mark a Claude Code terminal. Never a config folder of Nexa's.
    static QProcessEnvironment prepare(const QString &program);
    // `<dir>/work`, made (owner-only) if need be.
    static QString workspace(const QString &dir);

signals:
    void activity();                    // Claude Code printed something during a turn (thinking counts)
    void text(const QString &delta);
    void turnFinished(const nexa::ClaudeCodeEvent &result);
    void turnFailed(const nexa::Error &error);

private:
    void readEvents();
    void ended(const Error &error);

    QString program_;
    QString model_;
    QString systemPrompt_;
    QString workingDir_;
    QProcess process_;
    QTimer idle_;
    QByteArray buffer_;
    QString stderrTail_;
    QString apiError_;                  // the API error named before the result of the turn on its way
    bool started_{false};
    bool broken_{false};
    bool busy_{false};
    int turns_{0};
};

} // namespace nexa
