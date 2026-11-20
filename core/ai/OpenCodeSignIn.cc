#include "core/ai/OpenCodeSignIn.h"

#include "core/Logging.h"
#include "core/ai/OpenCodeServer.h"
#include "core/net/HttpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kRequestMs = 15'000;
constexpr int kAuthorizeMs = 30'000;
constexpr int kCodeMs = 60'000;
// opencode answers GET /mcp once every MCP server of the configuration has connected: 7 s to over 25 s measured.
constexpr int kMcpListMs = 100'000;
// opencode's own browser flows give up on their side after 5 minutes (ChatGPT's listener: 300 s, read from its
// source): the held request ends by itself then, and this is only the backstop for a flow that never does.
constexpr int kBrowserMs = 6 * 60 * 1000;

bool offered(const QString &providerId) {
    for (const OpenCodeSignIn::Provider &provider : OpenCodeSignIn::providers()) {
        if (provider.id == providerId) return true;
    }
    return false;
}

QString nameOf(const QString &providerId) {
    for (const OpenCodeSignIn::Provider &provider : OpenCodeSignIn::providers()) {
        if (provider.id == providerId) return provider.name;
    }
    return providerId;
}

// opencode's own wording, with the provider's name in front unless it already says it.
QString labelFor(const OpenCodeSignIn::Provider &provider, const OpenCodeAuthMethod &entry) {
    if (entry.label.isEmpty()) return provider.name;
    if (entry.label.contains(provider.name, Qt::CaseInsensitive)) return entry.label;
    return provider.name + u" · "_s + entry.label;
}

QString providerPath(const QString &providerId, const QString &tail) {
    return u"/provider/%1/%2"_s.arg(QString::fromLatin1(QUrl::toPercentEncoding(providerId)), tail);
}

} // namespace

// The sign-ins opencode provides by itself, read on 2026-10-31 from GET /provider/auth of opencode 1.19.3's
// `opencode serve`, started with empty XDG folders (no user configuration, so no plugin). Its OAuth entries, by
// index in the provider's list; `*` = opencode asks questions first, which Nexa does not support yet:
//   openai            [0] ChatGPT Pro/Plus (browser)   [1] ChatGPT Pro/Plus (headless)       ([2] API key)
//   github-copilot    [0*] Login with GitHub Copilot (deploymentType, enterpriseUrl)
//   gitlab            [0*] GitLab OAuth (instanceUrl)                                         ([1] access token)
//   poe               [0] Login with Poe (browser)                                            ([1] API key)
//   digitalocean      [0] Login with DigitalOcean                                             ([1] access key)
//   snowflake-cortex  [0*] Login with Snowflake (External Browser) (account, role)            ([1] token)
//   azure             [1*] Microsoft Entra ID (Azure CLI) (resourceName)                      ([0] API key)
//   xai               [0] SuperGrok Subscription                                              ([1] API key)
// The clean list also holds cloudflare-workers-ai and cloudflare-ai-gateway, which take only API keys. A user's
// plugins add more (the owner's opencode-antigravity-auth adds `google`): those are not opencode's own and are
// ignored. A provider is listed by id; its entries' labels may change between opencode versions.
const QList<OpenCodeSignIn::Provider> &OpenCodeSignIn::providers() {
    static const QList<Provider> allowed{
        {u"openai"_s, u"OpenAI"_s},
        {u"github-copilot"_s, u"GitHub Copilot"_s},
        {u"gitlab"_s, u"GitLab"_s},
        {u"poe"_s, u"Poe"_s},
        {u"digitalocean"_s, u"DigitalOcean"_s},
        {u"snowflake-cortex"_s, u"Snowflake Cortex"_s},
        {u"azure"_s, u"Azure"_s},
        {u"xai"_s, u"xAI"_s},
    };
    return allowed;
}

QList<SignInMethod> OpenCodeSignIn::offeredMethods(const QList<OpenCodeAuthMethod> &listed) {
    QList<SignInMethod> methods;
    for (const Provider &provider : providers()) {
        for (const OpenCodeAuthMethod &entry : listed) {
            if (entry.providerId != provider.id || !entry.oauth || entry.hasPrompts) continue;
            methods.append(SignInMethod{.id = u"%1#%2"_s.arg(provider.id).arg(entry.index),
                                        .label = labelFor(provider, entry),
                                        .needsCode = false});    // learnt at authorize: see pendingNeedsCode()
        }
    }
    return methods;
}

bool OpenCodeSignIn::parseMethodId(const QString &id, QString *providerId, int *index) {
    static const QRegularExpression form(u"\\A([a-z0-9][a-z0-9._-]*)#([0-9]{1,3})\\z"_s);
    const QRegularExpressionMatch match = form.match(id);
    if (!match.hasMatch()) return false;
    *providerId = match.captured(1);
    *index = match.captured(2).toInt();
    return true;
}

SignInStatus OpenCodeSignIn::statusFor(const QStringList &connected) {
    QStringList names;
    for (const Provider &provider : providers()) {
        if (connected.contains(provider.id)) names.append(provider.name);
    }
    if (names.isEmpty()) return SignInStatus{SignInStatus::State::SignedOut, {}};
    return SignInStatus{SignInStatus::State::SignedIn, names.join(u", "_s)};
}

OpenCodeSignIn::OpenCodeSignIn(std::shared_ptr<OpenCodeServer> server, QObject *parent)
    : ISignIn(parent), server_(std::move(server)) {}

OpenCodeSignIn::~OpenCodeSignIn() {
    end();
}

// ---- the state machine: one operation at a time, and whatever a cancelled one still has on its way is ignored ----

quint64 OpenCodeSignIn::begin(Op op) {
    if (op_ != Op::Idle) {
        emit failed(Error{ErrorCode::InvalidArgument, u"opencode's sign-in is still busy with the previous step"_s});
        return 0;
    }
    op_ = op;
    return ++serial_;
}

void OpenCodeSignIn::end() {
    op_ = Op::Idle;
    ++serial_;                                  // every callback of the operation is now stale
    needsCode_ = false;
    pendingProvider_.clear();
    pendingIndex_ = 0;
    if (reply_) {
        QNetworkReply *reply = reply_.data();
        reply_.clear();
        reply->abort();                         // a held callback ends; its handler sees the stale serial
        reply->deleteLater();
    }
}

void OpenCodeSignIn::fail(const Error &error) {
    const bool checking = op_ == Op::Checking;
    end();
    qCInfo(lcAi) << "opencode sign-in: failed:" << int(error.code);
    if (checking && error.code != ErrorCode::Cancelled) emit statusChecked(SignInStatus{});   // Unknown
    emit failed(error);
}

void OpenCodeSignIn::cancel() {
    if (op_ == Op::Idle) return;
    fail(Error{ErrorCode::Cancelled, u"Sign-in cancelled"_s});
}

// ---- requests: each reports to its operation only while that is still the current one ----

void OpenCodeSignIn::whenStarted(quint64 serial, std::function<void(Result<void>)> next) {
    server_->ensureStarted([self = QPointer<OpenCodeSignIn>(this), serial, next = std::move(next)](Result<void> started) {
        if (!self || !self->current(serial)) return;
        next(std::move(started));
    });
}

void OpenCodeSignIn::get(quint64 serial, const QString &path, std::function<void(Result<QByteArray>)> next) {
    reply_ = server_->http()->get(server_->request(path), kRequestMs, this,
                                  [this, serial, next = std::move(next)](Result<QByteArray> body) {
        if (!current(serial)) return;
        next(std::move(body));
    });
}

void OpenCodeSignIn::remove(quint64 serial, const QString &path, std::function<void(Result<QByteArray>)> next) {
    reply_ = server_->http()->deleteResource(server_->request(path), kRequestMs, this,
                                             [this, serial, next = std::move(next)](Result<QByteArray> body) {
        if (!current(serial)) return;
        next(std::move(body));
    });
}

// A POST whose HTTP error body is read: opencode names what went wrong there ({name, data}), and the shared
// client's buffered calls keep only the status. `idleTimeoutMs` ends a request nothing arrives on for that long
// — for the held callback of a browser flow, that is the whole wait.
void OpenCodeSignIn::post(quint64 serial, const QString &path, const QByteArray &body, int idleTimeoutMs,
                          std::function<void(Reply)> next) {
    QNetworkReply *reply = server_->http()->postStreaming(server_->request(path), body, idleTimeoutMs);
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, serial, reply, next = std::move(next)] {
        reply->deleteLater();
        if (!current(serial)) return;
        Reply answer;
        answer.status = HttpClient::httpStatus(reply);
        answer.body = reply->readAll();
        if (answer.status == 0) {
            const QNetworkReply::NetworkError code = reply->error();
            if (code == QNetworkReply::OperationCanceledError || code == QNetworkReply::TimeoutError) {
                answer.transport = Error{ErrorCode::Timeout, u"opencode did not answer in time"_s};
            } else {
                answer.transport = HttpClient::errorFor(reply, answer.body);
            }
        }
        next(std::move(answer));
    });
}

// opencode lists the providers it holds a login for, per running instance: the cheap list Nexa's model
// list reads too (GET /provider's is 7 MB and takes 2 s for the same names).
void OpenCodeSignIn::fetchConnected(quint64 serial, std::function<void(Result<QStringList>)> next) {
    get(serial, u"/config/providers"_s, [next = std::move(next)](Result<QByteArray> body) {
        if (!body) {
            next(body.error());
            return;
        }
        next(OpenCodeWire::parseConnected(body.value()));
    });
}

// opencode caches its providers per instance, so a new or removed login only shows once the instance is
// disposed — what its own TUI does after a sign-in. Best effort: the answer is read either way. The new instance
// connects the user's MCP servers again, which Nexa's own server must not have: they are left out once more.
void OpenCodeSignIn::disposeThenCheck(quint64 serial, std::function<void(Result<QStringList>)> next) {
    reply_ = server_->http()->post(server_->request(u"/instance/dispose"_s), "{}", kRequestMs, this,
                                   [this, serial, next = std::move(next)](Result<QByteArray> disposed) {
        if (!current(serial)) return;
        if (!disposed) qCWarning(lcAi) << "opencode sign-in: could not refresh opencode's providers:" << disposed.error().message;
        leaveMcpOut(serial, [this, serial, next] { fetchConnected(serial, next); });
    });
}

// After a dispose opencode builds its instance anew and connects the MCP servers of the user's configuration —
// measured on 1.19.3: /mcp shows them connected again. OpenCodeServer leaves them out of the server Nexa starts,
// because opencode offers their resources (mail, drive …) to the model whatever a prompt's tools say, and does it
// once, at the start. So it is done again here, the same way: by name from the configuration, which answers at
// once, and a look at /mcp, which waits for every server, for any that connected late. A server the user runs
// (no process of Nexa's) is theirs to configure.
void OpenCodeSignIn::leaveMcpOut(quint64 serial, std::function<void()> next) {
    if (server_->processId() == 0) {
        next();
        return;
    }
    get(serial, u"/config"_s, [this, serial, next = std::move(next)](Result<QByteArray> body) {
        if (!body) qCWarning(lcAi) << "opencode sign-in: could not read opencode's configuration:" << body.error().message;
        const QStringList names =
            body ? QJsonDocument::fromJson(body.value()).object().value(u"mcp"_s).toObject().keys() : QStringList();
        disconnectMcp(names, [this, serial, next] {
            // Late ones: not waited for, and not tied to the operation (it may be over by then).
            server_->http()->get(server_->request(u"/mcp"_s), kMcpListMs, this, [this](Result<QByteArray> listed) {
                if (!listed || server_->processId() == 0) return;
                QStringList connected;
                const QJsonObject servers = QJsonDocument::fromJson(listed.value()).object();
                for (auto it = servers.begin(); it != servers.end(); ++it) {
                    if (it.value().toObject().value(u"status"_s).toString() == u"connected") connected.append(it.key());
                }
                if (!connected.isEmpty()) disconnectMcp(connected, [] {});
            });
            if (current(serial)) next();
        });
    });
}

void OpenCodeSignIn::disconnectMcp(const QStringList &names, std::function<void()> done) {
    if (names.isEmpty()) {
        done();
        return;
    }
    qCInfo(lcAi) << "opencode sign-in: leaving out of Nexa's opencode again:" << names.size() << "MCP servers of the user's";
    auto left = std::make_shared<qsizetype>(names.size());
    for (const QString &name : names) {
        server_->http()->post(server_->request(u"/mcp/%1/disconnect"_s.arg(QString::fromLatin1(QUrl::toPercentEncoding(name)))),
                              "{}", kRequestMs, this, [left, done](Result<QByteArray>) {
            if (--*left == 0) done();
        });
    }
}

// ---- the operations ----

void OpenCodeSignIn::listMethods() {
    const quint64 serial = begin(Op::Listing);
    if (!serial) return;
    whenStarted(serial, [this, serial](Result<void> started) {
        if (!started) {
            fail(started.error());
            return;
        }
        get(serial, u"/provider/auth"_s, [this](Result<QByteArray> body) {
            if (!body) {
                fail(body.error());
                return;
            }
            const Result<QList<OpenCodeAuthMethod>> listed = OpenCodeWire::parseAuthMethods(body.value());
            if (!listed) {
                fail(listed.error());
                return;
            }
            const QList<SignInMethod> methods = offeredMethods(listed.value());
            end();
            emit methodsListed(methods);
        });
    });
}

void OpenCodeSignIn::checkStatus() {
    const quint64 serial = begin(Op::Checking);
    if (!serial) return;
    whenStarted(serial, [this, serial](Result<void> started) {
        if (!started) {
            fail(started.error());
            return;
        }
        fetchConnected(serial, [this](Result<QStringList> connected) {
            if (!connected) {
                fail(connected.error());
                return;
            }
            const SignInStatus status = statusFor(connected.value());
            end();
            emit statusChecked(status);
        });
    });
}

void OpenCodeSignIn::signIn(const QString &methodId) {
    const quint64 serial = begin(Op::Authorizing);
    if (!serial) return;
    QString providerId;
    int index = 0;
    if (!parseMethodId(methodId, &providerId, &index) || !offered(providerId)) {
        fail(Error{ErrorCode::InvalidArgument, methodId.isEmpty() ? u"Choose which provider to sign in to."_s
                                                                  : u"opencode has no such sign-in to offer."_s});
        return;
    }
    qCInfo(lcAi) << "opencode sign-in: starting" << providerId << "method" << index;
    whenStarted(serial, [this, serial, providerId, index](Result<void> started) {
        if (!started) {
            fail(started.error());
            return;
        }
        // The entry is looked at again, not taken from an earlier list: opencode may have changed, and only an
        // OAuth entry that asks nothing is something Nexa sends to authorize.
        get(serial, u"/provider/auth"_s, [this, serial, providerId, index](Result<QByteArray> body) {
            if (!body) {
                fail(body.error());
                return;
            }
            const Result<QList<OpenCodeAuthMethod>> listed = OpenCodeWire::parseAuthMethods(body.value());
            if (!listed) {
                fail(listed.error());
                return;
            }
            const OpenCodeAuthMethod *entry = nullptr;
            for (const OpenCodeAuthMethod &candidate : listed.value()) {
                if (candidate.providerId == providerId && candidate.index == index) entry = &candidate;
            }
            if (!entry) {
                fail(Error{ErrorCode::NotFound, u"opencode has no such sign-in (any more)."_s});
            } else if (!entry->oauth) {
                fail(Error{ErrorCode::NotSupported, u"That is not a browser sign-in."_s});
            } else if (entry->hasPrompts) {
                fail(Error{ErrorCode::NotSupported, u"That sign-in asks for details first, which Nexa does not do yet: "
                                                    "run `opencode auth login` in a terminal."_s});
            } else {
                authorize(serial, providerId, index);
            }
        });
    });
}

void OpenCodeSignIn::authorize(quint64 serial, const QString &providerId, int index) {
    post(serial, providerPath(providerId, u"oauth/authorize"_s), OpenCodeWire::authorizeBody(index), kAuthorizeMs,
         [this, serial, providerId, index](Reply reply) {
        if (reply.transport) {
            fail(*reply.transport);
            return;
        }
        if (reply.status >= 400) {
            fail(OpenCodeWire::authFailure(reply.status, reply.body, OpenCodeAuthStep::Authorize));
            return;
        }
        const Result<OpenCodeAuthorization> granted = OpenCodeWire::parseAuthorization(reply.body);
        if (!granted) {
            fail(granted.error());
            return;
        }
        QString words = granted.value().instructions;
        needsCode_ = granted.value().needsCode;
        if (needsCode_) {
            if (!words.isEmpty()) words += u'\n';
            words += u"When the page shows you a code, paste it into the code field."_s;
            op_ = Op::WaitingForCode;
            pendingProvider_ = providerId;
            pendingIndex_ = index;
        } else {
            op_ = Op::WaitingForBrowser;
        }
        qCInfo(lcAi) << "opencode sign-in: waiting for" << providerId << (needsCode_ ? "(a code)" : "(the browser)");
        // The caller opens the link: opencode opens nothing, and the link and the words are not logged.
        emit linkReady(granted.value().url, words, false);
        if (!current(serial) || needsCode_) return;     // cancelled by whoever listened, or now waiting for a code
        waitForBrowser(serial, providerId, index);
    });
}

// "auto": opencode finishes the flow itself (a listener on loopback for the redirect, or polling for a device
// code) and answers this request when it did — or when it gave up.
void OpenCodeSignIn::waitForBrowser(quint64 serial, const QString &providerId, int index) {
    post(serial, providerPath(providerId, u"oauth/callback"_s), OpenCodeWire::callbackBody(index), kBrowserMs,
         [this, serial, providerId](Reply reply) {
        if (reply.transport) {
            fail(reply.transport->code == ErrorCode::Timeout
                     ? Error{ErrorCode::Timeout, u"The browser sign-in did not finish in time. Try again."_s}
                     : *reply.transport);
            return;
        }
        if (reply.status >= 400) {
            fail(OpenCodeWire::authFailure(reply.status, reply.body, OpenCodeAuthStep::WaitForBrowser));
            return;
        }
        if (!OpenCodeWire::isTrue(reply.body)) {
            fail(Error{ErrorCode::Parse, u"opencode did not confirm the sign-in"_s});
            return;
        }
        finishSignIn(serial, providerId);
    });
}

void OpenCodeSignIn::submitCode(const QString &code) {
    // A refused call never touches the operation that is running.
    if (op_ != Op::WaitingForCode) {
        emit failed(Error{ErrorCode::InvalidArgument, u"No sign-in is waiting for a code."_s});
        return;
    }
    const QString trimmed = code.trimmed();         // what a paste brings along
    if (trimmed.isEmpty()) {
        emit failed(Error{ErrorCode::InvalidArgument, u"Paste the code the browser page shows."_s});
        return;
    }
    op_ = Op::SubmittingCode;
    const quint64 serial = serial_;
    const QString providerId = pendingProvider_;
    post(serial, providerPath(providerId, u"oauth/callback"_s), OpenCodeWire::callbackBody(pendingIndex_, trimmed),
         kCodeMs, [this, serial, providerId](Reply reply) {
        if (reply.transport) {
            fail(*reply.transport);
            return;
        }
        if (reply.status >= 400) {
            fail(OpenCodeWire::authFailure(reply.status, reply.body, OpenCodeAuthStep::SubmitCode));
            return;
        }
        if (!OpenCodeWire::isTrue(reply.body)) {
            fail(Error{ErrorCode::Parse, u"opencode did not confirm the sign-in"_s});
            return;
        }
        finishSignIn(serial, providerId);
    });
}

void OpenCodeSignIn::finishSignIn(quint64 serial, const QString &providerId) {
    op_ = Op::Refreshing;
    disposeThenCheck(serial, [this, providerId](Result<QStringList> connected) {
        QStringList names = connected ? connected.value() : QStringList();
        if (!names.contains(providerId)) names.append(providerId);      // opencode said true: it is signed in
        const SignInStatus status = statusFor(names);
        qCInfo(lcAi) << "opencode sign-in: signed in to" << providerId;
        end();
        emit signedIn(status);
    });
}

void OpenCodeSignIn::signOut() {
    const quint64 serial = begin(Op::SigningOut);
    if (!serial) return;
    whenStarted(serial, [this, serial](Result<void> started) {
        if (!started) {
            fail(started.error());
            return;
        }
        fetchConnected(serial, [this, serial](Result<QStringList> connected) {
            if (!connected) {
                fail(connected.error());
                return;
            }
            QStringList targets;
            for (const Provider &provider : providers()) {
                if (connected.value().contains(provider.id)) targets.append(provider.id);
            }
            if (targets.isEmpty()) {
                end();
                emit signedOut();
                return;
            }
            removeLogins(serial, targets, 0);
        });
    });
}

// DELETE /auth/<id> for each, then the instance is disposed and the list read again: a login that stays (set in
// the environment or in opencode's own configuration) is not one Nexa can remove, and is not called signed out.
void OpenCodeSignIn::removeLogins(quint64 serial, QStringList targets, qsizetype at) {
    if (at == targets.size()) {
        disposeThenCheck(serial, [this, targets](Result<QStringList> connected) {
            if (!connected) {
                fail(connected.error());
                return;
            }
            QStringList staying;
            for (const QString &id : targets) {
                if (connected.value().contains(id)) staying.append(nameOf(id));
            }
            if (!staying.isEmpty()) {
                fail(Error{ErrorCode::Unknown, u"%1 stays connected after its login was removed: opencode finds it in an "
                                               "environment variable or in its own configuration, which Nexa leaves "
                                               "alone."_s.arg(staying.join(u", "_s))});
                return;
            }
            end();
            emit signedOut();
        });
        return;
    }
    qCInfo(lcAi) << "opencode sign-in: removing the login of" << targets.at(at);
    remove(serial, u"/auth/%1"_s.arg(QString::fromLatin1(QUrl::toPercentEncoding(targets.at(at)))),
           [this, serial, targets, at](Result<QByteArray> removed) {
        if (!removed) {
            // Whatever was removed already should show as gone: refresh, then report.
            const Error error = removed.error();
            disposeThenCheck(serial, [this, error](Result<QStringList>) { fail(error); });
            return;
        }
        removeLogins(serial, targets, at + 1);
    });
}

} // namespace nexa
