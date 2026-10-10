#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>

#include <deque>

namespace nexa {

// Energy-based voice activity detector over 16 kHz mono Int16 PCM (pure). The noise floor is the
// quietest frame of the last 1.5 s — speech always has gaps between syllables, so the floor
// follows the room, not the voice, and the same settings work for a quiet laptop microphone and
// a loud headset. Speech starts `startMarginDb` above the floor and ends after `hangoverMs`
// below `stopMarginDb`; each utterance keeps a little audio from before its detected start.
class EnergyVad {
public:
    struct Settings {
        int sampleRate{16'000};
        int frameMs{20};
        double startMarginDb{14.0};
        double stopMarginDb{8.0};
        double minLevelDb{-58.0};       // quieter frames are never speech
        int floorWindowMs{1'500};
        int minSpeechMs{120};           // shorter bursts (clicks, bumps) are ignored
        int hangoverMs{700};            // this much quiet ends an utterance
        int trailingMs{200};            // quiet kept at the end of an utterance
        int preRollMs{300};             // audio kept from before the detected start
        int maxUtteranceMs{30'000};     // longer speech is cut here
    };

    enum class Event { SpeechStarted, SpeechEnded };

    EnergyVad();
    explicit EnergyVad(Settings settings);

    // Feeds samples of any length; returns the events they caused, in order. After SpeechEnded
    // the utterance is available from takeUtterance().
    QList<Event> feed(QByteArrayView pcm);
    QByteArray takeUtterance();
    void reset();

    void setHangoverMs(int ms) { settings_.hangoverMs = ms; }
    bool inSpeech() const { return inSpeech_; }
    bool heardSpeech() const { return heardSpeech_; }       // any speech since the last reset()
    double levelDb() const { return levelDb_; }             // of the last frame
    double noiseFloorDb() const;

    // RMS level in dBFS (-100 for digital silence).
    static double rmsDb(const qint16 *samples, qsizetype count);

    // Whether a whole recording contains speech: its loud frames stand well above its quiet ones,
    // or it is loud throughout. For push-to-talk, where speech may start before the live noise
    // floor has settled.
    static bool containsSpeech(QByteArrayView pcm);
    static bool containsSpeech(QByteArrayView pcm, const Settings &settings);

    // A whole recording without the quiet before and after the speech (pre-roll and trailing
    // margins kept): less to upload and less for a local recogniser to process.
    static QByteArray trimSilence(const QByteArray &pcm, const Settings &settings);

private:
    void processFrame(const qint16 *samples, QList<Event> &events);
    void endUtterance(QList<Event> &events);
    qsizetype bytesFor(int ms) const { return qsizetype(settings_.sampleRate) * ms / 1000 * 2; }

    Settings settings_;
    qsizetype frameSamples_;
    QByteArray pending_;            // samples of the incomplete frame
    QByteArray preRoll_;            // recent audio while quiet
    QByteArray candidate_;          // loud frames not yet long enough to count as speech
    QByteArray utterance_;          // the utterance being collected
    QByteArray finished_;           // the last completed utterance
    std::deque<double> recentLevels_;
    double levelDb_{-100.0};
    bool inSpeech_{false};
    bool heardSpeech_{false};
    int loudMs_{0};
    int quietMs_{0};
    int speechMs_{0};
};

} // namespace nexa
