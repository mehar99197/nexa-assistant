#pragma once

#include "core/Result.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace nexa {

struct VolumeState {
    int percent{0};
    bool muted{false};
};

// The speakers' — or the microphone's — volume through PipeWire's `wpctl` (the default sink or source).
// Every call reports the new state.
class WpctlAudioController : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<VolumeState>)>;
    enum class Target { Speaker, Microphone };

    explicit WpctlAudioController(Target target = Target::Speaker, QObject *parent = nullptr);

    void volume(Done done);
    void setVolume(int percent, Done done);             // clamped to 0..100
    void changeVolume(int delta, Done done);            // relative; raising the volume also unmutes
    void setMuted(std::optional<bool> muted, Done done);// nullopt toggles

    // "Volume: 0.45 [MUTED]" → {45, true}
    static Result<VolumeState> parse(const QString &output);

private:
    void wpctl(const QStringList &arguments, std::function<void(Result<QString>)> done);

    QString target_;    // @DEFAULT_AUDIO_SINK@ or @DEFAULT_AUDIO_SOURCE@
    void thenReport(const QStringList &arguments, Done done);
};

} // namespace nexa
