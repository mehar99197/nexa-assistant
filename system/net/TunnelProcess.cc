#include "system/net/TunnelProcess.h"

#include "core/Logging.h"
#include "core/ProcessUtils.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

// 5 s, then 10, 20, 40, and every minute after that.
int pauseAfter(int failures) {
    return std::min(60'000, 5'000 << std::min(failures - 1, 4));
}

void endTunnelProcess(QProcess *process, bool forceNow) {
    if (!process || process->state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    Q_UNUSED(forceNow);
    process::killTree(process->processId());
    process->kill();
#else
    if (forceNow) {
        process::killTree(process->processId());
        process->kill();
    } else {
        process->terminate();
    }
#endif
}

} // namespace

TunnelProcess::TunnelProcess(QString name, QObject *parent) : QObject(parent), name_(std::move(name)) {
    restart_.setSingleShot(true);
    connect(&restart_, &QTimer::timeout, this, &TunnelProcess::launch);
}

TunnelProcess::~TunnelProcess() {
    stop(true);
}

void TunnelProcess::start(Spec spec) {
    const bool same = process_ && spec.program == spec_.program && spec.arguments == spec_.arguments;
    if (same && state_ != State::Failed) return;
    stop();
    spec_ = std::move(spec);
    failures_ = 0;
    fatal_ = false;
    fallback_ = false;
    problem_.clear();
    if (spec_.program.isEmpty()) {
#ifdef Q_OS_WIN
        problem_ = tr("%1 is not installed. Install it on PATH or place it in %LOCALAPPDATA%\\nexa\\tunnels.").arg(name_);
#else
        problem_ = tr("%1 is not installed: scripts/download-tunnels.sh puts it in ~/.local/share/nexa/tunnels.").arg(name_);
#endif
        qCInfo(lcApp).noquote() << "tunnel:" << problem_;
        setState(State::Missing);
        return;
    }
    launch();
}

void TunnelProcess::stop(bool wait) {
    restart_.stop();
    if (process_) {
        process_->disconnect(this);
        endTunnelProcess(process_.get(), false);
        if (wait) {
            if (!process_->waitForFinished(3'000)) {
                endTunnelProcess(process_.get(), true);
                process_->waitForFinished(1'000);
            }
            process_.reset();
        } else {
            // It ends by itself in a moment; a tunnel that does not listen is killed.
            QProcess *ending = process_.release();
            connect(ending, &QProcess::finished, ending, &QObject::deleteLater);
            QTimer::singleShot(3'000, ending, [ending] {
                if (ending->state() != QProcess::NotRunning) endTunnelProcess(ending, true);
            });
        }
    }
    url_.clear();
    setState(State::Off);
}

void TunnelProcess::launch() {
    process_ = std::make_unique<QProcess>();
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process::configureChild(*process_);
    connect(process_.get(), &QProcess::readyRead, this, [this] {
        pending_ += process_->readAll();
        qsizetype end = -1;
        while ((end = pending_.indexOf('\n')) >= 0) {
            onLine(QString::fromUtf8(pending_.left(end)).trimmed());
            pending_.remove(0, end + 1);
        }
    });
    connect(process_.get(), &QProcess::finished, this, &TunnelProcess::onFinished);
    connect(process_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            problem_ = tr("%1 could not be started: %2").arg(name_, process_->errorString());
            onFinished();
        }
    });
    url_ = spec_.fixedUrl;
    pending_.clear();
    setState(State::Starting);
    qCInfo(lcApp).noquote() << "tunnel: starting" << name_ << (fallback_ ? u"(its servers named directly)"_s : QString());
    process_->start(spec_.program, fallback_ ? spec_.fallbackArguments : spec_.arguments);
}

void TunnelProcess::onLine(const QString &line) {
    if (line.isEmpty() || !spec_.read) return;
    const tunnellog::LineEvent event = spec_.read(line);
    if (!event.url.isEmpty()) url_ = event.url;
    if (event.edgeLookupFailed && !fallback_ && !spec_.fallbackArguments.isEmpty()) {
        // It cannot find its servers on this network's DNS; started again at once, naming them.
        qCInfo(lcApp).noquote() << "tunnel:" << name_ << "cannot look its servers up on this network's DNS";
        fallback_ = true;
        stopping_ = true;
        endTunnelProcess(process_.get(), false);
        return;
    }
    if (!event.error.isEmpty()) {
        problem_ = event.error;
        if (event.fatal) {
            fatal_ = true;
            qCWarning(lcApp).noquote() << "tunnel:" << name_ << "cannot work:" << event.error;
            stopping_ = true;           // it is ended here; its exit is not a failure to retry
            endTunnelProcess(process_.get(), false);
        }
    }
    if (event.connected && !url_.isEmpty() && !fatal_) {
        problem_.clear();
        failures_ = 0;
        if (state_ != State::Running) qCInfo(lcApp).noquote() << "tunnel:" << name_ << "reachable at" << url_;
        setState(State::Running);
    }
}

void TunnelProcess::onFinished() {
    if (!process_) return;
    process_.release()->deleteLater();
    url_.clear();
    if (fatal_) {
        stopping_ = false;
        setState(State::Failed);
        return;
    }
    if (stopping_) {
        stopping_ = false;
        if (fallback_ && state_ == State::Starting) launch();        // the same tunnel, its servers named
        return;
    }
    ++failures_;
    if (problem_.isEmpty()) problem_ = tr("%1 stopped.").arg(name_);
    const int pause = pauseAfter(failures_);
    qCInfo(lcApp).noquote() << "tunnel:" << name_ << "stopped; trying again in" << pause / 1000 << "s";
    setState(State::Starting);
    restart_.start(pause);
}

void TunnelProcess::setState(State state) {
    if (state_ == state) {
        emit changed();     // what it says may have changed (its address, its problem)
        return;
    }
    state_ = state;
    emit changed();
}

} // namespace nexa
