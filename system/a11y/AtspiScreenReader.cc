#include "system/a11y/AtspiScreenReader.h"

#include "core/Logging.h"
#include "system/a11y/AtspiBus.h"

#include <QPointer>

#ifdef Q_OS_UNIX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <QCoreApplication>
#include <QFileInfo>
#include <QSet>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <initguid.h>
#include <uiautomation.h>
#endif

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

#ifdef Q_OS_UNIX
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
#elif defined(Q_OS_WIN)
namespace {

constexpr int kWalkCap = 600;
constexpr int kWalkDepth = 48;
constexpr QLatin1StringView kRootPath{"/org/a11y/atspi/accessible/root"};

template <typename T>
struct ComPtr {
    T *ptr{nullptr};
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    ComPtr &operator=(ComPtr &&other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }
    void reset(T *next = nullptr) {
        if (ptr) ptr->Release();
        ptr = next;
    }
    T **put() {
        reset();
        return &ptr;
    }
    T *operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

struct ComScope {
    HRESULT hr{E_FAIL};
    ComScope() : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (hr == S_OK || hr == S_FALSE) ::CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct BstrGuard {
    BSTR value{nullptr};
    ~BstrGuard() {
        if (value) ::SysFreeString(value);
    }
    QString toQString() const {
        if (!value) return {};
        return QString::fromWCharArray(value, static_cast<int>(::SysStringLen(value)));
    }
};

struct VariantGuard {
    VARIANT var{};
    VariantGuard() { ::VariantInit(&var); }
    ~VariantGuard() { ::VariantClear(&var); }
};

struct SafeArrayGuard {
    SAFEARRAY *arr{nullptr};
    ~SafeArrayGuard() {
        if (arr) ::SafeArrayDestroy(arr);
    }
};

QString runtimeIdKey(SAFEARRAY *arr) {
    if (!arr) return {};
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(::SafeArrayGetLBound(arr, 1, &lower)) || FAILED(::SafeArrayGetUBound(arr, 1, &upper))) return {};
    if (upper < lower) return {};
    void *raw = nullptr;
    if (FAILED(::SafeArrayAccessData(arr, &raw))) return {};
    const auto *ints = static_cast<const int *>(raw);
    QStringList parts;
    const LONG count = upper - lower + 1;
    parts.reserve(count);
    for (LONG i = 0; i < count; ++i) parts.append(QString::number(ints[i]));
    ::SafeArrayUnaccessData(arr);
    return parts.join(u'.');
}

QString encodeWinPath(HWND topHwnd, const QString &rtKey) {
    return u"/win/%1/%2"_s.arg(static_cast<quintptr>(reinterpret_cast<uintptr_t>(topHwnd)), 0, 16).arg(rtKey);
}

bool decodeWinPath(const QString &path, HWND *topHwndOut, QList<int> *rtOut) {
    const QStringList parts = path.split(u'/', Qt::SkipEmptyParts);
    if (parts.size() != 3 || parts.at(0) != u"win"_s) return false;
    bool okHwnd = false;
    const quintptr rawHwnd = parts.at(1).toULongLong(&okHwnd, 16);
    if (!okHwnd) return false;
    const QStringList nums = parts.at(2).split(u'.', Qt::SkipEmptyParts);
    if (nums.isEmpty()) return false;
    QList<int> rt;
    rt.reserve(nums.size());
    for (const QString &num : nums) {
        bool okInt = false;
        const int v = num.toInt(&okInt);
        if (!okInt) return false;
        rt.append(v);
    }
    *topHwndOut = reinterpret_cast<HWND>(static_cast<uintptr_t>(rawHwnd));
    *rtOut = std::move(rt);
    return true;
}

uint mapControlType(CONTROLTYPEID ct, bool isPassword) {
    using atspi::Role;
    if (isPassword) return static_cast<uint>(Role::PasswordText);
    switch (ct) {
    case UIA_ButtonControlTypeId: return static_cast<uint>(Role::PushButton);
    case UIA_CheckBoxControlTypeId: return static_cast<uint>(Role::CheckBox);
    case UIA_ComboBoxControlTypeId: return static_cast<uint>(Role::ComboBox);
    case UIA_EditControlTypeId: return static_cast<uint>(Role::Entry);
    case UIA_HyperlinkControlTypeId: return static_cast<uint>(Role::Link);
    case UIA_ImageControlTypeId: return static_cast<uint>(Role::Image);
    case UIA_ListItemControlTypeId: return static_cast<uint>(Role::ListItem);
    case UIA_ListControlTypeId: return static_cast<uint>(Role::List);
    case UIA_MenuControlTypeId: return static_cast<uint>(Role::Menu);
    case UIA_MenuBarControlTypeId: return static_cast<uint>(Role::MenuBar);
    case UIA_MenuItemControlTypeId: return static_cast<uint>(Role::MenuItem);
    case UIA_RadioButtonControlTypeId: return static_cast<uint>(Role::RadioButton);
    case UIA_ScrollBarControlTypeId: return static_cast<uint>(Role::ScrollBar);
    case UIA_SliderControlTypeId: return static_cast<uint>(Role::Slider);
    case UIA_SpinnerControlTypeId: return static_cast<uint>(Role::SpinButton);
    case UIA_StatusBarControlTypeId: return static_cast<uint>(Role::StatusBar);
    case UIA_TabControlTypeId: return static_cast<uint>(Role::PageTabList);
    case UIA_TabItemControlTypeId: return static_cast<uint>(Role::PageTab);
    case UIA_TextControlTypeId: return static_cast<uint>(Role::Text);
    case UIA_ToolBarControlTypeId: return static_cast<uint>(Role::ToolBar);
    case UIA_ToolTipControlTypeId: return static_cast<uint>(Role::ToolTip);
    case UIA_TreeItemControlTypeId: return static_cast<uint>(Role::TreeItem);
    case UIA_WindowControlTypeId: return static_cast<uint>(Role::Window);
    case UIA_PaneControlTypeId:
    case UIA_GroupControlTypeId:
    case UIA_CustomControlTypeId: return static_cast<uint>(Role::Panel);
    case UIA_DocumentControlTypeId: return static_cast<uint>(Role::DocumentText);
    case UIA_SeparatorControlTypeId: return static_cast<uint>(Role::Separator);
    default: return static_cast<uint>(Role::Unknown);
    }
}

void setBit(quint64 &mask, atspi::State state) {
    mask |= quint64(1) << static_cast<int>(state);
}

QString processDisplayName(DWORD pid) {
    if (pid == 0) return u"Windows"_s;
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return u"pid-%1"_s.arg(pid);
    wchar_t buf[MAX_PATH]{};
    DWORD len = MAX_PATH;
    QString name;
    if (::QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        name = QFileInfo(QString::fromWCharArray(buf, static_cast<int>(len))).completeBaseName();
    }
    ::CloseHandle(proc);
    return name.isEmpty() ? u"pid-%1"_s.arg(pid) : name;
}

Result<ComPtr<IUIAutomation>> createAutomation() {
    ComPtr<IUIAutomation> automation;
    const HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                          reinterpret_cast<void **>(automation.put()));
    if (FAILED(hr) || !automation) {
        return Error{ErrorCode::NotSupported,
                     u"Windows UI Automation could not be started (0x%1)."_s.arg(static_cast<quint32>(hr), 8, 16, u'0')};
    }
    return automation;
}

Result<ComPtr<IUIAutomationElement>> resolveElement(IUIAutomation *automation, const atspi::NodeRef &ref) {
    HWND topHwnd = nullptr;
    QList<int> targetRt;
    if (!decodeWinPath(ref.path, &topHwnd, &targetRt)) {
        return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
    }

    ComPtr<IUIAutomationElement> root;
    if (topHwnd) {
        if (!::IsWindow(topHwnd) || FAILED(automation->ElementFromHandle(topHwnd, root.put())) || !root) {
            return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
        }
    } else {
        if (FAILED(automation->GetRootElement(root.put())) || !root) {
            return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
        }
    }

    SAFEARRAY *sa = ::SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(targetRt.size()));
    if (!sa) return Error{ErrorCode::Io, u"Out of memory looking up UI element."_s};
    void *raw = nullptr;
    if (SUCCEEDED(::SafeArrayAccessData(sa, &raw))) {
        auto *dst = static_cast<int *>(raw);
        for (qsizetype i = 0; i < targetRt.size(); ++i) dst[i] = targetRt.at(i);
        ::SafeArrayUnaccessData(sa);
    }

    VariantGuard rtVar;
    rtVar.var.vt = VT_ARRAY | VT_I4;
    rtVar.var.parray = sa;

    SafeArrayGuard rootRt;
    if (SUCCEEDED(root->GetRuntimeId(&rootRt.arr)) && rootRt.arr) {
        BOOL same = FALSE;
        if (SUCCEEDED(automation->CompareRuntimeIds(rootRt.arr, sa, &same)) && same) return root;
    }

    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(automation->CreatePropertyCondition(UIA_RuntimeIdPropertyId, rtVar.var, cond.put())) || !cond) {
        return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
    }
    ComPtr<IUIAutomationElement> found;
    if (FAILED(root->FindFirst(TreeScope_Subtree, cond.ptr, found.put())) || !found) {
        return Error{ErrorCode::NotFound, u"That element is gone: take screen_snapshot again."_s};
    }
    return found;
}

QString readElementText(IUIAutomationElement *elem, bool *isPasswordOut = nullptr) {
    BOOL pwd = FALSE;
    if (SUCCEEDED(elem->get_CurrentIsPassword(&pwd)) && pwd) {
        if (isPasswordOut) *isPasswordOut = true;
        return {};
    }
    if (isPasswordOut) *isPasswordOut = false;

    ComPtr<IUnknown> unk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_TextPatternId, unk.put())) && unk) {
        ComPtr<IUIAutomationTextPattern> textPat;
        if (SUCCEEDED(unk->QueryInterface(IID_IUIAutomationTextPattern, reinterpret_cast<void **>(textPat.put())))
            && textPat) {
            ComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(textPat->get_DocumentRange(range.put())) && range) {
                BstrGuard content;
                if (SUCCEEDED(range->GetText(-1, &content.value))) return content.toQString();
            }
        }
    }
    unk.reset();
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_ValuePatternId, unk.put())) && unk) {
        ComPtr<IUIAutomationValuePattern> valPat;
        if (SUCCEEDED(unk->QueryInterface(IID_IUIAutomationValuePattern, reinterpret_cast<void **>(valPat.put())))
            && valPat) {
            BstrGuard val;
            if (SUCCEEDED(valPat->get_CurrentValue(&val.value))) return val.toQString();
        }
    }
    BstrGuard name;
    if (SUCCEEDED(elem->get_CurrentName(&name.value))) return name.toQString();
    return {};
}

QStringList elementActions(IUIAutomationElement *elem) {
    QStringList list;
    const auto hasPat = [elem](PATTERNID pat) {
        ComPtr<IUnknown> unk;
        return SUCCEEDED(elem->GetCurrentPattern(pat, unk.put())) && unk;
    };
    if (hasPat(UIA_InvokePatternId)) list.append(u"invoke"_s);
    if (hasPat(UIA_TogglePatternId)) list.append(u"toggle"_s);
    if (hasPat(UIA_SelectionItemPatternId)) list.append(u"select"_s);
    ComPtr<IUnknown> ecUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_ExpandCollapsePatternId, ecUnk.put())) && ecUnk) {
        ComPtr<IUIAutomationExpandCollapsePattern> ec;
        if (SUCCEEDED(ecUnk->QueryInterface(IID_IUIAutomationExpandCollapsePattern,
                                            reinterpret_cast<void **>(ec.put())))
            && ec) {
            ExpandCollapseState st = ExpandCollapseState_LeafNode;
            if (SUCCEEDED(ec->get_CurrentExpandCollapseState(&st))) {
                list.append(st == ExpandCollapseState_Expanded ? u"collapse"_s : u"expand"_s);
            } else {
                list.append(u"expand"_s);
            }
        }
    }
    return list;
}

atspi::Node buildNode(IUIAutomationElement *elem, HWND topHwnd, const QString &busId, const QString &parentPath,
                      int indexInParent, bool isWindow, bool isForeground) {
    atspi::Node node;
    SafeArrayGuard rt;
    QString rtKey;
    if (SUCCEEDED(elem->GetRuntimeId(&rt.arr)) && rt.arr) rtKey = runtimeIdKey(rt.arr);
    if (rtKey.isEmpty()) rtKey = u"idx%1"_s.arg(indexInParent);
    node.ref = atspi::NodeRef{busId, encodeWinPath(topHwnd, rtKey)};
    node.parent = atspi::NodeRef{busId, parentPath};
    node.indexInParent = indexInParent;

    BstrGuard nameBstr;
    if (SUCCEEDED(elem->get_CurrentName(&nameBstr.value))) node.name = nameBstr.toQString();
    BstrGuard helpBstr;
    if (SUCCEEDED(elem->get_CurrentHelpText(&helpBstr.value))) node.description = helpBstr.toQString();

    BOOL isPwd = FALSE;
    elem->get_CurrentIsPassword(&isPwd);
    CONTROLTYPEID ct = UIA_CustomControlTypeId;
    elem->get_CurrentControlType(&ct);
    node.role = isWindow ? static_cast<uint>(atspi::Role::Frame) : mapControlType(ct, isPwd != FALSE);

    BOOL offscreen = FALSE;
    elem->get_CurrentIsOffscreen(&offscreen);
    if (!offscreen) {
        setBit(node.states, atspi::State::Showing);
        setBit(node.states, atspi::State::Visible);
    } else if (isWindow) {
        setBit(node.states, atspi::State::Showing);
        setBit(node.states, atspi::State::Visible);
    }
    if (isWindow && isForeground) setBit(node.states, atspi::State::Active);

    BOOL enabled = TRUE;
    if (SUCCEEDED(elem->get_CurrentIsEnabled(&enabled)) && enabled) {
        setBit(node.states, atspi::State::Enabled);
        setBit(node.states, atspi::State::Sensitive);
    }
    BOOL focusable = FALSE;
    if (SUCCEEDED(elem->get_CurrentIsKeyboardFocusable(&focusable)) && focusable) {
        setBit(node.states, atspi::State::Focusable);
    }
    BOOL focused = FALSE;
    if (SUCCEEDED(elem->get_CurrentHasKeyboardFocus(&focused)) && focused) {
        setBit(node.states, atspi::State::Focused);
    }

    if (!elementActions(elem).isEmpty()) node.interfaces.append(u"org.a11y.atspi.Action"_s);

    ComPtr<IUnknown> textUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_TextPatternId, textUnk.put())) && textUnk) {
        node.interfaces.append(u"org.a11y.atspi.Text"_s);
    }
    ComPtr<IUnknown> valUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_ValuePatternId, valUnk.put())) && valUnk) {
        ComPtr<IUIAutomationValuePattern> valPat;
        if (SUCCEEDED(valUnk->QueryInterface(IID_IUIAutomationValuePattern, reinterpret_cast<void **>(valPat.put())))
            && valPat) {
            if (!node.interfaces.contains(u"org.a11y.atspi.Text"_s)) node.interfaces.append(u"org.a11y.atspi.Text"_s);
            BOOL ro = TRUE;
            if (SUCCEEDED(valPat->get_CurrentIsReadOnly(&ro)) && !ro
                && (ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId || ct == UIA_ComboBoxControlTypeId)) {
                setBit(node.states, atspi::State::Editable);
                node.interfaces.append(u"org.a11y.atspi.EditableText"_s);
            }
        }
    }
    ComPtr<IUnknown> toggleUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_TogglePatternId, toggleUnk.put())) && toggleUnk) {
        ComPtr<IUIAutomationTogglePattern> togglePat;
        if (SUCCEEDED(toggleUnk->QueryInterface(IID_IUIAutomationTogglePattern,
                                                reinterpret_cast<void **>(togglePat.put())))
            && togglePat) {
            setBit(node.states, atspi::State::Checkable);
            ToggleState ts = ToggleState_Off;
            if (SUCCEEDED(togglePat->get_CurrentToggleState(&ts)) && ts == ToggleState_On) {
                setBit(node.states, atspi::State::Checked);
            }
        }
    }
    ComPtr<IUnknown> ecUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_ExpandCollapsePatternId, ecUnk.put())) && ecUnk) {
        ComPtr<IUIAutomationExpandCollapsePattern> ecPat;
        if (SUCCEEDED(ecUnk->QueryInterface(IID_IUIAutomationExpandCollapsePattern,
                                            reinterpret_cast<void **>(ecPat.put())))
            && ecPat) {
            ExpandCollapseState ecs = ExpandCollapseState_LeafNode;
            if (SUCCEEDED(ecPat->get_CurrentExpandCollapseState(&ecs)) && ecs != ExpandCollapseState_LeafNode) {
                setBit(node.states, atspi::State::Expandable);
                if (ecs == ExpandCollapseState_Expanded) setBit(node.states, atspi::State::Expanded);
                else setBit(node.states, atspi::State::Collapsed);
            }
        }
    }
    ComPtr<IUnknown> selUnk;
    if (SUCCEEDED(elem->GetCurrentPattern(UIA_SelectionItemPatternId, selUnk.put())) && selUnk) {
        ComPtr<IUIAutomationSelectionItemPattern> selPat;
        if (SUCCEEDED(selUnk->QueryInterface(IID_IUIAutomationSelectionItemPattern,
                                             reinterpret_cast<void **>(selPat.put())))
            && selPat) {
            setBit(node.states, atspi::State::Selectable);
            BOOL selected = FALSE;
            if (SUCCEEDED(selPat->get_CurrentIsSelected(&selected)) && selected) {
                setBit(node.states, atspi::State::Selected);
            }
        }
    }
    return node;
}

void walkUiaSubtree(IUIAutomationTreeWalker *walker, IUIAutomationElement *parentElem, HWND topHwnd,
                    const QString &busId, const QString &parentPath, QList<atspi::Node> &out, int depth, int maxNodes) {
    if (depth > kWalkDepth || out.size() >= maxNodes) return;
    ComPtr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(parentElem, child.put()))) return;
    int idx = 0;
    while (child && out.size() < maxNodes) {
        atspi::Node node = buildNode(child.ptr, topHwnd, busId, parentPath, idx++, false, false);
        const QString myPath = node.ref.path;
        out.append(node);
        walkUiaSubtree(walker, child.ptr, topHwnd, busId, myPath, out, depth + 1, maxNodes);
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetNextSiblingElement(child.ptr, next.put()))) break;
        child = std::move(next);
    }
}

bool isWinShellClass(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64]{};
    if (::GetClassNameW(hwnd, cls, 64) <= 0) return false;
    const QString name = QString::fromWCharArray(cls);
    return name == u"Progman"_s || name == u"WorkerW"_s || name == u"Shell_TrayWnd"_s
           || name == u"Shell_SecondaryTrayWnd"_s;
}

bool isWinShellProcess(const QString &procName) {
    static const QSet<QString> hosts{u"SearchHost"_s, u"StartMenuExperienceHost"_s, u"ShellExperienceHost"_s,
                                     u"TextInputHost"_s, u"LockApp"_s, u"Widgets"_s, u"ScreenClippingHost"_s,
                                     u"ShellHost"_s};
    return hosts.contains(procName);
}

HWND topNonNexaWindow(DWORD selfPid) {
    for (HWND hwnd = ::GetTopWindow(nullptr); hwnd != nullptr; hwnd = ::GetWindow(hwnd, GW_HWNDNEXT)) {
        if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd)) continue;
        if (isWinShellClass(hwnd)) continue;
        const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0) continue;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == selfPid) continue;
        if (isWinShellProcess(processDisplayName(pid))) continue;
        if (::GetWindowTextLengthW(hwnd) <= 0) continue;
        RECT rc{};
        if (::GetWindowRect(hwnd, &rc) && (rc.right - rc.left) > 10 && (rc.bottom - rc.top) > 10) {
            return hwnd;
        }
    }
    return nullptr;
}

void activateWindow(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return;
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    const HWND fg = ::GetForegroundWindow();
    const DWORD curTid = ::GetCurrentThreadId();
    const DWORD fgTid = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD targetTid = ::GetWindowThreadProcessId(hwnd, nullptr);
    const bool attachFg = fgTid != 0 && fgTid != curTid && ::AttachThreadInput(curTid, fgTid, TRUE) != FALSE;
    const bool attachTarget =
        targetTid != 0 && targetTid != curTid && targetTid != fgTid && ::AttachThreadInput(curTid, targetTid, TRUE) != FALSE;
    ::BringWindowToTop(hwnd);
    ::SetForegroundWindow(hwnd);
    if (attachTarget) ::AttachThreadInput(curTid, targetTid, FALSE);
    if (attachFg) ::AttachThreadInput(curTid, fgTid, FALSE);
}

} // namespace

AtspiScreenReader::AtspiScreenReader(AtspiBus *bus, QObject *parent) : QObject(parent), bus_(bus) {
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
    const QPointer<AtspiScreenReader> guard(this);
    pool_.start([guard, job = std::move(job), done = std::move(done)]() mutable {
        T result = job();
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [done = std::move(done), result = std::move(result)]() mutable {
            done(std::move(result));
        }, Qt::QueuedConnection);
    });
}

void AtspiScreenReader::apps(std::function<void(Result<QList<AtspiApp>>)> done) {
    run<Result<QList<AtspiApp>>>([]() -> Result<QList<AtspiApp>> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();

        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation.value()->GetRootElement(root.put())) || !root) {
            return Error{ErrorCode::Io, u"Cannot get desktop root from UI Automation."_s};
        }
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation.value()->get_ControlViewWalker(walker.put())) || !walker) {
            return Error{ErrorCode::Io, u"Cannot create UI Automation walker."_s};
        }

        const DWORD selfPid = ::GetCurrentProcessId();
        const HWND fgHwnd = ::GetForegroundWindow();
        DWORD fgPid = 0;
        if (fgHwnd) ::GetWindowThreadProcessId(fgHwnd, &fgPid);
        // When the user submits a request from Nexa's own window, Nexa itself is the foreground window.
        // Fall back to the top non-Nexa app window in Z-order so screen_snapshot still inspects it fully.
        HWND effectiveActive = fgHwnd;
        if (!effectiveActive || fgPid == selfPid || isWinShellClass(effectiveActive)
            || isWinShellProcess(processDisplayName(fgPid))) {
            effectiveActive = topNonNexaWindow(selfPid);
        }
        QHash<DWORD, int> appIndexByPid;
        QList<AtspiApp> apps;

        ComPtr<IUIAutomationElement> win;
        if (FAILED(walker->GetFirstChildElement(root.ptr, win.put()))) return apps;
        int winIndex = 0;
        int backgroundWalked = 0;
        while (win) {
            UIA_HWND uiaHwnd = nullptr;
            win->get_CurrentNativeWindowHandle(&uiaHwnd);
            const HWND hwnd = reinterpret_cast<HWND>(uiaHwnd);
            int rawPid = 0;
            win->get_CurrentProcessId(&rawPid);
            const DWORD pid = rawPid > 0 ? static_cast<DWORD>(rawPid) : 0;

            bool skip = (pid != 0 && pid == selfPid);
            if (!skip && hwnd) {
                if (!::IsWindowVisible(hwnd) || ::IsIconic(hwnd) || isWinShellClass(hwnd)) skip = true;
            }
            if (!skip) {
                const QString busId = u"win:%1"_s.arg(pid);
                if (!appIndexByPid.contains(pid)) {
                    appIndexByPid.insert(pid, int(apps.size()));
                    AtspiApp app;
                    app.bus = busId;
                    app.name = processDisplayName(pid);
                    app.toolkit = u"UIA"_s;
                    apps.append(std::move(app));
                }
                AtspiApp &app = apps[appIndexByPid.value(pid)];
                const bool isActive = (hwnd != nullptr && hwnd == effectiveActive);
                atspi::Node winNode =
                    buildNode(win.ptr, hwnd, busId, QString(kRootPath), winIndex++, true, isActive);
                const QString winPath = winNode.ref.path;
                QList<atspi::Node> nodes;
                nodes.append(std::move(winNode));
                if (isActive) {
                    walkUiaSubtree(walker.ptr, win.ptr, hwnd, busId, winPath, nodes, 1, kWalkCap);
                } else if (backgroundWalked < 6 && !isWinShellProcess(app.name)) {
                    ++backgroundWalked;
                    walkUiaSubtree(walker.ptr, win.ptr, hwnd, busId, winPath, nodes, 1, 120);
                } else {
                    walkUiaSubtree(walker.ptr, win.ptr, hwnd, busId, winPath, nodes, kWalkDepth - 1, 16);
                }
                app.nodes.append(std::move(nodes));
            }
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(win.ptr, next.put()))) break;
            win = std::move(next);
        }
        return apps;
    }, std::move(done));
}

void AtspiScreenReader::texts(const QList<TextRequest> &requests,
                              std::function<void(QHash<atspi::NodeRef, QString>)> done) {
    run<QHash<atspi::NodeRef, QString>>([requests]() -> QHash<atspi::NodeRef, QString> {
        QHash<atspi::NodeRef, QString> out;
        if (requests.isEmpty()) return out;
        ComScope com;
        if (!com.usable()) return out;
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return out;
        for (const TextRequest &req : requests) {
            const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, req.ref);
            if (!elem) continue;
            bool isPwd = false;
            QString s = readElementText(elem.value().ptr, &isPwd);
            if (isPwd || s.isEmpty()) continue;
            if (s.size() > req.maxChars) {
                s = req.fromEnd ? s.right(req.maxChars) : s.left(req.maxChars);
            }
            out.insert(req.ref, std::move(s));
        }
        return out;
    }, std::move(done));
}

void AtspiScreenReader::text(const atspi::NodeRef &ref, int offset, int maxChars,
                             std::function<void(Result<TextSlice>)> done) {
    run<Result<TextSlice>>([ref, offset, maxChars]() -> Result<TextSlice> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();
        const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, ref);
        if (!elem) return elem.error();
        bool isPwd = false;
        const QString all = readElementText(elem.value().ptr, &isPwd);
        if (isPwd) return Error{ErrorCode::PermissionDenied, u"Nexa does not read password fields."_s};
        const int total = int(all.size());
        const int clampedOffset = std::clamp(offset, 0, total);
        return TextSlice{all.mid(clampedOffset, std::max(0, maxChars)), clampedOffset, total};
    }, std::move(done));
}

void AtspiScreenReader::actions(const atspi::NodeRef &ref, std::function<void(Result<QStringList>)> done) {
    run<Result<QStringList>>([ref]() -> Result<QStringList> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();
        const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, ref);
        if (!elem) return elem.error();
        return elementActions(elem.value().ptr);
    }, std::move(done));
}

void AtspiScreenReader::doAction(const atspi::NodeRef &ref, int index, std::function<void(Result<void>)> done) {
    run<Result<void>>([ref, index]() -> Result<void> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();
        const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, ref);
        if (!elem) return elem.error();
        const QStringList names = elementActions(elem.value().ptr);
        if (index < 0 || index >= names.size()) {
            return Error{ErrorCode::InvalidArgument, u"The element has no action at index %1."_s.arg(index)};
        }
        const QString &act = names.at(index);
        if (act == u"invoke"_s) {
            ComPtr<IUnknown> unk;
            ComPtr<IUIAutomationInvokePattern> pat;
            if (SUCCEEDED(elem.value()->GetCurrentPattern(UIA_InvokePatternId, unk.put())) && unk
                && SUCCEEDED(unk->QueryInterface(IID_IUIAutomationInvokePattern, reinterpret_cast<void **>(pat.put())))
                && pat && SUCCEEDED(pat->Invoke())) {
                return {};
            }
        } else if (act == u"toggle"_s) {
            ComPtr<IUnknown> unk;
            ComPtr<IUIAutomationTogglePattern> pat;
            if (SUCCEEDED(elem.value()->GetCurrentPattern(UIA_TogglePatternId, unk.put())) && unk
                && SUCCEEDED(unk->QueryInterface(IID_IUIAutomationTogglePattern, reinterpret_cast<void **>(pat.put())))
                && pat && SUCCEEDED(pat->Toggle())) {
                return {};
            }
        } else if (act == u"select"_s) {
            ComPtr<IUnknown> unk;
            ComPtr<IUIAutomationSelectionItemPattern> pat;
            if (SUCCEEDED(elem.value()->GetCurrentPattern(UIA_SelectionItemPatternId, unk.put())) && unk
                && SUCCEEDED(unk->QueryInterface(IID_IUIAutomationSelectionItemPattern,
                                                 reinterpret_cast<void **>(pat.put())))
                && pat && SUCCEEDED(pat->Select())) {
                return {};
            }
        } else if (act == u"expand"_s || act == u"collapse"_s) {
            ComPtr<IUnknown> unk;
            ComPtr<IUIAutomationExpandCollapsePattern> pat;
            if (SUCCEEDED(elem.value()->GetCurrentPattern(UIA_ExpandCollapsePatternId, unk.put())) && unk
                && SUCCEEDED(unk->QueryInterface(IID_IUIAutomationExpandCollapsePattern,
                                                 reinterpret_cast<void **>(pat.put())))
                && pat) {
                const HRESULT hr = (act == u"collapse"_s) ? pat->Collapse() : pat->Expand();
                if (SUCCEEDED(hr)) return {};
            }
        }
        return Error{ErrorCode::Io, u"The application did not perform the action."_s};
    }, std::move(done));
}

void AtspiScreenReader::grabFocus(const atspi::NodeRef &ref, std::function<void(Result<void>)> done) {
    run<Result<void>>([ref]() -> Result<void> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();
        const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, ref);
        if (!elem) return elem.error();
        HWND topHwnd = nullptr;
        QList<int> unusedRt;
        if (decodeWinPath(ref.path, &topHwnd, &unusedRt) && topHwnd) {
            activateWindow(topHwnd);
        }
        if (SUCCEEDED(elem.value()->SetFocus())) return {};
        return Error{ErrorCode::Io, u"The application did not give that element the focus."_s};
    }, std::move(done));
}

void AtspiScreenReader::setText(const atspi::NodeRef &ref, const QString &text, std::function<void(Result<void>)> done) {
    run<Result<void>>([ref, text]() -> Result<void> {
        ComScope com;
        if (!com.usable()) return Error{ErrorCode::NotSupported, u"Windows COM unavailable."_s};
        const Result<ComPtr<IUIAutomation>> automation = createAutomation();
        if (!automation) return automation.error();
        const Result<ComPtr<IUIAutomationElement>> elem = resolveElement(automation.value().ptr, ref);
        if (!elem) return elem.error();
        ComPtr<IUnknown> unk;
        ComPtr<IUIAutomationValuePattern> pat;
        if (SUCCEEDED(elem.value()->GetCurrentPattern(UIA_ValuePatternId, unk.put())) && unk
            && SUCCEEDED(unk->QueryInterface(IID_IUIAutomationValuePattern, reinterpret_cast<void **>(pat.put())))
            && pat) {
            const std::wstring wide = text.toStdWString();
            BSTR bstr = ::SysAllocString(wide.c_str());
            if (!bstr) return Error{ErrorCode::Io, u"Out of memory setting text."_s};
            const HRESULT hr = pat->SetValue(bstr);
            ::SysFreeString(bstr);
            if (SUCCEEDED(hr)) return {};
        }
        return Error{ErrorCode::Io, u"The application did not take the text."_s};
    }, std::move(done));
}
#else
AtspiScreenReader::AtspiScreenReader(AtspiBus *bus, QObject *parent) : QObject(parent), bus_(bus) {}
AtspiScreenReader::~AtspiScreenReader() = default;

bool AtspiScreenReader::isAvailable() const { return false; }

QString AtspiScreenReader::unavailableReason() const {
    return bus_ ? bus_->unavailableReason() : u"No accessibility bus in this copy of Nexa."_s;
}

void AtspiScreenReader::apps(std::function<void(Result<QList<AtspiApp>>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}

void AtspiScreenReader::texts(const QList<TextRequest> &, std::function<void(QHash<atspi::NodeRef, QString>)> done) {
    done({});
}

void AtspiScreenReader::text(const atspi::NodeRef &, int, int, std::function<void(Result<TextSlice>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}

void AtspiScreenReader::actions(const atspi::NodeRef &, std::function<void(Result<QStringList>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}

void AtspiScreenReader::doAction(const atspi::NodeRef &, int, std::function<void(Result<void>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}

void AtspiScreenReader::grabFocus(const atspi::NodeRef &, std::function<void(Result<void>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}

void AtspiScreenReader::setText(const atspi::NodeRef &, const QString &, std::function<void(Result<void>)> done) {
    done(Error{ErrorCode::NotSupported, unavailableReason()});
}
#endif

} // namespace nexa
