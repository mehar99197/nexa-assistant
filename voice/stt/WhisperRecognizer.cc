#include "voice/stt/WhisperRecognizer.h"

#include "core/Logging.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <whisper.h>

#include <algorithm>
#include <mutex>
#include <vector>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kUnloadAfterMs = 10 * 60 * 1000;
constexpr int kSampleRate = 16'000;

void forwardLog(ggml_log_level level, const char *text, void *) {
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) {
        qCWarning(lcVoice).noquote() << "whisper:" << QString::fromUtf8(text).trimmed();
    }
}

bool abortRequested(void *flag) {
    return static_cast<std::atomic_bool *>(flag)->load();
}

// ggml runs fastest with one thread per physical core.
int threadCount() {
    return std::clamp(QThread::idealThreadCount() / 2, 1, 8);
}

} // namespace

// Owns the whisper context. Lives on the recogniser's worker thread; every call runs there.
class WhisperWorker : public QObject {
public:
    WhisperWorker(QString modelPath, QString language)
        : modelPath_(std::move(modelPath)), language_(language.isEmpty() ? QByteArray("auto") : language.toLatin1()) {}
    ~WhisperWorker() override { unload(); }

    Result<void> load() {
        if (context_) return {};
        static std::once_flag logging;
        std::call_once(logging, [] { whisper_log_set(&forwardLog, nullptr); });
        if (!QFileInfo::exists(modelPath_)) {
            return Error{ErrorCode::NotFound, u"The whisper model %1 is not installed."_s.arg(modelPath_)};
        }
        QElapsedTimer timer;
        timer.start();
        whisper_context_params params = whisper_context_default_params();
        params.use_gpu = false;
        context_ = whisper_init_from_file_with_params(QFile::encodeName(modelPath_).constData(), params);
        if (!context_) return Error{ErrorCode::Io, u"Cannot load the whisper model %1."_s.arg(modelPath_)};
        qCDebug(lcVoice) << "loaded" << QFileInfo(modelPath_).fileName() << "in" << timer.elapsed() << "ms";
        return {};
    }

    void unload() {
        if (!context_) return;
        whisper_free(context_);
        context_ = nullptr;
        qCDebug(lcVoice) << "unloaded the whisper model";
    }

    // With `words`, every segment is one word (max_len 1, split on words) and carries its time.
    Result<TimedTranscript> transcribe(const QByteArray &pcm, const QString &prompt, bool words, std::atomic_bool *abort) {
        if (const Result<void> loaded = load(); !loaded) return loaded.error();
        if (abort->load()) return Error{ErrorCode::Cancelled, u"Cancelled"_s};

        const auto *input = reinterpret_cast<const qint16 *>(pcm.constData());
        // Short clips are padded to a second of audio; whisper guesses wildly on less.
        std::vector<float> samples(std::max<qsizetype>(pcm.size() / 2, kSampleRate), 0.0f);
        for (qsizetype i = 0; i < pcm.size() / 2; ++i) samples[size_t(i)] = float(input[i]) / 32768.0f;

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = threadCount();
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.translate = false;
        params.no_context = true;
        params.no_timestamps = !words;
        params.token_timestamps = words;
        params.split_on_word = words;
        params.max_len = words ? 1 : 0;
        params.single_segment = samples.size() <= size_t(30 * kSampleRate);
        params.suppress_blank = true;
        params.suppress_nst = true;
        const QByteArray language = whisper_is_multilingual(context_) ? language_ : QByteArray("en");
        params.language = language.constData();
        const QByteArray initialPrompt = prompt.toUtf8();
        params.initial_prompt = initialPrompt.isEmpty() ? nullptr : initialPrompt.constData();
        params.abort_callback = &abortRequested;
        params.abort_callback_user_data = abort;
        // The encoder normally covers a 30 s window. Covering just the clip plus a second is 4–5×
        // faster for spoken commands with the same transcripts (measured with base.en): 0.5 s
        // instead of 2.3 s.
        const int frames = int(samples.size() / 320);      // encoder positions are 20 ms apart
        params.audio_ctx = std::min(1500, std::max(256, (frames + 50 + 63) / 64 * 64));

        QElapsedTimer timer;
        timer.start();
        const int status = whisper_full(context_, params, samples.data(), int(samples.size()));
        if (abort->load()) return Error{ErrorCode::Cancelled, u"Cancelled"_s};
        if (status != 0) return Error{ErrorCode::Unknown, u"Local speech recognition failed (%1)."_s.arg(status)};

        TimedTranscript transcript;
        const int segments = whisper_full_n_segments(context_);
        for (int i = 0; i < segments; ++i) {
            const QString segment = QString::fromUtf8(whisper_full_get_segment_text(context_, i));
            transcript.text += segment;
            if (words && !segment.trimmed().isEmpty()) {
                // Segment times are in hundredths of a second.
                transcript.words.append(TimedWord{.text = segment.trimmed(),
                                                  .startMs = int(whisper_full_get_segment_t0(context_, i) * 10),
                                                  .endMs = int(whisper_full_get_segment_t1(context_, i) * 10)});
            }
        }
        transcript.text = transcript.text.trimmed();
        qCDebug(lcVoice) << "whisper:" << pcm.size() / 32 << "ms of audio in" << timer.elapsed() << "ms with"
                         << params.n_threads << "threads";
        return transcript;
    }

private:
    QString modelPath_;
    QByteArray language_;
    whisper_context *context_{nullptr};
};

WhisperRecognizer::WhisperRecognizer(QString modelPath, QString language, QObject *parent)
    : ISpeechRecognizer(parent), modelPath_(std::move(modelPath)) {
    worker_ = new WhisperWorker(modelPath_, language);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(u"nexa-whisper"_s);
    thread_.start();

    unloadTimer_.setSingleShot(true);
    unloadTimer_.setInterval(kUnloadAfterMs);
    connect(&unloadTimer_, &QTimer::timeout, this, [worker = worker_] {
        QMetaObject::invokeMethod(worker, [worker] { worker->unload(); });
    });
}

WhisperRecognizer::~WhisperRecognizer() {
    cancel();
    thread_.quit();
    thread_.wait();     // the worker frees the model on its own thread
}

QString WhisperRecognizer::name() const {
    return u"whisper.cpp (%1)"_s.arg(QFileInfo(modelPath_).completeBaseName().remove(u"ggml-"_s));
}

void WhisperRecognizer::warmUp() {
    QMetaObject::invokeMethod(worker_, [worker = worker_] {
        if (const Result<void> loaded = worker->load(); !loaded) qCWarning(lcVoice) << loaded.error().message;
    });
    unloadTimer_.start();
}

void WhisperRecognizer::transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) {
    run(pcm16k, prompt, false, [done = std::move(done)](Result<TimedTranscript> result) {
        if (!result) {
            done(result.error());
            return;
        }
        done(result.value().text);
    });
}

void WhisperRecognizer::transcribeWords(const QByteArray &pcm16k, const QString &prompt, DoneWords done) {
    run(pcm16k, prompt, true, std::move(done));
}

void WhisperRecognizer::run(const QByteArray &pcm16k, const QString &prompt, bool words, DoneWords done) {
    cancel();
    auto call = std::make_shared<DoneWords>(std::move(done));
    auto abort = std::make_shared<std::atomic_bool>(false);
    pending_ = call;
    abort_ = abort;
    unloadTimer_.stop();
    QMetaObject::invokeMethod(worker_, [this, worker = worker_, pcm16k, prompt, words, abort, call] {
        Result<TimedTranscript> result = worker->transcribe(pcm16k, prompt, words, abort.get());
        // Back on the GUI thread; dropped if the recogniser is gone by then.
        QMetaObject::invokeMethod(this, [this, call, result] {
            unloadTimer_.start();
            if (*call) std::exchange(*call, nullptr)(result);
        }, Qt::QueuedConnection);
    });
}

void WhisperRecognizer::cancel() {
    if (abort_) abort_->store(true);
    abort_.reset();
    if (const std::shared_ptr<DoneWords> call = std::exchange(pending_, nullptr); call && *call) {
        std::exchange(*call, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }
}

} // namespace nexa
