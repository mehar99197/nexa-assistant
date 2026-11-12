#include "system/a11y/AtspiScreenReader.h"

#include "core/Logging.h"
#include "system/a11y/AtspiBus.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QPointer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

using atspi::NodeRef;
using atspi::WireRef;

constexpr int kCallTimeoutMs = 2000;
constexpr int kWalkCap = 600;
constexpr int kWalkDepth = 48;
constexpr QLatin1StringView kRegistry{"org.a11y.atspi.Registry"};
constexpr QLatin1StringView kRootPath{"/org/a11y/atspi/accessible/root"};
constexpr QLatin1StringView kCachePath{"/org/a11y/atspi/cache"};
constexpr QLatin1StringView kAccessible{"org.a11y.atspi.Accessible"};
constexpr QLatin1StringView kApplication{"org.a11y.atspi.Application"};
constexpr QLatin1StringView kCache{"org.a11y.atspi.Cache"};
constexpr QLatin1StringView kText{"org.a11y.atspi.Text"};
constexpr QLatin1StringView kEditableText{"org.a11y.atspi.EditableText"};
constexpr QLatin1StringView kAction{"org.a11y.atspi.Action"};
constexpr QLatin1StringView kComponent{"org.a11y.atspi.Component"};
constexpr QLatin1StringView kProperties{"org.freedesktop.DBus.Properties"};

QDBusMessage call(const QDBusConnection &bus, const QString &service, const QString &path, QLatin1StringView interface,
                  const QString &method, const QVariantList &arguments = {}) {
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, QString(interface), method);
    message.setArguments(arguments);
    return bus.call(message, QDBus::Block, kCallTimeoutMs);
}

QDBusMessage call(const QDBusConnection &bus, const NodeRef &ref, QLatin1StringView interface, const QString &method,
                  const QVariantList &arguments = {}) {
    return call(bus, ref.bus, ref.path, interface, method, arguments);
}

// The error a reply means for the model: an element that is gone, an application that hangs, or the rest.
Error errorOf(const QDBusMessage &reply, const QString &what) {
    const QString name = reply.errorName();
    static const QStringList gone{u"UnknownObject"_s, u"UnknownMethod"_s, u"UnknownInterface"_s, u"UnknownProperty"_s,
                                  u"ServiceUnknown"_s, u"NameHasNoOwner"_s, u"InvalidArgs"_s};
    for (const QString &kind : gone) {
        if (name.endsWith(kind)) return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
    }
    if (name.endsWith(u"NoReply"_s) || name.endsWith(u"Timeout"_s) || name.endsWith(u"TimedOut"_s)) {
        return Error{ErrorCode::Timeout, u"%1: the application did not answer."_s.arg(what)};
    }
    return Error{ErrorCode::Io, u"%1: %2"_s.arg(what, reply.errorMessage().isEmpty() ? name : reply.errorMessage())};
}

Result<QVariant> readProperty(const QDBusConnection &bus, const NodeRef &ref, QLatin1StringView interface, const QString &name) {
    const QDBusMessage reply = call(bus, ref, kProperties, u"Get"_s, {QString(interface), name});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return errorOf(reply, u"Reading "_s + name);
    return reply.arguments().first().value<QDBusVariant>().variant();
}

template <typename T>
T demarshal(const QVariant &value) {
    if (value.canConvert<QDBusArgument>()) return qdbus_cast<T>(value.value<QDBusArgument>());
    return value.value<T>();
}

// A bridge without a cache: the tree from the root through GetChildren, capped so a huge window cannot stall a snapshot.
void walk(const QDBusConnection &bus, const WireRef &ref, QList<atspi::Node> &out, int depth) {
    if (out.size() >= kWalkCap || depth > kWalkDepth) return;
    atspi::Node node;
    node.ref = NodeRef{ref.bus, ref.path.path()};
    if (const Result<QVariant> name = readProperty(bus, node.ref, kAccessible, u"Name"_s)) node.name = name.value().toString();
    if (const Result<QVariant> description = readProperty(bus, node.ref, kAccessible, u"Description"_s)) node.description = description.value().toString();
    if (const Result<QVariant> parent = readProperty(bus, node.ref, kAccessible, u"Parent"_s)) {
        const WireRef wire = demarshal<WireRef>(parent.value());
        node.parent = NodeRef{wire.bus, wire.path.path()};
    }
    if (const QDBusMessage role = call(bus, node.ref, kAccessible, u"GetRole"_s); role.type() == QDBusMessage::ReplyMessage) {
        node.role = role.arguments().value(0).toUInt();
    }
    if (const QDBusMessage state = call(bus, node.ref, kAccessible, u"GetState"_s); state.type() == QDBusMessage::ReplyMessage) {
        const QList<uint> words = demarshal<QList<uint>>(state.arguments().value(0));
        for (qsizetype i = 0; i < std::min<qsizetype>(words.size(), 2); ++i) node.states |= quint64(words.at(i)) << (32 * i);
    }
    if (const QDBusMessage interfaces = call(bus, node.ref, kAccessible, u"GetInterfaces"_s); interfaces.type() == QDBusMessage::ReplyMessage) {
        node.interfaces = demarshal<QStringList>(interfaces.arguments().value(0));
    }
    QList<WireRef> children;
    if (const QDBusMessage reply = call(bus, node.ref, kAccessible, u"GetChildren"_s); reply.type() == QDBusMessage::ReplyMessage) {
        children = demarshal<QList<WireRef>>(reply.arguments().value(0));
    }
    node.childCount = int(children.size());
    for (const WireRef &child : children) node.children.append(NodeRef{child.bus, child.path.path()});
    out.append(node);
    int index = 0;
    for (const WireRef &child : children) {
        if (out.size() >= kWalkCap) break;
        walk(bus, child, out, depth + 1);
        if (!out.isEmpty() && out.last().ref.path == child.path.path()) out.last().indexInParent = index;
        ++index;
    }
}

Result<QList<AtspiApp>> fetchApps(const QString &connectionName) {
    const QDBusConnection bus(connectionName);
    const QDBusMessage reply = call(bus, QString(kRegistry), QString(kRootPath), kAccessible, u"GetChildren"_s);
    if (reply.type() != QDBusMessage::ReplyMessage) return errorOf(reply, u"Listing the applications on the accessibility bus"_s);
    const QList<WireRef> children = demarshal<QList<WireRef>>(reply.arguments().value(0));
    QList<AtspiApp> apps;
    for (const WireRef &child : children) {
        AtspiApp app;
        app.bus = child.bus;
        const NodeRef root{child.bus, child.path.path()};
        if (const Result<QVariant> name = readProperty(bus, root, kAccessible, u"Name"_s)) {
            app.name = name.value().toString();
        } else {
            continue;       // an application that is gone or hangs: not on the screen for us
        }
        if (const Result<QVariant> toolkit = readProperty(bus, root, kApplication, u"ToolkitName"_s)) app.toolkit = toolkit.value().toString();
        const QDBusMessage cache = call(bus, child.bus, QString(kCachePath), kCache, u"GetItems"_s);
        bool cached = false;
        if (cache.type() == QDBusMessage::ReplyMessage && !cache.arguments().isEmpty()) {
            const QVariant items = cache.arguments().first();
            if (items.canConvert<QDBusArgument>()) {
                const Result<QList<atspi::Node>> nodes = atspi::decodeCacheItems(items.value<QDBusArgument>(), cache.signature());
                if (nodes) {
                    app.nodes = nodes.value();
                    cached = true;
                } else {
                    qCDebug(lcApp) << "a11y:" << app.name << nodes.error().message;
                }
            }
        }
        if (!cached) walk(bus, child, app.nodes, 0);
        apps.append(app);
    }
    return apps;
}

Result<QString> readText(const QDBusConnection &bus, const NodeRef &ref, int start, int end) {
    const QDBusMessage reply = call(bus, ref, kText, u"GetText"_s, {start, end});
    if (reply.type() != QDBusMessage::ReplyMessage) return errorOf(reply, u"Reading the text"_s);
    return reply.arguments().value(0).toString();
}

Result<int> characterCount(const QDBusConnection &bus, const NodeRef &ref) {
    const Result<QVariant> count = readProperty(bus, ref, kText, u"CharacterCount"_s);
    if (!count) return count.error();
    return count.value().toInt();
}

Result<void> boolReply(const QDBusMessage &reply, const QString &what, const QString &refused) {
    if (reply.type() != QDBusMessage::ReplyMessage) return errorOf(reply, what);
    if (!reply.arguments().value(0).toBool()) return Error{ErrorCode::Io, refused};
    return {};
}

} // namespace

AtspiScreenReader::AtspiScreenReader(AtspiBus *bus, QObject *parent) : QObject(parent), bus_(bus) {
    atspi::registerCacheTypes();
    pool_.setMaxThreadCount(1);
}

AtspiScreenReader::~AtspiScreenReader() {
    pool_.waitForDone();
}

bool AtspiScreenReader::isAvailable() const {
    return bus_ && bus_->isAvailable();
}

QString AtspiScreenReader::unavailableReason() const {
    return bus_ ? bus_->unavailableReason() : u"No accessibility bus in this copy of Nexa."_s;
}

template <typename T>
void AtspiScreenReader::run(std::function<T()> job, std::function<void(T)> done) {
    if (!isAvailable()) {
        if constexpr (std::is_same_v<T, QHash<atspi::NodeRef, QString>>) {
            done({});
        } else {
            done(Error{ErrorCode::NotSupported, unavailableReason()});
        }
        return;
    }
    const QPointer<AtspiScreenReader> self(this);
    pool_.start([self, job = std::move(job), done = std::move(done)] {
        T result = job();
        if (!self) return;
        QMetaObject::invokeMethod(self, [result = std::move(result), done]() mutable { done(std::move(result)); }, Qt::QueuedConnection);
    });
}

void AtspiScreenReader::apps(std::function<void(Result<QList<AtspiApp>>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<QList<AtspiApp>>>([name] { return fetchApps(name); }, std::move(done));
}

void AtspiScreenReader::texts(const QList<TextRequest> &requests, std::function<void(QHash<atspi::NodeRef, QString>)> done) {
    if (requests.isEmpty()) {
        done({});
        return;
    }
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<QHash<atspi::NodeRef, QString>>([name, requests] {
        const QDBusConnection bus(name);
        QHash<atspi::NodeRef, QString> out;
        for (const TextRequest &request : requests) {
            const Result<int> count = characterCount(bus, request.ref);
            if (!count) continue;
            const int total = std::max(0, count.value());
            const int start = request.fromEnd ? std::max(0, total - request.maxChars) : 0;
            const int end = request.fromEnd ? total : std::min(total, request.maxChars);
            if (const Result<QString> text = readText(bus, request.ref, start, end)) out.insert(request.ref, text.value());
        }
        return out;
    }, std::move(done));
}

void AtspiScreenReader::text(const atspi::NodeRef &ref, int offset, int maxChars, std::function<void(Result<TextSlice>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<TextSlice>>([name, ref, offset, maxChars]() -> Result<TextSlice> {
        const QDBusConnection bus(name);
        const Result<int> count = characterCount(bus, ref);
        if (!count) return count.error();
        const int total = std::max(0, count.value());
        const int start = std::clamp(offset, 0, total);
        const int end = std::min(total, start + std::max(0, maxChars));
        const Result<QString> text = readText(bus, ref, start, end);
        if (!text) return text.error();
        return TextSlice{text.value(), start, total};
    }, std::move(done));
}

void AtspiScreenReader::actions(const atspi::NodeRef &ref, std::function<void(Result<QStringList>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<QStringList>>([name, ref]() -> Result<QStringList> {
        const QDBusConnection bus(name);
        QStringList names;
        const QDBusMessage reply = call(bus, ref, kAction, u"GetActions"_s);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            const QDBusArgument list = reply.arguments().first().value<QDBusArgument>();
            list.beginArray();
            while (!list.atEnd()) {
                QString action, description, keys;
                list.beginStructure();
                list >> action >> description >> keys;
                list.endStructure();
                names.append(action);
            }
            list.endArray();
            return names;
        }
        // An older bridge: the count, then each name.
        const Result<QVariant> count = readProperty(bus, ref, kAction, u"NActions"_s);
        if (!count) {
            // An element without the Action interface has no actions — that is an answer, not a failure.
            if (count.error().code == ErrorCode::NotFound && reply.errorName().endsWith(u"UnknownInterface"_s)) return QStringList();
            return count.error();
        }
        for (int i = 0; i < count.value().toInt(); ++i) {
            const QDBusMessage one = call(bus, ref, kAction, u"GetName"_s, {i});
            if (one.type() == QDBusMessage::ReplyMessage) names.append(one.arguments().value(0).toString());
        }
        return names;
    }, std::move(done));
}

void AtspiScreenReader::doAction(const atspi::NodeRef &ref, int index, std::function<void(Result<void>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<void>>([name, ref, index]() -> Result<void> {
        const QDBusConnection bus(name);
        return boolReply(call(bus, ref, kAction, u"DoAction"_s, {index}), u"Performing the action"_s,
                         u"The application did not perform the action."_s);
    }, std::move(done));
}

void AtspiScreenReader::grabFocus(const atspi::NodeRef &ref, std::function<void(Result<void>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<void>>([name, ref]() -> Result<void> {
        const QDBusConnection bus(name);
        return boolReply(call(bus, ref, kComponent, u"GrabFocus"_s), u"Focusing the element"_s,
                         u"The application did not give that element the focus."_s);
    }, std::move(done));
}

void AtspiScreenReader::setText(const atspi::NodeRef &ref, const QString &text, std::function<void(Result<void>)> done) {
    const QString name = bus_ ? bus_->connectionName() : QString();
    run<Result<void>>([name, ref, text]() -> Result<void> {
        const QDBusConnection bus(name);
        return boolReply(call(bus, ref, kEditableText, u"SetTextContents"_s, {text}), u"Setting the text"_s,
                         u"The application did not take the text."_s);
    }, std::move(done));
}

} // namespace nexa
