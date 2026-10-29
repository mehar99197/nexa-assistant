#pragma once

#include <QByteArrayView>
#include <QString>

#include <deque>
#include <optional>

namespace nexa {

// Whether the microphone's sound is usable (pure). Clipped sound — the input volume too high for the room — or
// a floor of loud noise (a fan blowing on the microphone) leaves no detector able to tell when someone speaks,
// so Nexa says so instead of going quietly deaf. Also the steps of Settings' "Lower it": measured levels in,
// the next input volume out.
class MicrophoneCheck {
public:
    struct Stats {
        double clippedShare{0.0};   // of the samples, 0 … 1
        double floorDb{-100.0};     // the quietest 20 ms
        double p90Db{-100.0};       // the loudest tenth of 20 ms frames starts here
    };

    // 16 kHz mono Int16 as the microphone delivers it; `nowMs` is when this audio ended.
    void feed(QByteArrayView pcm16k, qint64 nowMs);

    // More than 5 % clipped over the last 3 s, or the floor above -30 dBFS for 10 s.
    bool overloaded() const;
    // What is wrong, in words; empty when nothing is.
    QString describe() const;
    // describe(), at most once a minute while overloaded.
    std::optional<QString> notice(qint64 nowMs);

    static Stats measure(QByteArrayView pcm16k);
    // 5 % lower while anything clips or the room leaves a voice no room (loudest tenth above -20 dBFS); none
    // when the level is right, or already at 5 %.
    static std::optional<int> nextVolume(int percent, const Stats &measured);

private:
    struct Chunk {
        qint64 endMs{0};
        qsizetype samples{0};
        qsizetype clipped{0};
        double quietestDb{-100.0};
    };

    double clippedShare() const;    // over the last 3 s
    double floorDb() const;         // over the last 1.5 s

    std::deque<Chunk> chunks_;
    qint64 loudFloorSinceMs_{-1};
    std::optional<qint64> lastNoticeMs_;
};

} // namespace nexa
