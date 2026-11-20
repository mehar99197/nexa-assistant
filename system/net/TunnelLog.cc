#include "system/net/TunnelLog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa::tunnellog {

namespace {

// One line, with the code ngrok puts at the end of an error kept ("… ERR_NGROK_4018").
QString oneLine(const QString &text) {
    const QStringList lines = text.split(QRegularExpression(u"[\\r\\n]+"_s), Qt::SkipEmptyParts);
    if (lines.isEmpty()) return {};
    QString first = lines.first().trimmed();
    for (const QString &line : lines) {
        if (line.trimmed().startsWith(u"ERR_NGROK_"_s) && !first.contains(line.trimmed())) {
            first += u" ("_s + line.trimmed() + u')';
        }
    }
    return first;
}

} // namespace

LineEvent cloudflaredLine(const QString &line) {
    LineEvent event;
    static const QRegularExpression quick(u"https://[a-z0-9-]+\\.trycloudflare\\.com"_s);
    if (const QRegularExpressionMatch found = quick.match(line); found.hasMatch()) event.url = found.captured();
    if (line.contains(u"Registered tunnel connection"_s)) event.connected = true;
    if (line.contains(u"Could not lookup srv records"_s)) event.edgeLookupFailed = true;
    static const QRegularExpression error(u"(?:^|\\s)ERR\\s+(.*)$"_s);
    if (const QRegularExpressionMatch found = error.match(line); found.hasMatch()) {
        event.error = found.captured(1).trimmed();
        // What stops the tunnel for good, until the owner signs in or creates it; the rest it gets over by itself.
        static const QRegularExpression fatal(
            u"cert\\.pem|credentials file|tunnel not found|no such tunnel|failed to unmarshal|Unauthorized|"
            "not authorized|error parsing tunnel ID|Cannot determine default"_s,
            QRegularExpression::CaseInsensitiveOption);
        event.fatal = fatal.match(event.error).hasMatch();
    }
    return event;
}

LineEvent ngrokLine(const QString &line) {
    LineEvent event;
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed().toUtf8());
    if (!document.isObject()) return event;
    const QJsonObject object = document.object();
    if (object.value(u"msg"_s).toString() == u"started tunnel"_s) {
        event.url = object.value(u"url"_s).toString();
        event.connected = !event.url.isEmpty();
    }
    const QString level = object.value(u"lvl"_s).toString();
    if (level == u"eror"_s || level == u"crit"_s) {
        event.error = oneLine(object.value(u"err"_s).toString());
        if (event.error.isEmpty()) event.error = object.value(u"msg"_s).toString();
        // Authentication, a domain that is not the account's, a second session on a free plan: nothing a retry fixes.
        static const QRegularExpression fatal(u"authentication failed|authtoken|ERR_NGROK_(4018|8012|108|313|320)|"
                                              "not (?:found|reserved)|forbidden"_s,
                                              QRegularExpression::CaseInsensitiveOption);
        event.fatal = fatal.match(event.error).hasMatch();
    }
    return event;
}

TailscaleState parseTailscaleStatus(const QByteArray &json) {
    TailscaleState state;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) return state;
    const QJsonObject root = document.object();
    state.backend = root.value(u"BackendState"_s).toString();
    state.signedIn = state.backend == u"Running"_s;
    const QJsonObject self = root.value(u"Self"_s).toObject();
    for (const QJsonValue &address : self.value(u"TailscaleIPs"_s).toArray()) {
        const QString ip = address.toString();
        if (ip.count(u'.') == 3) {
            state.ipv4 = ip;
            break;
        }
    }
    state.dnsName = self.value(u"DNSName"_s).toString();
    if (state.dnsName.endsWith(u'.')) state.dnsName.chop(1);
    if (!state.signedIn) {
        state.ipv4.clear();
        state.dnsName.clear();
    }
    return state;
}

QString tailscaleLoginUrl(const QString &output) {
    static const QRegularExpression link(u"https://login\\.tailscale\\.com/[A-Za-z0-9/._-]+"_s);
    return link.match(output).captured();
}

} // namespace nexa::tunnellog
