#include "core/ai/AgySession.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "core/ProcessUtils.h"

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kIdleStopMs = 10 * 60 * 1000;
constexpr qsizetype kStderrKept = 4'096;

void killGroup(qint64 pid) {
    process::killTree(pid);
}

constexpr QFileDevice::Permissions kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

void makePrivateDir(const QString &path) {
    QDir().mkpath(path);
    QFile::setPermissions(path, kOwnerOnly | QFileDevice::ExeOwner);
}

} // namespace

const QStringList &AgySession::deniedTools() {
    // agy's permission categories, matched with "(*)": "(**)" does not match a path.
    static const QStringList rules{u"read_file(*)"_s, u"write_file(*)"_s, u"command(*)"_s, u"read_url(*)"_s,
                                   u"execute_url(*)"_s, u"mcp(*)"_s, u"unsandboxed(*)"_s};
    return rules;
}

QString AgySession::workspace(const QString &dir) {
    return dir + u"/work"_s;
}

QProcessEnvironment AgySession::prepare(const QString &dir) {
    const QString home = dir + u"/home"_s;
    const QString settingsDir = home + u"/.gemini/antigravity-cli"_s;
    makePrivateDir(dir);
    makePrivateDir(home);
    makePrivateDir(home + u"/.gemini"_s);
    makePrivateDir(settingsDir);
    makePrivateDir(workspace(dir));

    const QString path = settingsDir + u"/settings.json"_s;
    QJsonObject settings;
    if (QFile existing(path); existing.open(QIODevice::ReadOnly)) {
        settings = QJsonDocument::fromJson(existing.readAll()).object();
    }
    QJsonObject permissions = settings.value(u"permissions"_s).toObject();
    permissions.remove(u"allow"_s);
    permissions.remove(u"ask"_s);
    permissions.insert(u"deny"_s, QJsonArray::fromStringList(deniedTools()));
    settings.insert(u"permissions"_s, permissions);
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.setPermissions(kOwnerOnly);
        file.write(QJsonDocument(settings).toJson());
        if (!file.commit()) qCWarning(lcAi) << "cannot write agy's settings" << path;
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"HOME"_s, home);
    return environment;
}

AgySession::AgySession(QString program, QString model, QString workingDir, QObject *parent)
    : QObject(parent), program_(std::move(program)), model_(std::move(model)), workingDir_(std::move(workingDir)) {
    idle_.setSingleShot(true);
    idle_.setInterval(kIdleStopMs);
    connect(&idle_, &QTimer::timeout, this, &AgySession::stop);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &AgySession::readEvents);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        if (stderrTail_.size() > kStderrKept) stderrTail_ = stderrTail_.right(kStderrKept);
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        stderrTail_ += QString::fromUtf8(process_.readAllStandardError());
        ended(AgyEvents::exitError(exitCode, stderrTail_));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        ended(Error{ErrorCode::NotFound, u"Cannot start agy (%1): %2"_s.arg(program_, process_.errorString())});
    });
}

AgySession::~AgySession() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
    process_.waitForFinished(1'000);
}

void AgySession::start() {
    if (started_) return;
    started_ = true;
    process_.setProcessEnvironment(prepare(workingDir_));
    QStringList arguments{u"--input-format"_s, u"stream-json"_s, u"--output-format"_s, u"stream-json"_s,
                          u"--disable-slash-commands"_s, u"--print="_s};
    if (!model_.isEmpty()) arguments += {u"--model"_s, model_};
    process_.setProgram(program_);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(workspace(workingDir_));
    process::configureChild(process_);
    qCInfo(lcAi) << "starting agy" << (model_.isEmpty() ? u"(its own model)"_s : model_);
    process_.start();
}

void AgySession::send(const QString &text) {
    busy_ = true;           // before start(): a program that cannot start fails this turn
    ++turns_;
    idle_.stop();
    start();
    if (broken_) return;
    process_.write(AgyEvents::userTurn(text));
}

void AgySession::stop() {
    idle_.stop();
    broken_ = true;
    if (process_.state() == QProcess::NotRunning) return;
    killGroup(process_.processId());
}

void AgySession::readEvents() {
    buffer_ += process_.readAllStandardOutput();
    qsizetype newline = 0;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline);
        buffer_.remove(0, newline + 1);
        const AgyEvent event = AgyEvents::decode(line);
        if (busy_) emit activity();
        switch (event.kind) {
        case AgyEvent::Kind::Text:
            if (busy_) emit text(event.text);
            break;
        case AgyEvent::Kind::ToolStep:
            if (!event.ok) qCInfo(lcAi) << "agy was refused its own tool" << event.toolName;
            emit toolStep(event.toolName, event.ok);
            break;
        case AgyEvent::Kind::Result:
            busy_ = false;
            idle_.start();
            emit turnFinished(event);
            break;
        case AgyEvent::Kind::Init:
        case AgyEvent::Kind::Ignored:
            break;
        }
    }
}

void AgySession::ended(const Error &error) {
    const bool wasBusy = std::exchange(busy_, false);
    broken_ = true;
    idle_.stop();
    if (wasBusy) emit turnFailed(error);
}

} // namespace nexa
