#pragma once

#include "core/Result.h"
#include "core/config/Config.h"
#include "voice/listen/ListeningFront.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace nexa {

class HttpClient;
class ISpeechRecognizer;
class ISpeechSynthesizer;
class ITextToSpeech;
class SherpaRuntime;

// A provider as Settings → Voice offers it for recognition or for the voice: greyed, with the reason, when it
// cannot do speech.
struct VoiceProviderChoice {
    QString id;
    QString name;
    bool usable{false};
    QString reason;     // "takes text only", "no API key", …; empty when usable
};

// Factory for the voice backends chosen in Settings → Voice. The cloud part comes from the provider chosen
// there — or, automatically, from the first choice and then every other provider with a speech model — with the
// local engine behind it as the fallback. The PC's voice, the phone app and the web remote all build from here,
// so they agree. Also builds the listening front, and knows where the downloaded engines and models live
// (~/.local/share/nexa).
class VoiceBackends {
public:
    static QString dataDir();
    static QString whisperModelPath(const QString &model);     // …/models/ggml-<model>.bin
    static QString piperBinary();
    static QStringList piperVoices();                           // installed voice names
    static bool whisperBuilt();

    // Which providers recognise requests, in order (none for an explicit choice that cannot).
    static QList<const ProviderConfig *> recognitionProviders(const Config &config);
    // Whose voice speaks; null for none.
    static const ProviderConfig *speechProvider(const Config &config);
    // Every provider, for Settings' lists.
    static QList<VoiceProviderChoice> recognitionChoices(const Config &config);
    static QList<VoiceProviderChoice> speechChoices(const Config &config);

    // Recogniser for spoken requests. The error explains why none could be built.
    static Result<std::unique_ptr<ISpeechRecognizer>> createRecognizer(const Config &config, HttpClient *http);
    // Local recogniser (whisper.cpp) — the only kind the wake word may use.
    static Result<std::unique_ptr<ISpeechRecognizer>> createLocalRecognizer(const VoiceConfig &voice);
    // Speech output; a null pointer when the user switched it off.
    static Result<std::unique_ptr<ITextToSpeech>> createTextToSpeech(const Config &config, HttpClient *http);
    // The engines that hand back audio instead of playing it — the provider's voice, Piper — in the
    // order Settings → Voice asks for. Empty when speech is off. speech-dispatcher can only play,
    // so with it the others are used the way "auto" would. `problems` says what was left out.
    static std::vector<std::unique_ptr<ISpeechSynthesizer>> createSynthesizers(const Config &config, HttpClient *http,
                                                                               QStringList *problems = nullptr);

    // What hears speech and the name: Silero, noise reduction (when on) and — `withSpotter` — the keyword
    // spotter, on a worker thread, when sherpa-onnx is installed; loudness alone otherwise. `problems` says what
    // is missing (only when the spotter was wanted).
    struct FrontBuild {
        std::unique_ptr<ListeningFront> front;
        QStringList problems;
    };
    static FrontBuild createListeningFront(const VoiceConfig &voice, SherpaRuntime *sherpa, bool withSpotter);
};

} // namespace nexa
