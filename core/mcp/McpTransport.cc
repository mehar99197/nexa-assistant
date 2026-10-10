#include "core/mcp/McpTransport.h"

#include "core/Logging.h"
#include "core/net/HttpClient.h"
#include "core/net/SseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kHttpTimeoutMs = 120'000;     // a tool may take a while; a stream ends when the result does
constexpr int kStopGraceMs = 2'000;

} // namespace

// ------------------------------------------------------------------ stdio

StdioMcpTransport::StdioMcpTransport(QString program, QStringList arguments, QStringList environment, QObject *parent)
    : IMcpTransport(parent),
      program_(std::move(program)),
      arguments_(std::move(arguments)),
      environment_(std::move(environment)) {
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &StdioMcpTransport::readLines);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray text = process_.readAllStandardError().trimmed();
        if (!text.isEmpty()) qCDebug(lcMcp).noquote() << program_ << "stderr:" << QString::fromUtf8(text.left(500));
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readLines();
        if (stopping_) return;
        emit failed(Error{ErrorCode::Io, status == QProcess::CrashExit
                                             ? tr("The connector's process crashed.")
                                             : tr("The connector's process exited (code %1).").arg(code)});
    });
}

StdioMcpTransport::~StdioMcpTransport() {
    stop();
}

void StdioMcpTransport::start(std::function<void(Result<void>)> done) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &assignment : environment_) {
        const qsizetype equals = assignment.indexOf(u'=');
        if (equals > 0) environment.insert(assignment.left(equals).trimmed(), assignment.mid(equals + 1));
    }
    process_.setProcessEnvironment(environment);
    process_.setProgram(program_);
    process_.setArguments(arguments_);
    // Started, or failed to: whichever comes first answers once.
    auto answered = std::make_shared<bool>(false);
    connect(&process_, &QProcess::started, this, [done, answered] {
        if (!std::exchange(*answered, true)) done(Result<void>{});
    });
    connect(&process_, &QProcess::errorOccurred, this, [this, done, answered](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || std::exchange(*answered, true)) return;
        done(Error{ErrorCode::NotFound, tr("Cannot start %1: %2").arg(program_, process_.errorString())});
    });
    process_.start();
}

void StdioMcpTransport::send(const QJsonObject &message) {
    if (process_.state() != QProcess::Running) return;
    process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void StdioMcpTransport::stop() {
    if (process_.state() == QProcess::NotRunning) return;
    stopping_ = true;
    process_.closeWriteChannel();
    process_.terminate();
    if (!process_.waitForFinished(kStopGraceMs)) process_.kill();
}

QString StdioMcpTransport::describe() const {
    return (QStringList{program_} + arguments_).join(u' ');
}

void StdioMcpTransport::readLines() {
    buffer_ += process_.readAllStandardOutput();
    qsizetype newline = -1;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qCDebug(lcMcp).noquote() << program_ << "said something that is not JSON-RPC:" << QString::fromUtf8(line.left(200));
            continue;
        }
        emit received(document.object());
    }
}

// ------------------------------------------------------------------ HTTP

HttpMcpTransport::HttpMcpTransport(HttpClient *http, QString url, QStringList headers, QObject *parent)
    : IMcpTransport(parent), http_(http), url_(std::move(url)), headers_(std::move(headers)) {}

void HttpMcpTransport::start(std::function<void(Result<void>)> done) {
    const QUrl url(url_);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
        done(Error{ErrorCode::InvalidArgument, tr("%1 is not a URL Nexa can reach.").arg(url_)});
        return;
    }
    stopped_ = false;
    done(Result<void>{});
}

void HttpMcpTransport::send(const QJsonObject &message) {
    if (stopped_) return;
    QNetworkRequest request = HttpClient::jsonRequest(QUrl(url_));
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", "2025-06-18");
    if (!sessionId_.isEmpty()) request.setRawHeader("Mcp-Session-Id", sessionId_);
    for (const QString &header : headers_) {
        const qsizetype colon = header.indexOf(u':');
        if (colon > 0) request.setRawHeader(header.left(colon).trimmed().toUtf8(), header.mid(colon + 1).trimmed().toUtf8());
    }
    const QPointer<HttpMcpTransport> self(this);
    auto contentType = std::make_shared<QByteArray>();
    QNetworkReply *reply = http_->post(request, QJsonDocument(message).toJson(QJsonDocument::Compact), kHttpTimeoutMs,
                                       this, [self, contentType](Result<QByteArray> body) {
        if (!self || self->stopped_) return;
        if (!body) {
            emit self->failed(body.error());
            return;
        }
        for (const QJsonObject &answer : messagesIn(body.value(), *contentType)) emit self->received(answer);
    });
    // The session id arrives with the first answer; the content type says how to read the body.
    connect(reply, &QNetworkReply::metaDataChanged, this, [self, reply, contentType] {
        if (!self) return;
        const QByteArray session = reply->rawHeader("Mcp-Session-Id");
        if (!session.isEmpty()) self->sessionId_ = session;
        *contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    });
}

void HttpMcpTransport::stop() {
    stopped_ = true;
}

QList<QJsonObject> HttpMcpTransport::messagesIn(const QByteArray &body, const QByteArray &contentType) {
    QList<QJsonObject> messages;
    const auto take = [&messages](const QByteArray &json) {
        const QJsonDocument document = QJsonDocument::fromJson(json);
        if (document.isObject()) {
            messages.append(document.object());
        } else if (document.isArray()) {
            for (const QJsonValue &value : document.array()) {
                if (value.isObject()) messages.append(value.toObject());
            }
        }
    };
    if (contentType.toLower().contains("text/event-stream")) {
        SseParser parser;
        QList<SseEvent> events = parser.feed(body);
        events += parser.finish();
        for (const SseEvent &event : events) take(event.data);
        return messages;
    }
    if (!body.trimmed().isEmpty()) take(body);
    return messages;
}

} // namespace nexa
