#include "voice/listen/EnergyVoiceDetector.h"

namespace nexa {

namespace {

EnergyVad::Settings settingsWith(double startMarginDb) {
    EnergyVad::Settings settings;
    settings.startMarginDb = startMarginDb;
    return settings;
}

} // namespace

EnergyVoiceDetector::EnergyVoiceDetector(double startMarginDb) : vad_(settingsWith(startMarginDb)) {}

QList<IVoiceDetector::Event> EnergyVoiceDetector::feed(QByteArrayView pcm16k) {
    QList<Event> events;
    for (const EnergyVad::Event event : vad_.feed(pcm16k)) {
        if (event == EnergyVad::Event::SpeechStarted) {
            events.append(Event{.kind = Event::Kind::Started, .start = vad_.utteranceStart(), .end = 0});
            continue;
        }
        const EnergyVad::Span span = vad_.lastSpan();
        vad_.takeUtterance();       // the audio is cut from the pipeline's ring, as it was said
        events.append(Event{.kind = Event::Kind::Ended, .start = span.start, .end = span.end});
    }
    return events;
}

} // namespace nexa
