# The screen as text Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A model that cannot see images reads the desktop and the phone as a text outline with refs and acts on the
refs, as it already does in the browser through Playwright.

**Architecture:** `system/a11y/` reads the desktop's accessibility bus (AT-SPI2) through QtDBus — pure decoding and
outline building, a reader Strategy with a fake for tests, five `screen_*` tools gated by Share screen.
`system/android/UiHierarchy` turns `uiautomator dump` into the same kind of outline; the phone tools gain refs. The fast
path swaps a picture tool for its text twin when the model is blind (`IntentMatch::textToolName`, decided in
`TaskRunner`).

**Tech Stack:** C++20, Qt 6.11 (Core, DBus, Gui, Widgets), Catch2 v3, AT-SPI2 (at-spi2-core 2.61 on this machine),
Android `uiautomator`.

**Spec:** `docs/superpowers/specs/2026-11-12-screen-as-text-design.md`

## Global Constraints

- `core/` has no Qt Widgets and no syscalls; `system/` may use QtDBus (it links `Qt6::DBus` already).
- Refs: desktop `s<n>`, phone `p<n>` (Playwright's are `e<n>`). Outline caps: 300 lines / 12 000 characters; a text
  excerpt 160 characters, a terminal's tail 400; `screen_read` at most 20 000 characters; `screen_wait_for` /
  `android_wait_for` poll every 500 ms, at most 30 000 ms.
- `screen_snapshot`, `screen_read`, `screen_wait_for`, `android_snapshot`, `android_wait_for`: `mutating = false`.
  `screen_click` / `screen_type`: `privileged = true`, `risk` "clicks for you" / "types for you".
- The five `screen_*` tools are withheld with `take_screenshot` while Share screen is off. Password text is never read.
- Nexa never sets `org.a11y.Status IsEnabled` except from the Settings checkbox.
- Every file listed in its `CMakeLists.txt`; class comments name the pattern; `Result<T>`, `u"…"_s`; nothing blocks
  the GUI thread (D-Bus calls on a worker thread).

## Review Focus

1. A bridge that answers `GetItems` with a D-Bus error or an unexpected signature → that app is listed as "exposes
   nothing", the others still shown (Task 1 decodes both signatures; Task 3's reader falls back to `GetChildren`).
2. A ref from a previous snapshot whose node is gone → "that element is gone: take screen_snapshot again", never a
   crash or an action on another element (Task 3 test "a stale ref").
3. The user asks "what's on my screen?" while Share screen is off and the model is blind → the text snapshot runs
   (the exception applies to the text as to the picture) (Task 5 test).
4. `uiautomator dump` printing "ERROR: could not get idle state" (an animating screen) → the error names it and says
   to try again; a dump with `<hierarchy>` but no node → "the phone showed an empty screen" (Task 4 tests).
5. Share screen switched off while a `screen_wait_for` polls → the poll ends with `Cancelled`-like failure, no
   further reads (Task 3: the tool checks `registry.isOffered` is not available to it — instead `ScreenTools` takes a
   `std::function<bool()> allowed` and stops polling when it returns false; `AppController` passes
   `isScreenShared`).

---

### Task 1: AT-SPI nodes and the cache decoding (pure)

**Files:** Create `system/a11y/AtspiNode.h`, `system/a11y/AtspiNode.cc`; test `tests/AtspiCacheTest.cc`; modify
`system/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
namespace nexa::atspi {
struct NodeRef { QString bus; QString path; bool operator==(const NodeRef &) const = default; };
size_t qHash(const NodeRef &, size_t seed = 0);
enum class State : int { Active = 1, Checked = 4, Collapsed = 5, Editable = 7, Enabled = 8, Expandable = 9,
    Expanded = 10, Focusable = 11, Focused = 12, Iconified = 15, Pressed = 20, Selectable = 22, Selected = 23,
    Sensitive = 24, Showing = 25, Visible = 30, Checkable = 41, ReadOnly = 43 };
enum class Role : int { Invalid = 0, CheckBox = 7, CheckMenuItem = 8, ComboBox = 11, Dialog = 16, Filler = 20,
    Frame = 23, GlassPane = 24, Image = 27, Label = 29, LayeredPane = 30, List = 31, ListItem = 32, Menu = 33,
    MenuBar = 34, MenuItem = 35, PageTab = 37, PageTabList = 38, Panel = 39, PasswordText = 40, PushButton = 43,
    RadioButton = 44, RootPane = 46, ScrollBar = 48, ScrollPane = 49, Separator = 50, Slider = 51, SpinButton = 52,
    SplitPane = 53, StatusBar = 54, TableCell = 56, Terminal = 60, Text = 61, ToggleButton = 62, ToolBar = 63,
    ToolTip = 64, TreeItem = 91, Unknown = 67, Viewport = 68, Window = 69, Paragraph = 73, Application = 75,
    Entry = 79, DocumentFrame = 82, Heading = 83, Section = 85, RedundantObject = 86, Form = 87, Link = 88,
    DocumentText = 94, DocumentWeb = 95, Static = 116, Switch = 130 };
struct Node {
    NodeRef ref, parent; QList<NodeRef> children; int childCount{0}; QStringList interfaces;
    QString name, description; uint role{0}; quint64 states{0};
    bool has(State) const; bool implements(QLatin1StringView interface) const;   // "Text", "Action", "EditableText", "Component"
};
QString roleName(uint role);                      // "button" for PushButton, "toggle button", "check box", "page tab" → "tab", …; "unknown" past the table
// Both bridge forms: the GTK/Chromium `a((so)(so)(so)iiassusau)` (index in parent, child count) and Qt's
// `a((so)(so)(so)a(so)assusau)` (the children listed). The signature is the message's.
Result<QList<Node>> decodeCacheItems(const QDBusArgument &items, const QString &signature);
}
```

- [ ] Write `tests/AtspiCacheTest.cc`: `TEST_CASE("The GTK cache form decodes: parent, index and count, interfaces, name, role and the two state words")` — build a `QDBusArgument` with `beginArray(QMetaType(QMetaType::QVariantList))`… (write the structure by hand: `beginStructure` ×, `<< QDBusObjectPath`), one item mirroring the live terminal frame (name `""`, role 39, states words `1124073728, 0`); CHECK `nodes[0].has(State::Showing)`, `has(State::Visible)`, `has(State::Sensitive)`, `!has(State::Focused)`, `childCount == 5`. `TEST_CASE("Qt's cache form lists the children instead")` — the `a(so)` form; CHECK `children.size() == 2`, `childCount == 2`. `TEST_CASE("A signature neither bridge sends is a Parse error")`. `TEST_CASE("Role names are the words a person uses")`: `roleName(43) == "button"`, `roleName(62) == "toggle button"`, `roleName(37) == "tab"`, `roleName(60) == "terminal"`, `roleName(999) == "unknown"`.
- [ ] Run: `cmake --build build && ./build/tests/nexa_tests "*cache form*,*Role names*"` → fails (no such files).
- [ ] Implement `AtspiNode.cc`: the role table as a `static const char *const kRoles[131]` from `atspi-constants.h`
  (ACCELERATOR_LABEL … SWITCH; words lower-case with spaces; `PUSH_BUTTON` → "button", `PAGE_TAB` → "tab",
  `PAGE_TAB_LIST` → "tab list", `PASSWORD_TEXT` → "password", `DOCUMENT_*` → "document"); `decodeCacheItems` reads
  `items.beginArray()` then per item `beginStructure`, three `(so)` refs, then by signature either `int index, count`
  or `a(so)` children, `QStringList`, `QString name`, `uint role`, `QString description`, `QList<uint>` states →
  `states = words[0] | (words[1] << 32)`.
- [ ] Run the two filters → pass. Add both files to `system/CMakeLists.txt` (an `a11y/` block) and the test to
  `tests/CMakeLists.txt`.
- [ ] Commit: `git add system/a11y tests/AtspiCacheTest.cc system/CMakeLists.txt tests/CMakeLists.txt && git commit -m "M27: AT-SPI nodes and cache decoding"`.

### Task 2: The outline (pure)

**Files:** Create `system/a11y/ScreenOutline.h`, `system/a11y/ScreenOutline.cc`; test `tests/ScreenOutlineTest.cc`.

**Interfaces — Consumes:** Task 1's `atspi::Node`, `Role`, `State`, `roleName`. **Produces:**
```cpp
namespace nexa {
struct AtspiApp { QString bus; QString name; QString toolkit; QList<atspi::Node> nodes; };   // the cache of one application
struct ScreenOutline {
    QString text;
    QHash<QString, atspi::NodeRef> refs;                 // "s12" → the node
    QHash<QString, uint> roles;                          // "s12" → its role (for click: Space on a check box)
    int windows{0};                                      // windows written out
    bool truncated{false};
};
// What the builder cannot know from the cache: a text element's words (fetched for the shown ones only).
using TextLookup = std::function<QString(const atspi::NodeRef &, int maxChars, bool fromEnd)>;
struct OutlineOptions {
    QString scope{QStringLiteral("active")};             // active | all | words of an app or window title
    QString filter;                                      // keep lines containing these words
    QString ownApp{QStringLiteral("nexa-assistant")};    // Nexa's own application name: skipped
    int maxLines{300}; int maxChars{12000}; int excerptChars{160}; int terminalTailChars{400};
};
ScreenOutline buildScreenOutline(const QList<AtspiApp> &apps, const OutlineOptions &options, const TextLookup &text);
// The words the tools and the prompt use for a window that shows nothing.
QString exposesNothingHint();
}
```
Rules (the spec's): windows = an application's children of role Frame/Window/Dialog with Showing && Visible &&
!Iconified; active = `has(Active)` else a window with a Focused descendant; shell apps skipped by name
(`gnome-shell`, `gjs`, `ibus-extension-gtk3`, `xdg-desktop-portal-gtk`, `at-spi2-registryd`) and `options.ownApp`;
structural roles lifted (Filler, Panel, Viewport, ScrollPane, Section, RootPane, LayeredPane, GlassPane, SplitPane,
Separator, ScrollBar, ToolTip, Unknown, RedundantObject, Form, ToolBar, MenuBar, List, PageTabList, Application when
name empty — a *named* panel/section stays as a `group "name"` line); a node is written when Showing (a child of a
non-Showing node is not visited); line = `"  " * depth + "- " + roleName + (name.isEmpty() ? "" : " \"name\"") + flags
+ " [ref=sN]" + (description non-empty and != name ? " — description" : "") + excerpt`; flags in this order: `[checked]`
(Checked), `[selected]` (Selected), `[expanded]`/`[collapsed]`, `[focused]`, `[disabled]` (!Sensitive || !Enabled, only
when the node is Focusable or has Action), `[editable]` (Editable, roles Text/Entry/Terminal/Document*); excerpt: for
a node that `implements("Text")` with role in {Text, Entry, Terminal, Paragraph, Static, DocumentText, DocumentWeb,
DocumentFrame, Heading, Label} and `name.isEmpty()` → `text(ref, excerptChars, false)` (Terminal: `terminalTailChars`,
`fromEnd = true`), written as `: "…"` with newlines as `⏎`; PasswordText → `[password]` and no text; a window header
`Window "title" — AppName (active) [ref=sN]`; a window with no written child → header + `  (exposes nothing — ` +
`exposesNothingHint()` + `)`; scope `active` = the active window + one line per other window `Also open: …`; scope
`all` = every window; other words = windows whose title or app name contains them (case-insensitive); `filter` keeps
window headers and matching lines; the cap appends `… N more elements; ask again with filter`.

- [ ] Write `tests/ScreenOutlineTest.cc` with a fixture builder `node(path, parent, role, name, states…)` and the
  terminal tree of the live sample (frame "◑ nexa-mission-control-dashboard", a panel with buttons "New Tab", "Find",
  toggle button "Menu", a page tab list → page tab → panel → terminal "Terminal" with Text; the window Active). Cases:
  `"The active window comes first with its buttons, the terminal's tail quoted and every element a ref"` (text contains
  `Window "◑ nexa-mission-control-dashboard" — org.gnome.Terminal (active) [ref=s1]`, `- button "New Tab" [ref=s`,
  `- toggle button "Menu"`, `- terminal "Terminal"` … hmm the terminal has a name, so the excerpt rule needs
  `Terminal` always excerpted: make the rule "Terminal role: always the tail"; CHECK the tail text appears and
  `refs.size() == 5`), `"Fillers and panels are lifted out, so nothing is nested for nothing"` (depth of the buttons
  is 1), `"A window that exposes nothing says how to change that"` (Chrome: frame with childCount 1 but the child
  absent → `exposesNothingHint()` in the text and `Also open` line when another window is active), `"Nexa's own window
  and the shell are never in the outline"`, `"A password field shows no text"`, `"filter keeps the window header and
  the matching lines"`, `"The cap says how many were left out"` (400 buttons → `truncated`, text has `… 100 more
  elements`), `"scope names a window by its title"`.
- [ ] Run `./build/tests/nexa_tests "*outline*"` → fails. Implement. Run → passes. Add to CMake lists.
- [ ] Commit `M27: the desktop outline`.

### Task 3: The bus, the reader and the screen tools

**Files:** Create `system/a11y/IScreenReader.h`, `system/a11y/AtspiBus.h/.cc`, `system/a11y/AtspiScreenReader.h/.cc`,
`system/a11y/ScreenTools.h/.cc`; tests `tests/FakeScreenReader.h`, `tests/ScreenToolsTest.cc`; modify `app/main.cc`
(register after `registerSystemTools`), CMake lists.

**Interfaces — Produces:**
```cpp
class IScreenReader {                                   // Strategy: the desktop's trees and the acts on them
public:
    using Done = std::function<void(Result<QList<AtspiApp>>)>;
    virtual bool isAvailable() const = 0;               // a bus to read
    virtual QString unavailableReason() const = 0;
    virtual void apps(Done done) = 0;                   // every application's cache (one GetItems each; GetChildren walk, capped at 2000 nodes, for one without a Cache)
    virtual void text(const atspi::NodeRef &, int offset, int maxChars, bool fromEnd, std::function<void(Result<QString>)>) = 0;
    virtual void actions(const atspi::NodeRef &, std::function<void(Result<QStringList>)>) = 0;
    virtual void doAction(const atspi::NodeRef &, int index, std::function<void(Result<void>)>) = 0;
    virtual void grabFocus(const atspi::NodeRef &, std::function<void(Result<void>)>) = 0;
    virtual void setText(const atspi::NodeRef &, const QString &, std::function<void(Result<void>)>) = 0;   // EditableText.SetTextContents
};
class AtspiBus : public QObject {                        // Facade over the session bus's org.a11y.Bus
public:
    explicit AtspiBus(QObject *parent = nullptr);
    bool isAvailable() const;                           // GetAddress answered and the connection is up
    QString unavailableReason() const;
    QDBusConnection connection() const;                 // "nexa-a11y"
    Result<bool> appsExposeContents() const;            // org.a11y.Status IsEnabled
    Result<void> setAppsExposeContents(bool on);
};
class AtspiScreenReader : public QObject, public IScreenReader { explicit AtspiScreenReader(AtspiBus *bus, QObject *parent = nullptr); … };
struct ScreenServices { IScreenReader *reader{nullptr}; std::function<IInputBackend *()> input; std::function<bool()> allowed; };
void registerScreenTools(ToolRegistry &registry, const ScreenServices &services);   // screen_snapshot, screen_read, screen_click, screen_type, screen_wait_for
```
`AtspiScreenReader`: every call runs a lambda on `QThreadPool` (a pool of 1) doing blocking `QDBusConnection::call`s on
`bus->connection()`, then `QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)` with the result; a D-Bus error
`org.freedesktop.DBus.Error.UnknownObject` / `UnknownMethod` / `ServiceUnknown` on a ref → `ErrorCode::NotFound`
("that element is gone: take screen_snapshot again"). `text()`: `CharacterCount` property then `GetText(start, end)`
(`fromEnd`: the last `maxChars`). The application's name = the root node's `Name`, toolkit from the
`org.a11y.atspi.Application.ToolkitName` property. `ScreenTools` keeps `ScreenOutline current_`; `screen_click`:
`actions()` → if any: `doAction(ref, index of the named one, else 0)`, result "Clicked "name" (action click)"; none:
`grabFocus` then `input()->pressKeys({KEY_ENTER})` (`KEY_SPACE` when `current_.roles[ref]` is CheckBox,
ToggleButton, RadioButton, Switch, CheckMenuItem), "Focused "name" and pressed Enter". `screen_type`: Editable &&
implements EditableText → `setText`; else `grabFocus` then `input()->typeText(text)`; no backend → the same sentence
`type_text` gives. `screen_wait_for`: `apps()` every 500 ms (QTimer, single shot, re-armed) until the outline's text
contains the words (case-insensitive) or the deadline; stops with `Cancelled` when `allowed()` turns false.

- [ ] Write `tests/FakeScreenReader.h` (scripted apps, text per ref, actions per ref, a log of calls) and
  `tests/ScreenToolsTest.cc`: `"screen_snapshot writes the outline and remembers its refs"`, `"screen_read gives the
  element's text from an offset"`, `"screen_click performs the element's action, or focuses it and presses Enter"`
  (two sections; a fake `IInputBackend` records `pressKeys`), `"screen_type sets an editable element's text, and types
  into a terminal"`, `"A ref from an older snapshot that is gone says to snapshot again"` (the fake answers NotFound),
  `"screen_wait_for returns when the words appear and fails at the deadline"` (the fake's apps change after the second
  call), `"Without a bus every screen tool says why"` (`isAvailable` false → `NotSupported` with the reason),
  `"The read-only tools are not mutating and the acting ones are privileged"` (specs).
- [ ] Run → fails. Implement. Run → passes. Register in `main.cc`: `AtspiBus a11y; AtspiScreenReader screenReader(&a11y);
  registerScreenTools(tools, ScreenServices{.reader = &screenReader, .input = [&input] { return input.current(); },
  .allowed = [&controller] { return controller.isScreenShared(); }});` — the controller is constructed after the tools
  today, so `allowed` captures a `QPointer`/flag set after construction (see how `browserTarget` is wired) — and
  `status.a11y = &a11y` (Task 6 adds the field).
- [ ] Live check (no test): `QT_QPA_PLATFORM=offscreen ./build/app/nexa-assistant --new-instance --ask "share my screen"
  --ask "run screen_snapshot and tell me the title of the window in front and its buttons"` with private XDG folders →
  the journal/stdout shows `screen_snapshot` run and the reply naming the terminal's window.
- [ ] Commit `M27: the accessibility bus, its reader and the screen_* tools`.

### Task 4: The phone's hierarchy and its tools

**Files:** Create `system/android/UiHierarchy.h/.cc`; test `tests/UiHierarchyTest.cc`, `tests/AndroidToolsTest.cc`;
modify `system/android/AdbShell.h/.cc` (+ `tests/AdbShellTest.cc`), `system/android/AndroidDevice.h/.cc`,
`system/android/AndroidTools.h/.cc`, `tests/fake-adb/adb`, CMake lists.

**Interfaces — Produces:**
```cpp
namespace nexa::uihierarchy {
struct UiNode { QString className, text, description, resourceId, package; QRect bounds; int depth{0};
    bool clickable{false}, longClickable{false}, checkable{false}, checked{false}, enabled{true}, focused{false},
    scrollable{false}, selected{false}, password{false}; };
Result<QList<UiNode>> parse(const QString &xml);           // Parse: "the phone did not describe its screen: <uiautomator's line or the XML error>"
struct PhoneOutline { QString text; QHash<QString, QRect> refs; bool truncated{false}; QString package; };
PhoneOutline outline(const QList<UiNode> &nodes, const QString &filter = {}, int maxLines = 300, int maxChars = 12000);
QString role(const QString &className);                    // "Button" → "button", "EditText" → "entry", … "item"
}
QString adbshell::uiDumpCommand();                         // the one-line dump-cat-rm
// AndroidDevice
void uiHierarchy(std::function<void(Result<uihierarchy::PhoneOutline>)> done);   // dumps, parses, keeps the refs
std::optional<QPoint> uiCentre(const QString &ref) const;  // the tap point of a ref from the last outline
```
Lifting: a node with empty text and description, not clickable and not scrollable is lifted (children stand in its
place). Line: `- role "text or description" [tap] [checked] [selected] [focused] [disabled] [scrollable] [password]
[ref=pN]` — `text` first, else `description`; both → `"text" — description`; the resource id's last segment in
parentheses when it adds a word (`(search_button)`). Header `Phone screen — <package of the first node>`. A dump
with `<hierarchy` and no node → `outline` text "The phone showed an empty screen."

- [ ] Write `tests/UiHierarchyTest.cc` with the fixture XML (a video app: `FrameLayout` → `ImageButton`
  content-desc "Search" clickable bounds `[924,84][1032,192]`, a `RecyclerView` scrollable with three `TextView`
  titles, a bottom `LinearLayout` of four clickable items with descriptions Home/Shorts/Subscriptions/Library, an
  `EditText` password="true" text "secret"): `"The dump becomes an outline with a ref per element and the centre of
  each"` (`refs["p1"]` → centre `(978,138)`), `"Bare layouts are lifted out"`, `"A password field shows no text"`,
  `"uiautomator's own error is reported as such"` (`parse("ERROR: could not get idle state.")` → Parse error naming
  it), `"An empty hierarchy is said in words"`, `"filter and the cap"`.
- [ ] Add to `tests/AdbShellTest.cc`: `uiDumpCommand()` equals
  `uiautomator dump /sdcard/nexa-ui.xml >/dev/null 2>&1 && cat /sdcard/nexa-ui.xml && rm -f /sdcard/nexa-ui.xml`.
- [ ] Run → fails. Implement `UiHierarchy`, `uiDumpCommand`, `AndroidDevice::uiHierarchy` (`shell(uiDumpCommand(), 20'000, …)`,
  `parse`, `outline`, `lastUi_ = outline`), `uiCentre`.
- [ ] Tools in `AndroidTools.cc`: `android_snapshot {filter}` (not mutating; "Phone screen — …" text; the description
  says "Read the phone's screen as text: every element has a ref for android_tap and android_type. Works whether or not
  you can see images."); `android_tap`: new optional `ref` (string) — with it, `device->uiCentre(ref)` (none →
  `InvalidArgument` "no element p7 in the last android_snapshot; take one again"), the tap at the centre, sentence
  "Tapped "Search" (p1) at 978, 138."; `android_type`: optional `ref` → `input tap` first then the text command in the
  same shell line joined with ` && sleep 0.3 && `; `android_wait_for {text, timeout_ms}` polling `uiHierarchy` every
  500 ms. `android_screenshot`'s description: "… Prefer android_snapshot when you cannot see images."
- [ ] `tests/fake-adb/adb`: in `shell)`, `case "$2" in uiautomator*) cat <<'XML' … XML ;; esac` printing the fixture
  (the same video app) before the properties fall-through; keep the old behaviour for other commands.
- [ ] Write `tests/AndroidToolsTest.cc`: `qputenv("PATH", tests/fake-adb + ":" + PATH)` (AdbClient finds `adb` on the
  PATH — confirm in `AdbClient.cc`; if it takes a path, give it the fake's), `AdbClient adb; AndroidDevice phone(&adb);
  ToolRegistry registry; registerAndroidTools(registry, &phone, &files);` then `"android_snapshot reads the fake phone's
  screen with refs"` (`Phone screen — com.google.android.youtube`, `[ref=p1]`), `"android_tap by ref taps the element's
  centre"` (the result sentence has `978, 138`), `"android_tap with an unknown ref asks for a new snapshot"`.
- [ ] Run `./build/tests/nexa_tests "*hierarchy*,*android_*,*uiDump*"` → passes. CMake lists. Update
  `AndroidTools.h`'s comment (20 tools).
- [ ] Commit `M27: the phone's screen as text — android_snapshot, taps by ref`.

### Task 5: The blind swap, the gate and the prompt

**Files:** Modify `core/intent/IIntentHandler.h` (`IntentMatch::textToolName`), `core/intent/PcIntents.cc`,
`core/intent/PhoneIntents.cc`, `core/mission/TaskRunner.cc`, `core/mission/RoleClassifier.cc`, `core/agent/Agent.cc`,
`app/AppController.h/.cc`; tests `tests/TaskRunnerTest.cc`, `tests/IntentRouterTest.cc`, `tests/PhoneIntentTest.cc`,
`tests/RoleClassifierTest.cc`, `tests/AppControllerTest.cc`, `tests/AgentTest.cc`.

**Interfaces — Produces:** `IntentMatch::textToolName` (`QString`; "the tool to run instead when the model cannot see
images"); `AppController::kScreenTools` = `{"screen_snapshot","screen_read","screen_click","screen_type","screen_wait_for"}`.

- [ ] `TaskRunnerTest`: `"A model that takes no images gets the text twin of a picture tool on the fast path"` — register
  `picture` (returns an image) and `words`; `match{.toolName = "picture", .forwardToAgent = true, .textToolName = "words"}`;
  SECTION provider `images = false` → the model's request text contains `words`'s result and no image; SECTION
  `images = true` → `picture`'s; SECTION `words` not registered → `picture`'s. Run → fails. Implement in
  `startFastPath`: `const bool blind = provider_ && (provider_->takesImages(model_).value_or(provider_->capabilities().vision) == false); const QString tool = blind && !match.textToolName.isEmpty() && registry_->contains(match.textToolName) ? match.textToolName : match.toolName;` (`registry_` — add the member; the constructor already receives the registry). Pass.
- [ ] `IntentRouterTest` / `PhoneIntentTest`: `route("what's on my screen")->textToolName == "screen_snapshot"`,
  `"see this"` likewise; `"what's on my phone"` → `android_snapshot`. Implement in `ScreenIntent::match` and
  `PhoneIntents::askAfter(name, prompt, textTool)`. `AppController::askAboutPicture` gains `textTool`;
  `askAboutPhone` passes `android_snapshot`.
- [ ] `RoleClassifierTest`: `forTool("screen_snapshot") == AgentRole::Screen`, `forTool("screen_click")`. Implement
  (`toolName.startsWith(u"screen_")`).
- [ ] `AppControllerTest`: `"The screen tools are withheld with the screenshot until the screen is shared"` — register
  five stubs named as `kScreenTools` plus `take_screenshot`; before `share my screen` `tools.isOffered("screen_snapshot")`
  is false and `withheldReason` mentions `Share screen`; after, true; `"stop sharing my screen"` → withheld again.
  Implement in `applyScreenSharing` (loop over `kScreenTools` too; the reason sentence: "the user is not sharing the
  screen. Work through the other tools (in the browser, browser_snapshot reads the page as text); if the request cannot
  be done without the screen, ask the user to switch Share screen on."). `basePrompt`: shared → "The user is sharing
  their screen with you: call screen_snapshot to read the window in front as text — every element has a ref for
  screen_click, screen_type and screen_read, and it works whether or not you can see images — and take_screenshot when
  only a picture will do (drawings, photos, a game, or a window that exposes nothing)."; not shared → add "and the
  screen_* tools" after `take_screenshot is not`; `browserParagraph` shared → "screen_snapshot or take_screenshot, then
  screen_click, screen_type or the mouse and keyboard tools."
- [ ] `AgentTest`: the blind notice ends with "The screen can still be read as text with screen_snapshot." when a tool
  of that name is offered (register a stub), and without it unchanged. Implement in `Agent::sendStep` (both notices).
- [ ] Run `ctest --test-dir build --output-on-failure` → all pass. Commit `M27: the text twin on the fast path, the gate, the prompt`.

### Task 6: Settings, docs, verification

**Files:** Modify `app/SystemStatus.h` (`AtspiBus *a11y`), `app/SettingsDialog.h/.cc` (General tab group, applied in
`accept()`), `app/main.cc`, `CLAUDE.md`, `README.md`, `ARCHITECTURE.md`; test `tests/SettingsDialogThemesTest.cc` (or a
new case in `SettingsDialogProvidersTest.cc`): the General tab has `QCheckBox` object name `exposeContents`.

- [ ] Settings → General, above the hotkeys: `QGroupBox tr("Screen as text")` with a word-wrapped status label
  (`status_.a11y` null or `!isAvailable()` → "The accessibility bus is not running on this desktop, so windows cannot be
  read as text."; else "Nexa reads windows as text through the desktop's accessibility bus while Share screen is on.")
  and `QCheckBox tr("Let apps expose their contents to Nexa (GNOME's accessibility switch; Chrome, Electron and Qt apps
  need it, from their next start — GTK apps always do)")`, checked from `appsExposeContents()`, disabled without a
  bus; on OK, when changed, `setAppsExposeContents(checked)` and a `qCInfo(lcApp)` line. Test: the check box exists
  and is disabled with a null bus (the test's `SystemStatus` has none).
- [ ] Docs: CLAUDE.md — the layout bullet for `system/` gains `a11y/` (AtspiBus, AtspiNode, ScreenOutline,
  IScreenReader → AtspiScreenReader, ScreenTools) and `android/UiHierarchy`; the conventions bullet "The model sees the
  PC screen only while Share screen is on" names the `screen_*` tools and the text rule; the "Build, run, test" section
  gets one sentence on `toolkit-accessibility` (Settings → General). README: the "Looks only when you share" bullet
  says the screen is read as text first. ARCHITECTURE: the M27 row with the evidence labels, Status line "M27 built".
- [ ] `cmake --build build && cmake --build build-debug && ctest --test-dir build --output-on-failure` → clean, all pass.
- [ ] Live: an isolated copy (private XDG, wake word and remote off), `--ask "share my screen" --ask "<a request that
  needs screen_snapshot>"`; with the real GNOME switch left as it is unless the owner asks. Read the journal lines.
  Restart the owner's Nexa on the new build (memory rule). Commit `M27: the screen as text — Settings, docs`.
