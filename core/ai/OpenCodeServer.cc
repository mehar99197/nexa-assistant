#include "core/ai/OpenCodeServer.h"

#include "core/Logging.h"
#include "core/net/HttpClient.h"

#include <QDir>
#include <QFile>
#include <QNetworkReply>
#include <QPointer>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <signal.h>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kHealthPollMs = 250;
constexpr int kHealthTimeoutMs = 2'000;
constexpr int kEventsIdleMs = 60'000;      // opencode sends a heartbeat every 10 s
constexpr int kReopenEventsMs = 1'000;
constexpr qsizetype kStderrKept = 4'096;

constexpr auto kProcessFlags = QProcess::UnixProcessFlag::CreateNewSession
    | QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors;

// opencode's own permissions, denied as a second lock: every prompt switches its tools off anyway.
const QString kConfig = uR"({"permission":{"edit":"deny","bash":"deny","webfetch":"deny"}})"_s;

QString freshPassword() {
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), bytes.size() / 4);
    return QString::fromLatin1(bytes.toHex());
}

} // namespace

OpenCodeServer::OpenCodeServer(HttpClient *http, QString program, QUrl attachUrl, QString attachPassword,
                               QString workingDir, QObject *parent)
    : QObject(parent), http_(http), program_(std::move(program)), attachUrl_(std::move(attachUrl)),
      workingDir_(std::move(workingDir)), password_(std::move(attachPassword)) {
    if (workingDir_.isEmpty()) {
        workingDir_ = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/nexa/opencode"_s;
    }
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        stopProcess();
        finishStart(Error{ErrorCode::Timeout, u"opencode did not start within %1 s"_s.arg(startTimeoutMs_ / 1000)});
    });
    retry_.setSingleShot(true);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        output_ += process_.readAllStandardOutput();
        if (state_ != State::Starting || base_.isValid()) return;
        if (const QUrl url = OpenCodeWire::parseListening(output_); url.isValid()) {
            base_ = url;
            checkHealth();
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        if (stderrTail_.size() > kStderrKept) stderrTail_ = stderrTail_.right(kStderrKept);
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        const QStringList lines = stderrTail_.trimmed().split(u'\n', Qt::SkipEmptyParts);
        const Error error{ErrorCode::ConnectionRefused,
                          u"opencode stopped (exit %1)%2"_s.arg(exitCode).arg(
                              lines.isEmpty() ? QString() : u": "_s + lines.last().trimmed())};
        qCInfo(lcAi) << "opencode server ended:" << error.message;
        const State was = std::exchange(state_, State::Idle);
        base_.clear();
        if (events_) {
            QNetworkReply *events = std::exchange(events_, nullptr);
            events->disconnect(this);
            events->abort();
            events->deleteLater();
        }
        if (was == State::Starting) {
            finishStart(error);
        } else if (was == State::Ready) {
            emit stopped(error);
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || state_ != State::Starting) return;
        state_ = State::Idle;
        finishStart(Error{ErrorCode::NotFound, u"Cannot start opencode (%1): %2"_s.arg(program_, process_.errorString())});
    });
}

OpenCodeServer::~OpenCodeServer() {
    if (events_) {
        events_->disconnect(this);
        events_->abort();
        events_->deleteLater();
    }
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() == QProcess::NotRunning) return;
    const qint64 pid = process_.processId();
    if (::kill(pid_t(-pid), SIGKILL) != 0) ::kill(pid_t(pid), SIGKILL);
    process_.waitForFinished(2'000);
}

qint64 OpenCodeServer::processId() const {
    return process_.state() == QProcess::NotRunning ? 0 : process_.processId();
}

QNetworkRequest OpenCodeServer::request(const QString &path) const {
    return OpenCodeWire::request(base_, path, password_);
}

void OpenCodeServer::ensureStarted(std::function<void(Result<void>)> done) {
    if (state_ == State::Ready) {
        QTimer::singleShot(0, this, [done = std::move(done)] { done(Result<void>{}); });
        return;
    }
    waiting_.append(std::move(done));
    if (state_ == State::Starting) return;
    state_ = State::Starting;
    deadline_.start(startTimeoutMs_);
    if (attachUrl_.isValid()) {
        base_ = attachUrl_;
        checkHealth();
        return;
    }
    if (program_.isEmpty()) {
        state_ = State::Idle;
        QTimer::singleShot(0, this, [this] {
            finishStart(Error{ErrorCode::NotFound, u"opencode is not installed (looked on PATH and in ~/.local/bin)"_s});
        });
        return;
    }
    startProcess();
}

void OpenCodeServer::startProcess() {
    QDir().mkpath(workingDir_);
    QFile::setPermissions(workingDir_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    password_ = freshPassword();
    output_.clear();
    stderrTail_.clear();
    base_.clear();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"OPENCODE_SERVER_PASSWORD"_s, password_);
    environment.insert(u"OPENCODE_CONFIG_CONTENT"_s, kConfig);
    process_.setProcessEnvironment(environment);
    process_.setProgram(program_);
    process_.setArguments({u"serve"_s, u"--hostname"_s, u"127.0.0.1"_s, u"--port"_s, u"0"_s});
    process_.setWorkingDirectory(workingDir_);
    process_.setStandardInputFile(QProcess::nullDevice());    // `opencode run` would wait for stdin to end
    process_.setUnixProcessParameters(kProcessFlags);
    qCInfo(lcAi) << "starting opencode's server";
    process_.start();
}

void OpenCodeServer::checkHealth() {
    if (state_ != State::Starting) return;
    http_->get(request(u"/global/health"_s), kHealthTimeoutMs, this, [this](Result<QByteArray> body) {
        if (state_ != State::Starting) return;
        if (body) {
            openEvents();
        } else if (attachUrl_.isValid() && body.error().code == ErrorCode::ConnectionRefused) {
            // A server the user runs is either there or not: nothing of Nexa's is on its way up.
            finishStart(Error{ErrorCode::ConnectionRefused,
                              u"nothing answers at %1 — is your opencode server running?"_s.arg(base_.toDisplayString())});
        } else {
            QTimer::singleShot(kHealthPollMs, this, &OpenCodeServer::checkHealth);
        }
    });
}

void OpenCodeServer::openEvents() {
    parser_.reset();
    events_ = http_->getStreaming(request(u"/event"_s), kEventsIdleMs);
    QNetworkReply *events = events_;
    // The stream is open once its headers arrive: from then on no event of a new turn is missed.
    connect(events, &QNetworkReply::metaDataChanged, this, [this, events] {
        if (events != events_ || state_ != State::Starting) return;
        if (HttpClient::httpStatus(events) == 200) {
            state_ = State::Ready;
            deadline_.stop();
            qCInfo(lcAi) << "opencode's server is ready";
            finishStart(Result<void>{});
        }
    });
    connect(events, &QNetworkReply::readyRead, this, &OpenCodeServer::readEvents);
    connect(events, &QNetworkReply::finished, this, [this, events] {
        events->deleteLater();
        if (events != events_) return;
        events_ = nullptr;
        if (state_ == State::Starting) {
            QTimer::singleShot(kHealthPollMs, this, &OpenCodeServer::checkHealth);
        } else if (state_ == State::Ready && process_.state() == QProcess::Running) {
            QTimer::singleShot(kReopenEventsMs, this, [this] {
                if (state_ == State::Ready && !events_) openEvents();
            });
        } else if (state_ == State::Ready) {
            // A server the user runs went away; the next turn finds out whether it is back.
            state_ = State::Idle;
            emit stopped(Error{ErrorCode::ConnectionRefused,
                               u"opencode's server at %1 went away"_s.arg(base_.toDisplayString())});
        }
    });
}

void OpenCodeServer::readEvents() {
    if (!events_) return;
    for (const SseEvent &sse : parser_.feed(events_->readAll())) emit event(OpenCodeWire::decodeEvent(sse.data));
}

void OpenCodeServer::finishStart(const Result<void> &result) {
    deadline_.stop();
    if (!result && state_ == State::Starting) state_ = State::Idle;
    const QList<std::function<void(Result<void>)>> waiting = std::exchange(waiting_, {});
    for (const auto &done : waiting) done(result);
}

void OpenCodeServer::stopProcess() {
    state_ = State::Idle;
    if (process_.state() == QProcess::NotRunning) return;
    const qint64 pid = process_.processId();
    if (::kill(pid_t(-pid), SIGKILL) != 0) ::kill(pid_t(pid), SIGKILL);
}

} // namespace nexa
