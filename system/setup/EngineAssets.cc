#include "system/setup/EngineAssets.h"

#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace nexa::engineassets {

namespace {

const QString kSherpaReleases = u"https://github.com/k2-fsa/sherpa-onnx/releases/download"_s;

} // namespace

QList<Asset> all() {
    static const QList<Asset> assets{
        Asset{u"silero"_s, u"listening"_s, kSherpaReleases + u"/asr-models/silero_vad.onnx"_s, 643'854,
              u"9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6"_s, u"sherpa"_s, {u"silero_vad.onnx"_s}, {}},
        Asset{u"gtcrn"_s, u"listening"_s, kSherpaReleases + u"/speech-enhancement-models/gtcrn_simple.onnx"_s, 535'638,
              u"e77603ac0c23dac3227dd2d7135b3a585cbee2679048aecfa886657d3ae1b534"_s, u"sherpa"_s, {u"gtcrn_simple.onnx"_s}, {}},
        Asset{u"kws"_s, u"listening"_s,
              kSherpaReleases + u"/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2"_s, 17'626'723,
              u"f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a"_s, u"sherpa/kws"_s,
              {u"encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s, u"decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s,
               u"joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"_s, u"tokens.txt"_s, u"bpe.model"_s},
              u"sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01"_s},
        Asset{u"speaker"_s, u"speaker"_s,
              kSherpaReleases + u"/speaker-recongition-models/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"_s, 26'485'263,
              u"c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5"_s, u"sherpa/speaker"_s,
              {u"3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"_s}, {}},
        Asset{u"piper-en_US-lessac-medium.json"_s, u"voice"_s,
              u"https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json"_s, 4'885,
              u"efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0"_s, u"piper/voices"_s,
              {u"en_US-lessac-medium.onnx.json"_s}, {}},
        Asset{u"piper-en_US-lessac-medium"_s, u"voice"_s,
              u"https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx"_s, 63'201'294,
              u"5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f"_s, u"piper/voices"_s,
              {u"en_US-lessac-medium.onnx"_s}, {}},
        Asset{u"vocos"_s, u"own-voice"_s, kSherpaReleases + u"/vocoder-models/vocos_24khz.onnx"_s, 54'157'409,
              u"bcb3b970e384161c4d634f0bb9e999ff1c471b34c9bc0b1049a5014065ed3cc0"_s, u"sherpa/zipvoice"_s, {u"vocos_24khz.onnx"_s}, {}},
        Asset{u"zipvoice"_s, u"own-voice"_s,
              kSherpaReleases + u"/tts-models/sherpa-onnx-zipvoice-distill-int8-zh-en-emilia.tar.bz2"_s, 109'162'785,
              u"77219c8b40f4ee8d73a7f902305ff6c1128ef9b54461c41b4ca6ed890b6c2803"_s, u"sherpa/zipvoice"_s,
              {u"encoder.int8.onnx"_s, u"decoder.int8.onnx"_s, u"tokens.txt"_s, u"lexicon.txt"_s},
              u"sherpa-onnx-zipvoice-distill-int8-zh-en-emilia"_s},
        Asset{u"whisper-base.en"_s, u"listening"_s, u"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"_s,
              147'964'211, u"a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"_s, u"models"_s,
              {u"ggml-base.en.bin"_s}, {}},
    };
    return assets;
}

QList<Asset> engines() {
    static const QList<Asset> assets{
        Asset{u"sherpa-onnx"_s, u"engine"_s, kSherpaReleases + u"/v2.0.1/sherpa-onnx-v2.0.1-linux-x64-shared-lib.tar.bz2"_s, 0,
              u"532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120"_s, u"sherpa/lib"_s,
              {u"lib/libsherpa-onnx-c-api.so"_s, u"lib/libonnxruntime.so"_s}, u"sherpa-onnx-v2.0.1-linux-x64-shared-lib"_s},
        Asset{u"piper"_s, u"engine"_s, u"https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz"_s, 0,
              u"a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992"_s, u"piper"_s, {u"piper"_s, u"piper_phonemize"_s},
              u"piper"_s},
    };
    return assets;
}

QList<Asset> missing(const QString &dataDir) {
    QList<Asset> out;
    for (const Asset &asset : all()) {
        bool complete = true;
        for (const QString &file : asset.files) {
            if (!QFileInfo::exists(dataDir + u'/' + asset.installDir + u'/' + file)) {
                complete = false;
                break;
            }
        }
        if (!complete) out.append(asset);
    }
    return out;
}

qint64 totalBytes(const QList<Asset> &assets) {
    qint64 total = 0;
    for (const Asset &asset : assets) total += asset.bytes;
    return total;
}

QStringList groups() {
    return {u"listening"_s, u"speaker"_s, u"voice"_s, u"own-voice"_s};
}

QString groupTitle(const QString &group) {
    if (group == u"listening"_s) return u"Listening — the wake name, speech detection and recognition"_s;
    if (group == u"speaker"_s) return u"Your voice — telling who is speaking"_s;
    if (group == u"voice"_s) return u"Speaking — the Piper voice"_s;
    if (group == u"own-voice"_s) return u"Speak in my voice — ZipVoice"_s;
    if (group == u"engine"_s) return u"Engines"_s;
    return group;
}

QString sizeText(qint64 bytes) {
    if (bytes >= 1'000'000'000) return u"%1 GB"_s.arg(bytes / 1'000'000'000.0, 0, 'f', 1);
    if (bytes >= 1'000'000) return u"%1 MB"_s.arg(qRound(bytes / 1'000'000.0));
    return u"%1 KB"_s.arg(qRound(bytes / 1'000.0));
}

} // namespace nexa::engineassets
