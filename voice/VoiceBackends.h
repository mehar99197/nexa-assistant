#pragma once

#include "core/Result.h"
#include "core/config/Config.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace nexa {

class HttpClient;
class ISpeechRecognizer;
class ISpeechSynthesizer;
class ITextToSpeech;

// Factory for the voice backends chosen in Settings → Voice. "auto" means the active provider's
// cloud model when it has one (OmniRoute by default) with the local engine behind it as the
// fallback. Also knows where the downloaded engines and models live (~/.local/share/nexa).
class VoiceBackends {
public:
    static QString dataDir();
    static QString whisperModelPath(const QString &model);     // …/models/ggml-<model>.bin
    static QString piperBinary();
    static QStringList piperVoices();                           // installed voice names
    static bool whisperBuilt();

    // Recogniser for spoken requests. The error explains why none could be built.
    static Result<std::unique_ptr<ISpeechRecognizer>> createRecognizer(const VoiceConfig &voice,
                                                                        const ProviderConfig *provider,
                                                                        HttpClient *http);
    // Local recogniser (whisper.cpp) — the only kind the wake word may use.
    static Result<std::unique_ptr<ISpeechRecognizer>> createLocalRecognizer(const VoiceConfig &voice);
    // Speech output; a null pointer when the user switched it off.
    static Result<std::unique_ptr<ITextToSpeech>> createTextToSpeech(const VoiceConfig &voice,
                                                                      const ProviderConfig *provider,
                                                                      HttpClient *http);
    // The engines that hand back audio instead of playing it — the provider's voice, Piper — in the
    // order Settings → Voice asks for. Empty when speech is off. speech-dispatcher can only play,
    // so with it the others are used the way "auto" would. `problems` says what was left out.
    static std::vector<std::unique_ptr<ISpeechSynthesizer>> createSynthesizers(const VoiceConfig &voice,
                                                                               const ProviderConfig *provider,
                                                                               HttpClient *http,
                                                                               QStringList *problems = nullptr);
};

} // namespace nexa
