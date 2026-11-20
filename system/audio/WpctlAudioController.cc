#include "system/audio/WpctlAudioController.h"

#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>
#include <functional>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kTimeoutMs = 5'000;
const QString kSink = u"@DEFAULT_AUDIO_SINK@"_s;

#ifdef Q_OS_WIN
struct ComScope {
    HRESULT hr{E_FAIL};
    ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (hr == S_OK || hr == S_FALSE) ::CoUninitialize();
    }
};

Result<VolumeState> winWithEndpoint(bool microphone, const std::function<HRESULT(IAudioEndpointVolume *)> &fn) {
    ComScope com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) {
        return Error{ErrorCode::Io, u"COM initialization failed (0x%1)"_s.arg(quint32(com.hr), 8, 16, u'0')};
    }
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                    reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return Error{ErrorCode::NotSupported, u"CoreAudio device enumerator unavailable"_s};
    }
    IMMDevice *device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(microphone ? eCapture : eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return Error{ErrorCode::NotFound,
                     microphone ? u"No default microphone endpoint found"_s : u"No default speaker endpoint found"_s};
    }
    IAudioEndpointVolume *endpoint = nullptr;
    hr = device->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&endpoint));
    device->Release();
    if (FAILED(hr) || !endpoint) {
        return Error{ErrorCode::Io, u"Could not activate CoreAudio endpoint volume"_s};
    }
    if (fn) {
        hr = fn(endpoint);
        if (FAILED(hr)) {
            endpoint->Release();
            return Error{ErrorCode::Io, u"Audio control failed (0x%1)"_s.arg(quint32(hr), 8, 16, u'0')};
        }
    }
    float scalar = 0.0f;
    BOOL mute = FALSE;
    const HRESULT volHr = endpoint->GetMasterVolumeLevelScalar(&scalar);
    const HRESULT muteHr = endpoint->GetMute(&mute);
    endpoint->Release();
    if (FAILED(volHr) || FAILED(muteHr)) {
        return Error{ErrorCode::Io, u"Could not read updated volume state"_s};
    }
    return VolumeState{qBound(0, int(std::lround(double(scalar) * 100.0)), 100), mute != FALSE};
}
#endif

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
#ifdef Q_OS_WIN
    done(winWithEndpoint(target_ != kSink, {}));
#else
    wpctl({u"get-volume"_s, target_}, [done = std::move(done)](Result<QString> output) {
        if (!output) {
            done(output.error());
            return;
        }
        done(parse(output.value()));
    });
#endif
}

void WpctlAudioController::setVolume(int percent, Done done) {
#ifdef Q_OS_WIN
    const float level = float(qBound(0, percent, 100)) / 100.0f;
    done(winWithEndpoint(target_ != kSink, [level](IAudioEndpointVolume *ep) {
        return ep->SetMasterVolumeLevelScalar(level, nullptr);
    }));
#else
    const double level = qBound(0, percent, 100) / 100.0;
    thenReport({u"set-volume"_s, target_, QString::number(level, 'f', 2)}, std::move(done));
#endif
}

void WpctlAudioController::changeVolume(int delta, Done done) {
#ifdef Q_OS_WIN
    done(winWithEndpoint(target_ != kSink, [delta](IAudioEndpointVolume *ep) {
        float current = 0.0f;
        HRESULT hr = ep->GetMasterVolumeLevelScalar(&current);
        if (FAILED(hr)) return hr;
        const float next = qBound(0.0f, current + float(delta) / 100.0f, 1.0f);
        hr = ep->SetMasterVolumeLevelScalar(next, nullptr);
        if (FAILED(hr)) return hr;
        if (delta > 0) return ep->SetMute(FALSE, nullptr);
        return S_OK;
    }));
#else
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
#endif
}

void WpctlAudioController::setMuted(std::optional<bool> muted, Done done) {
#ifdef Q_OS_WIN
    done(winWithEndpoint(target_ != kSink, [muted](IAudioEndpointVolume *ep) {
        BOOL next = FALSE;
        if (muted.has_value()) {
            next = *muted ? TRUE : FALSE;
        } else {
            BOOL cur = FALSE;
            const HRESULT hr = ep->GetMute(&cur);
            if (FAILED(hr)) return hr;
            next = (cur == FALSE) ? TRUE : FALSE;
        }
        return ep->SetMute(next, nullptr);
    }));
#else
    const QString state = !muted ? u"toggle"_s : (*muted ? u"1"_s : u"0"_s);
    thenReport({u"set-mute"_s, target_, state}, std::move(done));
#endif
}

void WpctlAudioController::thenReport(const QStringList &arguments, Done done) {
#ifdef Q_OS_WIN
    Q_UNUSED(arguments);
    volume(std::move(done));
#else
    wpctl(arguments, [this, done = std::move(done)](Result<QString> result) {
        if (!result) {
            done(result.error());
            return;
        }
        volume(done);
    });
#endif
}

void WpctlAudioController::wpctl(const QStringList &arguments, std::function<void(Result<QString>)> done) {
#ifdef Q_OS_WIN
    Q_UNUSED(arguments);
    done(Error{ErrorCode::NotSupported, u"wpctl is only used on Linux."_s});
#else
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
#endif
}

} // namespace nexa
