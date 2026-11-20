#include "voice/tts/Phonemizer.h"

#include "core/ProcessUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kTimeoutMs = 20'000;
const QString kInstall = u"run scripts/download-piper.sh"_s;

} // namespace

Phonemizer::Phonemizer(QString piperDir) : piperDir_(std::move(piperDir)) {}

QString Phonemizer::binary(const QString &piperDir) {
#ifdef Q_OS_WIN
    return piperDir + u"/piper_phonemize.exe"_s;
#else
    return piperDir + u"/piper_phonemize"_s;
#endif
}

bool Phonemizer::available() const {
    return QFileInfo(binary(piperDir_)).isExecutable() && QFileInfo(piperDir_ + u"/espeak-ng-data"_s).isDir();
}

QString Phonemizer::problem() const {
    if (available()) return {};
    return u"Piper's phonemizer is not installed (it turns words into sounds for your voice) — %1."_s.arg(kInstall);
}

Result<QStringList> Phonemizer::tokens(const QString &text) const {
    if (!available()) return Error{ErrorCode::NotFound, problem()};
    const QString line = text.simplified();
    if (line.isEmpty()) return Error{ErrorCode::InvalidArgument, u"Nothing to say."_s};
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
    const QString pathEnv = environment.value(u"PATH"_s);
    environment.insert(u"PATH"_s, pathEnv.isEmpty() ? piperDir_ : piperDir_ + QDir::listSeparator() + pathEnv);
#else
    const QString libraries = environment.value(u"LD_LIBRARY_PATH"_s);
    environment.insert(u"LD_LIBRARY_PATH"_s, libraries.isEmpty() ? piperDir_ : piperDir_ + u':' + libraries);
#endif
    process.setProcessEnvironment(environment);
    process::configureChild(process);
    process.setProgram(binary(piperDir_));
    process.setArguments({u"-l"_s, u"en-us"_s, u"--espeak-data"_s, piperDir_ + u"/espeak-ng-data"_s});
    process.start();
    if (!process.waitForStarted(5'000)) {
        return Error{ErrorCode::NotFound, u"Cannot start Piper's phonemizer: %1"_s.arg(process.errorString())};
    }
    process.write(line.toUtf8() + '\n');
    process.closeWriteChannel();
    if (!process.waitForFinished(kTimeoutMs)) {
        process.kill();
        return Error{ErrorCode::Timeout, u"Piper's phonemizer did not answer within %1 s."_s.arg(kTimeoutMs / 1000)};
    }
    const QByteArray output = process.readAllStandardOutput();
    const qsizetype newline = output.indexOf('\n');
    return parse(newline < 0 ? output : output.left(newline));
}

Result<QStringList> Phonemizer::parse(const QByteArray &jsonLine) {
    const QJsonDocument document = QJsonDocument::fromJson(jsonLine.trimmed());
    if (!document.isObject()) return Error{ErrorCode::Parse, u"Piper's phonemizer answered something that is not JSON."_s};
    const QJsonValue phonemes = document.object().value(u"phonemes"_s);
    if (!phonemes.isArray()) return Error{ErrorCode::Parse, u"Piper's phonemizer listed no phonemes."_s};
    QStringList tokens;
    for (const QJsonValue &value : phonemes.toArray()) {
        const QString token = value.toString();
        if (!token.isEmpty()) tokens.append(token);
    }
    if (tokens.isEmpty()) return Error{ErrorCode::Parse, u"Piper's phonemizer found no sounds in the words."_s};
    return tokens;
}

} // namespace nexa
