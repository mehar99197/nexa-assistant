# M27 — The screen as text: the desktop and the phone as an outline a model without eyes can work on

Date: 2026-11-12 · Status: designed from the owner's request, decisions below taken by Claude and open to correction ·
Branch: `m20-hardcore` (continues)

## Why

The owner: "I am making an AI assistant that monitors the screen using a tool like xdotool or images. Is there a method
so it can monitor and work without images, like we use Playwright, which works even for models that do not support
images? Make it so it can monitor my screen — desktop, phone and browser — even through a model that cannot see images."

Today Nexa reaches the three screens three ways:

- **Browser** — Playwright (M14): `browser_snapshot` gives the page as a text outline in which every element has a ref,
  and `browser_click` / `browser_type` act on a ref. Works for every model.
- **Desktop** — `take_screenshot` (a picture) and the mouse and keyboard tools (coordinates read off that picture).
  Needs a model that sees; a blind model gets the words without the picture and is told so.
- **Phone** — `android_screenshot` (a picture) and `android_tap x y`. The same limit.

The answer is yes, and the method already exists on both: every GTK, Qt, Chromium, Electron and LibreOffice window
describes itself on the desktop's **accessibility bus** (AT-SPI2, what the Orca screen reader uses) as a tree of named
widgets with states and actions, and an Android phone describes its screen through **`uiautomator dump`** as XML with
every view's text, description, id, bounds and whether it can be tapped. Both become the same kind of outline Playwright
gives, with refs, so a model that cannot see images reads the screen and acts on it by ref — no picture, no synthetic
typing at coordinates.

Checked on this machine (2026-11-12): the a11y bus runs (`org.a11y.Bus.GetAddress`); GNOME Terminal's whole tree
(89 nodes) comes back in one call to `org.a11y.atspi.Cache.GetItems`; its terminal node's text (4 243 characters) is
readable through `org.a11y.atspi.Text`; Chrome is on the bus but shows only its window frames, and Nexa itself (Qt)
nothing, because GNOME's accessibility switch (`org.a11y.Status IsEnabled`, which `toolkit-accessibility` mirrors) is
off — switched on, Chromium, Electron and Qt apps expose their contents from their next start (GTK apps always do).
The phone was not connected; `uiautomator dump` is standard on Android 4.1 and later.

## Decisions (taken by Claude; say if any should change)

1. **The same consent as a picture.** The text of every window is as private as a screenshot of it, so the `screen_*`
   tools are offered only while **Share screen** is on, exactly like `take_screenshot`, and the user's own "what's on
   my screen?" is the one exception as before. The phone's screen is the phone's own (its panel mirrors it already):
   `android_snapshot` is not gated, like `android_screenshot`.
2. **Reading, not watching.** A snapshot is taken when a tool asks for it (as Playwright's is); nothing follows the
   screen continuously. `screen_wait_for` / `android_wait_for` poll until words appear, for a model that has just
   acted. Following focus changes through AT-SPI events, and feeding `AppContext` (the app in front) from the bus, are
   left for later: they would read the desktop while Share screen is off.
3. **Actions by ref, never by coordinates.** On Wayland an app does not know where its window is, so AT-SPI's screen
   coordinates are not trustworthy; `screen_click` performs the element's own action (`click`, `press`, `activate`,
   `jump`, `toggle`…), or focuses it and presses Enter or Space when it has none. `screen_type` sets an editable
   element's text through `EditableText`, or focuses it and types through the input backend (the helper or the portal,
   as `type_text` does). The phone's bounds are its own pixels, so `android_tap {ref}` taps the centre of them.
4. **GNOME's accessibility switch is the user's.** Settings → General gains "Let apps expose their contents to Nexa";
   ticking it sets `org.a11y.Status IsEnabled` (GNOME persists it as `toolkit-accessibility`). Nexa never flips it on
   its own; a window that exposes nothing is reported with the hint.
5. **Pictures stay.** `take_screenshot` and `android_screenshot` remain for what a tree cannot say (drawings, photos, a
   game, a canvas); the prompt says to prefer the text, and a model that cannot see images gets the text alternative
   automatically on the fast path ("what's on my screen?" → `screen_snapshot`; "what's on my phone?" →
   `android_snapshot`).
6. **The browser stays Playwright's.** Nothing changes there but the prompt's wording about what to do when the browser
   tools cannot reach something.

## How

### Desktop — `system/a11y/`

- **`AtspiBus`** (Facade over the accessibility bus): asks the session bus `org.a11y.Bus.GetAddress` and connects
  (`QDBusConnection::connectToBus(address, "nexa-a11y")`); `isAvailable()`; `appsExposeContents()` and
  `setAppsExposeContents(bool)` read and write `org.a11y.Status IsEnabled` on the session bus's `org.a11y.Bus`.
- **`AtspiNode`** + **`atspi::` helpers** (pure): a node as the cache gives it — `NodeRef {bus, path}`, parent, children
  (a list, or an index and a count), interfaces, name, description, role (number), states (64 bits);
  `decodeCacheItems(QDBusArgument, signature)` reads both forms the bridges send — the GTK/Chromium one
  `a((so)(so)(so)iiassusau)` and Qt's `a((so)(so)(so)a(so)assusau)`; `roleName(role)` from the AT-SPI role table
  (`atspi-constants.h`, 131 roles); `hasState(states, State)` with the state table (SHOWING 25, VISIBLE 30, ACTIVE 1,
  FOCUSED 12, SENSITIVE 24, CHECKED 4, SELECTED 23, EXPANDED 10, EDITABLE 7, ICONIFIED 15…).
- **`IScreenReader`** (Strategy): what the tools need of the bus — `apps()` (every application's name and its cached
  tree, fetched in one `GetItems` per app, or by `GetChildren` for a bridge without the cache), `text(ref, offset,
  max, fromEnd)` (`org.a11y.atspi.Text`), `actions(ref)` and `doAction(ref, index)` (`org.a11y.atspi.Action`),
  `grabFocus(ref)` (`Component`), `setText(ref, text)` (`EditableText`). **`AtspiScreenReader`** is the real one: the
  blocking D-Bus calls run on a worker thread (QtDBus connections may be used from any thread), the result comes back
  queued; one fetch at a time. A fake implements the interface for the tests.
- **`ScreenOutline`** (pure): the outline from the trees. Windows are an application's children of role frame, window
  or dialog that are showing, visible and not iconified; the **active** one (state ACTIVE, else a focused descendant)
  comes first, marked `(active)`; Nexa's own windows and the shell's (gnome-shell, the desktop icons, ibus, the portal)
  are left out. Nodes not SHOWING are skipped; structural nodes with no name (filler, panel, viewport, scroll pane,
  section, root/layered/glass/split pane, separator, scroll bar, tool tip) are lifted out so their children stand in
  their place; everything else is one line, indented by depth: `- <role> "<name>" [ref=s12]`, with `[checked]`,
  `[selected]`, `[expanded]`/`[collapsed]`, `[focused]`, `[disabled]`, `[editable]` where they apply, the description
  after ` — ` when it adds something, and for a text element without a name an excerpt in quotes (the first 160
  characters; a terminal's **last** 400, where the prompt is), with "(N characters; screen_read for all of it)". A
  password field's text is never read (`[password]`). A window with no exposed children says so: "exposes nothing —
  switch on Settings → General → Let apps expose their contents, then open it again". Cap 300 lines / 12 000
  characters, then "… N more; ask again with filter". `filter` keeps the lines containing the words (and their window
  header). Refs are `s1, s2, …` (Playwright's are `e…`, the phone's `p…`, so a model never mixes them); the outline
  carries ref → node.
- **`ScreenTools`** (`registerScreenTools(registry, ScreenServices{reader, input})`):

  | Tool | Arguments | Does |
  |---|---|---|
  | `screen_snapshot` | `scope` (`active` default, `all`, or words of an app or window title), `filter` | the outline; keeps its refs as the current ones |
  | `screen_read` | `ref`, `offset`, `max_chars` (≤ 20 000) | the whole text of a text element (a terminal, a document, an editor) |
  | `screen_click` | `ref`, `action` (a name; default the first) | `DoAction`; with no action: `GrabFocus` then Enter (Space for a check box or toggle) through the input backend |
  | `screen_type` | `ref`, `text` (`\n` = Enter) | `SetTextContents` on an editable element; else `GrabFocus` then the input backend types |
  | `screen_wait_for` | `text`, `timeout_ms` (≤ 30 000) | snapshots every 500 ms until the words appear |

  `screen_snapshot`, `screen_read` and `screen_wait_for` are read-only (`mutating` false); `screen_click` and
  `screen_type` are `privileged` with a risk sentence like the mouse tools'. A ref from an older snapshot that no longer
  resolves answers "that element is gone: take screen_snapshot again". Without a bus (not GNOME, or a test's offscreen
  copy) the tools are registered and answer `NotSupported` with the reason.
- **The gate**: `AppController::applyScreenSharing` withholds and offers the five `screen_*` tools together with
  `take_screenshot` and `browser_take_screenshot`. The prompt, screen shared: "call screen_snapshot to read the window in
  front as text — every element has a ref for screen_click, screen_type and screen_read; it works whether or not you can
  see images — and take_screenshot when only a picture will do"; not shared: as today, naming both. `Agent`'s notice to a
  blind model about a dropped picture adds "the screen can be read as text with screen_snapshot" when that tool is
  offered. `RoleClassifier::forTool` puts `screen_*` with the Screen role.
- **The fast path**: `IntentMatch::textToolName` — the tool to run instead when the model cannot see images.
  `ScreenIntent` sets `screen_snapshot`, `PhoneIntent` and `askAboutPhone` set `android_snapshot`;
  `TaskRunner::startFastPath` takes it when the provider says the model takes no images (`takesImages(model)` false,
  else `capabilities().vision` false) and the registry has the tool.
- **Settings → General → Screen as text**: a status line ("The accessibility bus is running: 4 windows of 3 apps expose
  their contents" / "not running on this desktop") and the checkbox "Let apps expose their contents to Nexa (GNOME's
  accessibility switch; Chrome, Electron and Qt apps need it, from their next start — GTK apps always do)", applied on OK
  through `AtspiBus::setAppsExposeContents`. `SystemStatus` gains `AtspiBus *a11y`.

### Phone — `system/android/`

- **`UiHierarchy`** (pure): `parse(xml)` with `QXmlStreamReader` → nodes (class, text, content-desc, resource-id,
  package, bounds `[l,t][r,b]`, clickable, long-clickable, checkable, checked, enabled, focused, scrollable, selected,
  password, depth); `outline(nodes, filter)` → the text and ref → bounds. The role is the class's last segment made
  plain (`Button`, `ImageButton` → button; `EditText` → entry; `TextView` → text; `CheckBox`, `Switch`, `RadioButton`,
  `Spinner` → combo box; `RecyclerView`, `ListView`, `ScrollView`, `ViewPager` → list; `WebView` → web view; anything
  else with text or a description → item); a container with no text, no description and not clickable is lifted out;
  `[tap]` marks a clickable element, `[checked]`, `[selected]`, `[focused]`, `[disabled]`, `[scrollable]`,
  `[password]` (its text hidden) as they apply. Header: `Phone screen — <package>`; refs `p1, p2, …`; cap and filter as
  the desktop's. `centre(ref)` is the tap point, in the phone's own pixels.
- **`AndroidDevice::uiHierarchy(done)`**: one shell command — `uiautomator dump /sdcard/nexa-ui.xml >/dev/null 2>&1 &&
  cat /sdcard/nexa-ui.xml && rm -f /sdcard/nexa-ui.xml` — 20 s; an answer without `<hierarchy` is an error in words
  ("the phone did not describe its screen: …", with uiautomator's own line, e.g. "could not get idle state" while the
  screen is still animating). The device keeps the last outline's refs.
- **Tools**: `android_snapshot {filter}`; `android_tap` and `android_type` gain `ref` (the tap at the element's
  centre, the typing after a tap on it); `android_wait_for {text, timeout_ms}`. `android_screenshot`'s description
  points at the snapshot for a model without eyes.
- **`tests/fake-adb/adb`**: `shell uiautomator dump …` prints a canned hierarchy (a video app: a search button, three
  video titles, a bottom bar), so an offscreen run shows the snapshot without a phone.

### Browser

Unchanged. The prompt's "only when the browser tools cannot do the job, use the screen" now names `screen_snapshot`
and `screen_click` before the picture and the mouse.

## Security and privacy

- The accessibility tree is window contents: read only while Share screen is on, or for the user's own "what's on my
  screen?" — the same rule as a picture. Nothing of it is stored; the current snapshot's refs live in memory until the
  next one. The audit line is the tool's call and the result's first line, as for every tool.
- Password fields: a desktop `password text` role and a phone node with `password="true"` show no text.
- The root helper is untouched: AT-SPI is read in Nexa's own process, unprivileged, through the bus any screen reader
  uses; the helper stays write-only.
- GNOME's accessibility switch is changed only by the user, in Settings.

## Tests

`AtspiCacheTest` (both cache signatures round-tripped through `QDBusArgument`; the live sample's state word
`0x43000000` = SENSITIVE, SHOWING, VISIBLE; role names), `ScreenOutlineTest` (the terminal tree from the live sample
→ its buttons and the terminal's tail; a dialog with an entry, a check box and buttons; the active window first;
Nexa's own and the shell's windows skipped; a window that exposes nothing → the hint; the password rule; filter; the
cap; refs distinct and resolvable), `ScreenToolsTest` (over a fake reader and a fake input backend: snapshot, read with
offset, click → the action, click on an element without one → focus + Enter, type → `SetTextContents`, type into a
terminal → focus + typed, a stale ref, wait_for success and timeout, no bus → `NotSupported`), `UiHierarchyTest`
(the video-app fixture: roles, refs, bounds and centres; a settings screen with switches; lifting of bare containers;
a password field; broken XML; filter; the cap), `AdbShellTest` (the dump command), `TaskRunnerTest` (a provider that
takes no images runs the text tool; one that does runs the picture tool; no text tool registered → the picture tool),
`IntentRouterTest` and `PhoneIntentTest` (`textToolName` set), `RoleClassifierTest` (`screen_*` → Screen),
`AppControllerTest` (the five tools withheld until the screen is shared, offered after), `SettingsDialog…` (the General
tab shows the group).

## Verification

Release and debug (`-Werror`) builds, `ctest`. Live, from an isolated copy with Share screen on: `screen_snapshot` of
this desktop (the terminal's buttons and its last lines; the Nexa window after GNOME's switch is on and Nexa restarted),
`screen_read` of the terminal, and a `screen_click` on a button of a GTK dialog; `android_snapshot` against the fake
adb (the phone is not connected today — a real phone's dump is checked when one is). The owner's copy restarted on the
new build. Evidence labelled [VERIFIED] / [REASONED] / [BLOCKED].

## Out of scope

Following the active window continuously (AT-SPI events) and feeding `AppContext` from it; a text outline of the
Nexa Phone app's own screens; `screen_*` from the web remote beyond what any tool already is; Wayland-safe pointer
clicks at an element's position (no reliable coordinates on Wayland).
