#include "system/media/MprisController.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QHash>
#include <QStringList>

#include <memory>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kTimeoutMs = 3'000;
const QString kPrefix = u"org.mpris.MediaPlayer2."_s;
const QString kPath = u"/org/mpris/MediaPlayer2"_s;
const QString kPlayerInterface = u"org.mpris.MediaPlayer2.Player"_s;

QString identity(const QString &player) {
    return player.mid(kPrefix.size()).section(u'.', 0, 0);  // "firefox.instance_1_74" → "firefox"
}

QString pastTense(MprisController::Action action) {
    switch (action) {
    case MprisController::Action::Play: return u"Playing"_s;
    case MprisController::Action::Pause: return u"Paused"_s;
    case MprisController::Action::Toggle: return u"Toggled play/pause on"_s;
    case MprisController::Action::Next: return u"Skipped to the next track on"_s;
    case MprisController::Action::Previous: return u"Went back a track on"_s;
    case MprisController::Action::Stop: return u"Stopped"_s;
    }
    return {};
}

} // namespace

MprisController::MprisController(QDBusConnection bus, QObject *parent) : QObject(parent), bus_(std::move(bus)) {}

std::optional<MprisController::Action> MprisController::actionFromString(const QString &name) {
    const QString action = name.trimmed().toLower();
    if (action == u"play" || action == u"resume") return Action::Play;
    if (action == u"pause") return Action::Pause;
    if (action == u"toggle" || action == u"playpause" || action == u"play_pause") return Action::Toggle;
    if (action == u"next" || action == u"skip") return Action::Next;
    if (action == u"previous" || action == u"prev" || action == u"back") return Action::Previous;
    if (action == u"stop") return Action::Stop;
    return std::nullopt;
}

QString MprisController::methodName(Action action) {
    switch (action) {
    case Action::Play: return u"Play"_s;
    case Action::Pause: return u"Pause"_s;
    case Action::Toggle: return u"PlayPause"_s;
    case Action::Next: return u"Next"_s;
    case Action::Previous: return u"Previous"_s;
    case Action::Stop: return u"Stop"_s;
    }
    return {};
}

void MprisController::control(Action action, Done done) {
    const QDBusMessage list = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                                                             u"org.freedesktop.DBus"_s, u"ListNames"_s);
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(list, kTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, action, done = std::move(done)](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        const QDBusPendingReply<QStringList> reply = *call;
        if (reply.isError()) {
            done(Error{ErrorCode::Unknown, u"D-Bus: %1"_s.arg(reply.error().message())});
            return;
        }
        QStringList players;
        for (const QString &name : reply.value()) {
            if (name.startsWith(kPrefix)) players.append(name);
        }
        if (players.isEmpty()) {
            done(Error{ErrorCode::NotFound, u"No media player is running"_s});
            return;
        }
        choosePlayer(players, action, done);
    });
}

void MprisController::choosePlayer(const QStringList &players, Action action, Done done) {
    struct Survey {
        QStringList players;
        QHash<QString, QString> status;
        int pending{0};
    };
    auto survey = std::make_shared<Survey>();
    survey->players = players;
    survey->pending = int(players.size());

    const auto decide = [this, survey, action, done] {
        QString chosen;
        for (const QString &wanted : {u"Playing"_s, u"Paused"_s}) {
            for (const QString &player : std::as_const(survey->players)) {
                if (chosen.isEmpty() && survey->status.value(player) == wanted) chosen = player;
            }
        }
        send(chosen.isEmpty() ? survey->players.first() : chosen, action, done);
    };

    for (const QString &player : players) {
        QDBusMessage get = QDBusMessage::createMethodCall(player, kPath, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get << kPlayerInterface << u"PlaybackStatus"_s;
        auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(get, kTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [survey, player, decide](QDBusPendingCallWatcher *call) {
            call->deleteLater();
            const QDBusPendingReply<QDBusVariant> reply = *call;
            if (!reply.isError()) survey->status.insert(player, reply.value().variant().toString());
            if (--survey->pending == 0) decide();
        });
    }
}

void MprisController::send(const QString &player, Action action, Done done) {
    const QDBusMessage call = QDBusMessage::createMethodCall(player, kPath, kPlayerInterface, methodName(action));
    auto *watcher = new QDBusPendingCallWatcher(bus_.asyncCall(call, kTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [player, action, done = std::move(done)](QDBusPendingCallWatcher *reply) {
        reply->deleteLater();
        if (reply->isError()) {
            done(Error{ErrorCode::Unknown, u"%1: %2"_s.arg(identity(player), reply->error().message())});
            return;
        }
        done(u"%1 %2"_s.arg(pastTense(action), identity(player)));
    });
}

} // namespace nexa
