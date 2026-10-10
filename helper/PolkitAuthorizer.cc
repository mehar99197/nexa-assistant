#include "helper/PolkitAuthorizer.h"

#include "core/Logging.h"

#include <polkitqt1-authority.h>
#include <polkitqt1-subject.h>

using namespace Qt::StringLiterals;
using PolkitQt1::Authority;

namespace nexa {

PolkitAuthorizer::PolkitAuthorizer(QObject *parent) : QObject(parent) {
    connect(Authority::instance(), &Authority::checkAuthorizationFinished, this, [this](Authority::Result result) {
        if (!busy_) return;
        switch (result) {
        case Authority::Yes:
            finish(true, {});
            return;
        case Authority::No:
            finish(false, tr("polkit refused this action."));
            return;
        case Authority::Challenge:
            // auth_admin_keep never ends here: polkit resolves the password prompt itself and
            // answers Yes or No. A Challenge means no authentication agent was running.
            finish(false, tr("No authentication agent is running, so the password could not be asked for."));
            return;
        case Authority::Unknown:
            break;
        }
        finish(false, tr("polkit gave no answer."));
    });
}

void PolkitAuthorizer::check(const QString &actionId, const QString &callerBusName, Answer answer) {
    queue_.push_back(Request{actionId, callerBusName, std::move(answer)});
    if (!busy_) startNext();
}

void PolkitAuthorizer::startNext() {
    if (busy_ || queue_.empty()) return;
    const Request &request = queue_.front();
    Authority *authority = Authority::instance();
    authority->clearError();
    busy_ = true;
    // The caller's unique bus name, as the bus reported it on the message — never an identity the
    // caller put in the message body.
    //
    // Not the login session, though that would look tidier: polkit 127 *aborts* when asked about a
    // unix-session subject, and every polkitd crash throws away the authorisations it was
    // remembering. The helper remembers the answer against the login itself instead.
    const PolkitQt1::SystemBusNameSubject subject(request.callerBusName);
    qCDebug(lcApp).noquote() << "asking polkit about" << request.actionId << "for" << request.callerBusName;

    // AllowUserInteraction: polkit may show the desktop's password prompt. The call can therefore
    // take as long as the user does, which is why every privileged method replies late.
    authority->checkAuthorization(request.actionId, subject, Authority::AllowUserInteraction);
    if (authority->hasError()) {
        const QString details = authority->errorDetails();
        authority->clearError();
        finish(false, tr("polkit could not be reached: %1").arg(details));
    }
}

void PolkitAuthorizer::revoke(const QString &callerBusName) {
    Authority *authority = Authority::instance();
    authority->clearError();
    const PolkitQt1::SystemBusNameSubject subject(callerBusName);
    if (!authority->revokeTemporaryAuthorizationsSync(subject)) {
        // Nothing to revoke is the common case and not a problem: polkit forgets a process-scoped
        // grant on its own as soon as that process is gone.
        qCDebug(lcApp).noquote() << "polkit had nothing to revoke for" << callerBusName;
    }
    authority->clearError();
}

void PolkitAuthorizer::finish(bool allowed, const QString &reason) {
    if (queue_.empty()) {
        busy_ = false;
        return;
    }
    Request request = std::move(queue_.front());
    queue_.pop_front();
    busy_ = false;
    // Every decision is journalled, so there is a record of what was allowed and what was not.
    qCInfo(lcApp).noquote() << (allowed ? "allowed" : "refused") << request.actionId << "for"
                            << request.callerBusName << (allowed ? QString() : u"— "_s + reason);
    request.answer(allowed, reason);
    startNext();
}

} // namespace nexa
