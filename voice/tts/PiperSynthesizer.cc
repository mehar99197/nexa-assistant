#include "voice/tts/PiperSynthesizer.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

PiperSynthesizer::PiperSynthesizer(QString binary, QString model, double rate, QObject *parent)
    : ISpeechSynthesizer(parent),
      binary_(std::move(binary)),
      model_(std::move(model)),
      rate_(rate),
      outputDir_(QDir::temp().filePath(u"nexa-piper-XXXXXX"_s)) {
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &PiperSynthesizer::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        qCDebug(lcVoice) << "piper:" << process_.readAllStandardError().trimmed();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode) {
        failAll(Error{ErrorCode::Io, u"Piper stopped unexpectedly (exit code %1)."_s.arg(exitCode)});
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        failAll(Error{ErrorCode::NotFound, u"Cannot start Piper (%1): %2"_s.arg(binary_, process_.errorString())});
    });
}

PiperSynthesizer::~PiperSynthesizer() {
    cancel();
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1'000);
    }
}

QString PiperSynthesizer::name() const {
    return u"Piper (%1)"_s.arg(QFileInfo(model_).completeBaseName());
}

void PiperSynthesizer::synthesize(const QString &text, Done done) {
    const QString line = text.simplified();     // one sentence per line
    if (line.isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, u"Nothing to say."_s});
        return;
    }
    if (!ensureRunning()) {
        done(Error{ErrorCode::NotFound, u"Piper is not available (%1)."_s.arg(binary_)});
        return;
    }
    pending_.push_back(std::move(done));
    process_.write(line.toUtf8() + '\n');
}

void PiperSynthesizer::cancel() {
    // Piper still writes the files of these sentences; they are deleted unheard.
    discard_ += int(pending_.size());
    auto pending = std::exchange(pending_, {});
    for (Done &done : pending) done(Error{ErrorCode::Cancelled, u"Cancelled"_s});
}

bool PiperSynthesizer::ensureRunning() {
    if (process_.state() != QProcess::NotRunning) return true;
    if (!outputDir_.isValid() || !QFileInfo(binary_).isExecutable() || !QFileInfo::exists(model_)) return false;
    QStringList arguments{u"--model"_s, model_, u"--output_dir"_s, outputDir_.path(), u"--quiet"_s};
    // Rate ±1 → phonemes 1.6× shorter / longer.
    if (rate_ != 0.0) arguments << u"--length_scale"_s << QString::number(std::pow(1.6, -rate_), 'f', 2);
    stdout_.clear();
    discard_ = 0;
    process_.start(binary_, arguments);
    qCDebug(lcVoice) << "started Piper with" << QFileInfo(model_).fileName();
    return true;
}

void PiperSynthesizer::readOutput() {
    stdout_ += process_.readAllStandardOutput();
    const QString prefix = QDir::cleanPath(outputDir_.path()) + u'/';
    for (qsizetype newline; (newline = stdout_.indexOf('\n')) >= 0;) {
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(stdout_.left(newline)).trimmed()));
        stdout_.remove(0, newline + 1);
        // Only ever touch files in our own directory, whatever Piper prints.
        if (!path.startsWith(prefix) || path.contains(u"/../"_s)) continue;

        QFile file(path);
        const QByteArray wav = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        file.remove();
        if (discard_ > 0) {
            --discard_;
            continue;
        }
        if (pending_.empty()) continue;
        Done done = std::move(pending_.front());
        pending_.pop_front();
        if (wav.isEmpty()) {
            done(Error{ErrorCode::Io, u"Piper produced no audio."_s});
        } else {
            done(wav::decode(wav));
        }
    }
}

void PiperSynthesizer::failAll(const Error &error) {
    discard_ = 0;
    stdout_.clear();
    auto pending = std::exchange(pending_, {});
    for (Done &done : pending) done(error);
}

} // namespace nexa
