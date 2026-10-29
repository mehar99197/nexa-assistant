#include "system/audio/WpctlAudioController.h"

#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kTimeoutMs = 5'000;
const QString kSink = u"@DEFAULT_AUDIO_SINK@"_s;

} // namespace

WpctlAudioController::WpctlAudioController(Target target, QObject *parent)
    : QObject(parent), target_(target == Target::Microphone ? u"@DEFAULT_AUDIO_SOURCE@"_s : kSink) {}

Result<VolumeState> WpctlAudioController::parse(const QString &output) {
    static const QRegularExpression expression(u"Volume:\\s*([0-9]+(?:\\.[0-9]+)?)(\\s*\\[MUTED\\])?"_s);
    const QRegularExpressionMatch match = expression.match(output);
    if (!match.hasMatch()) return Error{ErrorCode::Parse, u"Unexpected wpctl output: %1"_s.arg(output.trimmed())};
    return VolumeState{int(std::lround(match.captured(1).toDouble() * 100.0)), match.hasCaptured(2)};
}

void WpctlAudioController::volume(Done done) {
    wpctl({u"get-volume"_s, target_}, [done = std::move(done)](Result<QString> output) {
        if (!output) {
            done(output.error());
            return;
        }
        done(parse(output.value()));
    });
}

void WpctlAudioController::setVolume(int percent, Done done) {
    const double level = qBound(0, percent, 100) / 100.0;
    thenReport({u"set-volume"_s, target_, QString::number(level, 'f', 2)}, std::move(done));
}

void WpctlAudioController::changeVolume(int delta, Done done) {
    const QString step = u"%1%%2"_s.arg(qMin(qAbs(delta), 100)).arg(delta >= 0 ? u'+' : u'-');
    if (delta <= 0) {
        thenReport({u"set-volume"_s, target_, step}, std::move(done));
        return;
    }
    // -l 1.0 caps the level at 100%.
    wpctl({u"set-volume"_s, u"-l"_s, u"1.0"_s, target_, step}, [this, done = std::move(done)](Result<QString> result) {
        if (!result) {
            done(result.error());
            return;
        }
        thenReport({u"set-mute"_s, target_, u"0"_s}, done);
    });
}

void WpctlAudioController::setMuted(std::optional<bool> muted, Done done) {
    const QString state = !muted ? u"toggle"_s : (*muted ? u"1"_s : u"0"_s);
    thenReport({u"set-mute"_s, target_, state}, std::move(done));
}

void WpctlAudioController::thenReport(const QStringList &arguments, Done done) {
    wpctl(arguments, [this, done = std::move(done)](Result<QString> result) {
        if (!result) {
            done(result.error());
            return;
        }
        volume(done);
    });
}

void WpctlAudioController::wpctl(const QStringList &arguments, std::function<void(Result<QString>)> done) {
    auto *process = new QProcess(this);
    auto finished = std::make_shared<bool>(false);
    const auto report = [process, finished, done = std::move(done)](Result<QString> result) {
        if (*finished) return;
        *finished = true;
        process->deleteLater();
        done(std::move(result));
    };
    connect(process, &QProcess::finished, this, [process, report](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0) {
            const QString detail = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            report(Error{ErrorCode::Unknown, u"wpctl failed: %1"_s.arg(detail.isEmpty() ? u"exit code %1"_s.arg(exitCode) : detail)});
            return;
        }
        report(QString::fromLocal8Bit(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::errorOccurred, this, [report](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            report(Error{ErrorCode::NotFound, u"wpctl is not installed (PipeWire's WirePlumber CLI)"_s});
        }
    });
    QTimer::singleShot(kTimeoutMs, process, [process, report] {
        process->kill();
        report(Error{ErrorCode::Timeout, u"wpctl did not answer"_s});
    });
    process->start(u"wpctl"_s, arguments);
}

} // namespace nexa
