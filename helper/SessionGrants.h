#pragma once

#include "helper/LoginSession.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace nexa {

// Repository: the logins that have already been authorised, so the password is asked for once per
// login instead of once per calling process.
//
// Why this exists at all. The policy file asks polkit for auth_admin_keep, which is meant to mean
// "ask once, then remember". polkit does remember — but against the *process* that asked. Nexa is
// one process among many short-lived ones (every `--ask` run, every restart during a day's use is
// a new one), and each is a stranger to polkit, so the prompt came back again and again. polkit
// can be asked about a login session instead, which would have the right lifetime, but polkitd 127
// aborts when asked that, and a crash loses everything it was remembering. So the answer is kept
// here, against the login, which is the lifetime the policy file always promised.
//
// What is remembered is only *that* the user said yes, never the password: the password is typed
// into the desktop's own authentication agent and is never seen by Nexa or by the helper.
//
// The record lives in /run, which is a memory-backed filesystem wiped on every boot, owned by root
// and unreadable by anyone else. Keeping it in a file rather than in memory means an authorisation
// survives the helper exiting when idle — the ten-minute idle exit would otherwise bring the prompt
// straight back. It does not survive a logout: the login session id and its start time are both
// part of the record, so the next login does not inherit it.
class SessionGrants {
public:
    struct Grant {
        uint uid{0};
        QString sessionId;
        qulonglong startedMonotonicUsec{0};
        QString grantedAt;      // ISO 8601 UTC, for the record in the journal and nothing else

        // Identity only — whether this is the same login. Whether that login is still the one on
        // the screen is LoginSession::isAtTheScreen(), and the caller checks it every time.
        bool covers(const LoginSession &session) const;
    };

    // The default path is /run/nexa-helper/grants.json; tests pass their own.
    explicit SessionGrants();
    explicit SessionGrants(QString path);

    static QString defaultPath();

    bool has(const LoginSession &session) const;
    void add(const LoginSession &session);
    // Gives up the authorisation for one login. True when there was something to give up.
    bool forget(const LoginSession &session);

    // Pure, so the file format is tested without touching a disk. decode() drops any record it
    // cannot read in full rather than guessing at it.
    static QByteArray encode(const QList<Grant> &grants);
    static QList<Grant> decode(const QByteArray &json);

private:
    void load();
    void save() const;

    QString path_;
    QList<Grant> grants_;
};

} // namespace nexa
