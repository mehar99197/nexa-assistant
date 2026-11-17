#include "system/commands/RootCommandRunner.h"

#include "core/Logging.h"
#include "core/ProcessUtils.h"
#include "proto/HelperTypes.h"
#include "system/helper/HelperClient.h"

#ifdef Q_OS_WIN
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QThreadPool>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <utility>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

#ifndef Q_OS_WIN
// The same quiet, non-interactive environment the unprivileged runner uses, so output is text
// rather than a pager full of escape codes.
QStringList quietEnvironment() {
    return {u"PAGER=cat"_s,  u"GIT_PAGER=cat"_s, u"MANPAGER=cat"_s,         u"SYSTEMD_PAGER="_s,
            u"NO_COLOR=1"_s, u"TERM=dumb"_s,     u"DEBIAN_FRONTEND=noninteractive"_s};
}
#endif

} // namespace

#ifdef Q_OS_WIN
struct RootCommandRunner::WinJob {
    Done done;
    QTemporaryDir scratch;
    HANDLE cancelEvent{nullptr};
    std::atomic<bool> cancelled{false};

    ~WinJob() {
        if (cancelEvent) {
            ::CloseHandle(cancelEvent);
            cancelEvent = nullptr;
        }
    }
};

namespace {

QByteArray readBoundedOutput(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    OutputCollector collector(64 * 1024, 64 * 1024);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(16 * 1024);
        if (chunk.isEmpty()) break;
        collector.append(chunk);
    }
    return collector.text();
}

} // namespace
#endif

RootCommandRunner::RootCommandRunner(HelperClient *helper, QObject *parent) : QObject(parent), helper_(helper) {}

RootCommandRunner::~RootCommandRunner() {
    cancelAll();
}

void RootCommandRunner::run(const QString &command, const QString &workingDirectory, int timeoutMs, Done done) {
#ifdef Q_OS_WIN
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"There is no command to run."_s});
        return;
    }

    auto job = std::make_shared<WinJob>();
    if (!job->scratch.isValid()) {
        done(Error{ErrorCode::Io, u"Cannot create a temporary folder for the elevated command."_s});
        return;
    }
    job->cancelEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->cancelEvent) {
        done(Error{ErrorCode::Io, u"Cannot create cancellation handle for elevated command."_s});
        return;
    }
    job->done = std::move(done);

    const QString innerPath = QDir::toNativeSeparators(job->scratch.filePath(u"inner.cmd"_s));
    const QString wrapperPath = QDir::toNativeSeparators(job->scratch.filePath(u"wrapper.cmd"_s));
    const QString outPath = QDir::toNativeSeparators(job->scratch.filePath(u"out.txt"_s));
    const QString codePath = QDir::toNativeSeparators(job->scratch.filePath(u"code.txt"_s));

    {
        QFile inner(innerPath);
        if (!inner.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            job->done(Error{ErrorCode::Io, u"Cannot write elevated script: %1"_s.arg(inner.errorString())});
            return;
        }
        inner.write("@echo off\r\nchcp 65001 >nul 2>&1\r\n");
        inner.write(trimmed.toUtf8());
        inner.write("\r\n");
    }
    {
        QFile wrapper(wrapperPath);
        if (!wrapper.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            job->done(Error{ErrorCode::Io, u"Cannot write elevated wrapper: %1"_s.arg(wrapper.errorString())});
            return;
        }
        wrapper.write("@echo off\r\nchcp 65001 >nul 2>&1\r\n");
        if (!workingDirectory.isEmpty()) {
            const QString nativeCwd = QDir::toNativeSeparators(workingDirectory);
            wrapper.write(u"cd /d \"%1\"\r\n"_s.arg(nativeCwd).toUtf8());
        }
        wrapper.write(u"call \"%1\" > \"%2\" 2>&1\r\n"_s.arg(innerPath, outPath).toUtf8());
        wrapper.write(u"echo %ERRORLEVEL% > \"%1\"\r\n"_s.arg(codePath).toUtf8());
    }

    winJobs_.push_back(job);
    const QPointer<RootCommandRunner> self(this);
    const QString nativeCwd = workingDirectory.isEmpty() ? QString() : QDir::toNativeSeparators(workingDirectory);

    QThreadPool::globalInstance()->start([self, job, wrapperPath, outPath, codePath, nativeCwd, timeoutMs] {
        QElapsedTimer clock;
        clock.start();

        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const QString cmdExe = qEnvironmentVariable("COMSPEC", u"cmd.exe"_s);
        const QString params = u"/D /Q /C \"%1\""_s.arg(wrapperPath);

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = reinterpret_cast<LPCWSTR>(cmdExe.utf16());
        sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
        sei.lpDirectory = nativeCwd.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(nativeCwd.utf16());
        sei.nShow = SW_HIDE;

        const BOOL launched = ::ShellExecuteExW(&sei);
        const DWORD launchErr = launched ? 0 : ::GetLastError();
        if (comHr == S_OK || comHr == S_FALSE) ::CoUninitialize();

        if (!launched) {
            Result<CommandOutput> res = (launchErr == ERROR_CANCELLED)
                ? Result<CommandOutput>(Error{ErrorCode::PermissionDenied,
                                              u"Administrator elevation was declined in the UAC prompt."_s})
                : Result<CommandOutput>(Error{ErrorCode::Io,
                                              u"Could not launch elevated command (Win32 error %1)."_s.arg(launchErr)});
            if (self) {
                QMetaObject::invokeMethod(self, [self, job, res = std::move(res)]() mutable {
                    if (!self) return;
                    std::erase(self->winJobs_, job);
                    if (!job->cancelled.load(std::memory_order_acquire) && job->done) job->done(std::move(res));
                }, Qt::QueuedConnection);
            }
            return;
        }

        if (job->cancelled.load(std::memory_order_acquire)) {
            if (sei.hProcess) {
                const DWORD pid = ::GetProcessId(sei.hProcess);
                if (pid != 0) process::killTree(qint64(pid));
                ::TerminateProcess(sei.hProcess, 1);
                ::CloseHandle(sei.hProcess);
            }
            if (self) {
                QMetaObject::invokeMethod(self, [self, job] {
                    if (!self) return;
                    std::erase(self->winJobs_, job);
                    if (job->done) job->done(Error{ErrorCode::Cancelled, u"The root command was stopped."_s});
                }, Qt::QueuedConnection);
            }
            return;
        }

        bool timedOut = false;
        bool wasCancelled = false;
        DWORD winExit = 0;

        if (sei.hProcess) {
            HANDLE handles[2] = {sei.hProcess, job->cancelEvent};
            const DWORD waitMs = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : INFINITE;
            const DWORD waited = ::WaitForMultipleObjects(2, handles, FALSE, waitMs);
            if (waited == WAIT_TIMEOUT) {
                timedOut = true;
                const DWORD pid = ::GetProcessId(sei.hProcess);
                if (pid != 0) process::killTree(qint64(pid));
                ::TerminateProcess(sei.hProcess, 1);
                ::WaitForSingleObject(sei.hProcess, 1'000);
            } else if (waited == WAIT_OBJECT_0 + 1 || job->cancelled.load(std::memory_order_acquire)) {
                wasCancelled = true;
                const DWORD pid = ::GetProcessId(sei.hProcess);
                if (pid != 0) process::killTree(qint64(pid));
                ::TerminateProcess(sei.hProcess, 1);
            } else {
                ::GetExitCodeProcess(sei.hProcess, &winExit);
            }
            ::CloseHandle(sei.hProcess);
        }

        if (wasCancelled) {
            if (self) {
                QMetaObject::invokeMethod(self, [self, job] {
                    if (!self) return;
                    std::erase(self->winJobs_, job);
                    if (job->done) job->done(Error{ErrorCode::Cancelled, u"The root command was stopped."_s});
                }, Qt::QueuedConnection);
            }
            return;
        }

        int exitCode = static_cast<int>(winExit);
        QFile codeFile(codePath);
        if (codeFile.open(QIODevice::ReadOnly)) {
            bool ok = false;
            const int parsed = QString::fromUtf8(codeFile.readAll()).trimmed().toInt(&ok);
            if (ok) exitCode = parsed;
        }

        CommandOutput result;
        result.exitCode = timedOut ? -1 : exitCode;
        result.timedOut = timedOut;
        result.crashed = false;
        result.output = QString::fromUtf8(readBoundedOutput(outPath));
        result.elapsedMs = clock.elapsed();

        if (self) {
            QMetaObject::invokeMethod(self, [self, job, result = std::move(result)]() mutable {
                if (!self) return;
                std::erase(self->winJobs_, job);
                if (job->done) job->done(std::move(result));
            }, Qt::QueuedConnection);
        }
    });
#else
    if (!helper_) {
        done(Error{ErrorCode::NotSupported, u"The root helper is not available."_s});
        return;
    }
    const QStringList argv = LocalCommandRunner::commandLine(command);
    if (argv.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"There is no command to run."_s});
        return;
    }

    auto job = std::make_shared<Job>();
    job->done = std::move(done);
    job->clock.start();

    helper_->runCommand(
        argv, workingDirectory, quietEnvironment(), timeoutMs,
        [job](quint8, const QByteArray &data) { job->output.append(data); },
        [this, job](int exitCode, const QString &status) {
            jobs_.erase(job->id);
            CommandOutput result;
            result.exitCode = exitCode;
            result.crashed = status == helper::kStatusCrashed;
            result.timedOut = status == helper::kStatusTimeout;
            result.output = QString::fromUtf8(job->output.text());
            result.elapsedMs = job->clock.elapsed();
            if (status == helper::kStatusCancelled) {
                job->done(Error{ErrorCode::Cancelled, u"The root command was stopped."_s});
                return;
            }
            job->done(result);
        },
        [this, job](Result<quint64> started) {
            if (!started) {
                job->done(started.error());
                return;
            }
            job->id = started.value();
            jobs_.emplace(job->id, job);
        });
#endif
}

void RootCommandRunner::cancelAll() {
#ifdef Q_OS_WIN
    const auto active = std::exchange(winJobs_, {});
    for (const auto &job : active) {
        job->cancelled.store(true, std::memory_order_release);
        if (job->cancelEvent) ::SetEvent(job->cancelEvent);
    }
#else
    jobs_.clear();
    if (helper_) helper_->cancelAllJobs();
#endif
}

} // namespace nexa
