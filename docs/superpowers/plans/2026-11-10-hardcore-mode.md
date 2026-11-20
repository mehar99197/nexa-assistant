# Hardcore mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While a hardcore task is active, the laptop's own keyboard and pointer deliver nothing; the screen stays
awake; the voice and the phone can end it; Nexa going away always ends it.

**Architecture:** The root helper inhibits the physical input devices through the kernel's sysfs `inhibited`
attribute (pure, testable `InputDevices` + `DeviceLock` over a sysfs root), behind two new D-Bus methods. Nexa's
`HardcoreMode` (core, a state machine over `MissionControl` and an `IInputLock` strategy) decides when; the app, the
intents and the remote drive and show it.

**Tech Stack:** C++20, Qt 6.11 (Core, DBus, Widgets, HttpServer/WebSockets), Catch2 v3, the sysfs input class.

**Spec:** `docs/superpowers/specs/2026-11-10-hardcore-mode-design.md`

## Global Constraints

- The helper stays write-only for input: it reads `name`, `capabilities/*`, `inhibited` and the device's sysfs path,
  never an event node.
- Locked set: physical (path not under `/devices/virtual/`) keyboards (KEY_A, KEY_Z, KEY_SPACE, KEY_ENTER) and
  pointers (REL_X+REL_Y+BTN_LEFT, or ABS_X+ABS_Y+one of BTN_TOUCH / BTN_TOOL_FINGER / BTN_LEFT).
- Record: `/run/nexa-helper/locked-input.json`, 0600. Heartbeat: Nexa every 5 s, the helper's limit 30 s. Countdown 3 s.
- No time limit on a lock. Approvals do not unlock.
- Every source file listed in its `CMakeLists.txt`; project conventions (CLAUDE.md) apply; class comments name the
  pattern.

## Review Focus

1. A task that ends during the countdown or while `LockInput` is in flight → nothing stays locked (the reply's
   arrival with nothing to lock unlocks at once). Test in Task 3.
2. A second Nexa (a `--new-instance` copy) asking to lock while the first holds it → refused, the first's lock
   untouched. Test in Task 1 (`DeviceLock::lock(owner)`).
3. A recorded device unplugged, and another device later taking its `inputN` → release skips it (name and physical
   path must match the record). Test in Task 1.
4. Nexa quits cleanly while locked (SIGTERM, "stop everything" then quit) → `HelperInputLock`'s destructor unlocks;
   the helper's name watcher is the backstop. Covered by reasoning + the live test (Task 8).
5. The password prompt at the first lock of a login → the devices are inhibited only after polkit answers
   (`whenAllowed`), so the owner can type it. Live test (Task 8).

---

### Task 1: The helper's device rules and lock (pure)

**Files:** Create `helper/InputDevices.{h,cc}`, `helper/DeviceLock.{h,cc}`; tests `tests/InputDevicesTest.cc`,
`tests/DeviceLockTest.cc`; modify `helper/CMakeLists.txt`, `tests/CMakeLists.txt` (link the two `.cc` like
`SessionGrants.cc`).

**Interfaces — Produces:**
- `namespace nexa::inputdevices { enum class Kind { Keyboard, Pointer, Other };
  std::vector<bool> parseBitmap(const QString &sysfsWords, int wordBits = 64);
  struct Caps { QString name; QString ev, key, rel, abs; bool isVirtual{false}; };
  Kind classify(const Caps &caps); }`
- `class DeviceLock { DeviceLock(QString classRoot = "/sys/class/input", QString recordPath =
  "/run/nexa-helper/locked-input.json"); Result<QStringList> lock(const QString &owner);
  void unlock(); bool isLocked() const; QString owner() const; static int releaseRecorded(const QString &recordPath,
  const QString &classRoot); }` — `lock` returns the names of every device it holds; a call with a different owner
  while locked → `ErrorCode::PermissionDenied` ("Another Nexa holds the keyboard and mouse.").

- [ ] Write `InputDevicesTest`: this laptop's bitmaps (spec fixtures: input0 keyboard `key` 9 words, input6 touchpad
  `ev b`, `key e520 10000 0 0 0 0`, `abs 660800011000003`, Power Button, Lid Switch, Video Bus, PC Speaker, HP WMI
  hotkeys virtual) → Keyboard, Pointer, Other ×5; a USB mouse (`ev 17`, `rel 1803`/`903`, `key` with BTN_LEFT) →
  Pointer; `parseBitmap("1 0")` has bit 64 set and bit 0 clear.
- [ ] Write `DeviceLockTest` over a `QTemporaryDir` tree (`class/input/inputN` symlinks to `devices/platform/…` or
  `devices/virtual/…`, with `name`, `capabilities/*`, `inhibited`): lock writes `1` to the keyboard and the touchpad
  only and returns their names; the record lists them; a second `lock` (same owner) after adding a mouse locks it too;
  another owner → PermissionDenied; a device already `1` before the lock is neither recorded nor released; `unlock`
  writes `0` and deletes the record; `releaseRecorded` from the record does the same; a recorded device whose `name`
  changed is skipped.
- [ ] Run `ctest -R "InputDevices|DeviceLock"` → fail; implement; → pass. Commit "M20: the helper's device rules".

### Task 2: The helper's contract, service and packaging

**Files:** Modify `proto/org.nexa.Helper.xml` (`LockInput → as names`, `UnlockInput`, signal `InputUnlocked(s)`;
the header comment: input is still never read), `proto/HelperTypes.h` (`kErrorLocked`),
`helper/HelperService.{h,cc}`, `helper/main.cc` (`--release-input`, release at start, no idle exit while locked),
`packaging/nexa-helper.service` (`ExecStopPost=/usr/local/libexec/nexa-helper --release-input`),
`packaging/install-helper.sh` + `uninstall-helper.sh` (say so; the check line), `system/helper/HelperClient.{h,cc}`
(`lockInput(std::function<void(Result<QStringList>)>)`, `unlockInput(Done)`, signal `inputUnlocked(QString)`).

- `HelperService::LockInput()` = `whenAllowed` → `DeviceLock::lock(callerUniqueName)`; on the first lock: a
  `QDBusServiceWatcher` on the caller (unregistered → unlock "Nexa closed"), a 30 s single-shot heartbeat restarted by
  every `LockInput` ("Nexa stopped answering"), a `QFileSystemWatcher` on `/dev/input` (rescan). `UnlockInput()`: only
  the owner's unique name or uid 0, else `kErrorDenied`. Each end emits `InputUnlocked(reason)` and logs one line.
- `UnknownMethod` from an older helper → `ErrorCode::NotSupported`, "The installed root helper is older than Nexa:
  update it with: sudo ./packaging/install-helper.sh".
- [ ] `cmake --build build --target nexa_helper` and the app build pass (the adaptor checks the slots); commit.

### Task 3: The board learns hardcore; HardcoreMode

**Files:** Create `core/mission/IInputLock.h`, `core/mission/HardcoreMode.{h,cc}`, `tests/HardcoreModeTest.cc`;
modify `core/mission/Task.h` (`bool hardcore{false};`), `core/mission/MissionControl.{h,cc}` (`TaskRequest::hardcore`,
copied in `add`, inherited in `delegate`; `void setHardcore(TaskId, bool)`; `void releaseHardcore()`), `core/CMakeLists.txt`,
`tests/CMakeLists.txt`.

**Interfaces — Produces:**
- `class IInputLock : public QObject { virtual bool canLock(QString *why) const = 0;
  virtual void lock(std::function<void(Result<QStringList>)> done) = 0; virtual void unlock() = 0;
  signals: void lost(const QString &reason); }`
- `class HardcoreMode : public QObject { enum class State { Off, Armed, Counting, Locking, Locked, Failed };
  HardcoreMode(MissionControl *, IInputLock *, QObject *parent = nullptr); void setSwitch(bool on); bool isSwitchOn();
  State state(); int secondsLeft(); QString reason(); QStringList devices(); QString banner();
  void unlockNow(const QString &why); static bool wantsLock(const QList<Task> &);
  signals: stateChanged(State), switchChanged(bool), countdownTick(int), unlocked(QString why); }`;
  `static constexpr int kCountdownMs = 3000;` (a setter `setCountdownMs` for tests).
- [ ] Tests (FakeLock + `tests/MissionRig.h`): switch on + a hardcore task → Counting → (after the countdown) Locking →
  done ok → Locked; the task finishes → `unlock()` called, Armed; a second queued hardcore task keeps Locked; Paused →
  unlocked, resume → Counting again; finished during Counting → no `lock()` call; finished while Locking → `unlock()` on
  the reply; `unlockNow` → switch off, every task's `hardcore` false, Off; `lost` → the same with its reason; `lock`
  fails → Failed with the reason, the task still runs; a delegated child of a hardcore lead is hardcore; a non-hardcore
  task never locks.
- [ ] Fail → implement → pass; commit "M20: hardcore mode on the board".

### Task 4: The words

**Files:** Modify `core/intent/IIntentHandler.h` (`Control::Unlock`, `Control::HardcoreOn`),
`core/intent/MissionIntents.cc`, `tests/IntentRouterTest.cc`.

- [ ] Tests: "unlock", "Nexa, unlock", "unlock the laptop", "unlock my keyboard and mouse", "give me my keyboard back",
  "hardcore off", "turn off hardcore mode", "stop hardcore mode" → Unlock; "hardcore on", "turn on hardcore mode",
  "hardcore mode" → HardcoreOn; "unlock my phone", "unlock the door", "how do I unlock a pdf" → not Unlock.
- [ ] Fail → implement → pass; commit.

### Task 5: The PC side of the lock

**Files:** Create `system/input/HelperInputLock.{h,cc}` (IInputLock over `HelperClient`: 5 s heartbeat `lockInput`,
`inputUnlocked` and the helper vanishing → `lost`, destructor unlocks), `system/screen/ScreenAwake.{h,cc}`
(`hold(reason)`, `release()`: `org.freedesktop.portal.Inhibit.Inhibit("", 4|8, {reason})` → `Request.Close` on
release; on a portal error `org.gnome.SessionManager.Inhibit("org.nexa.Assistant", 0, reason, 4|8)` / `Uninhibit`);
`system/CMakeLists.txt`.
- [ ] Build passes; commit.

### Task 6: The app

**Files:** Modify `app/AppController.{h,cc}` (`AppServices::inputLock`; owns `std::unique_ptr<HardcoreMode> hardcore_`
built when `inputLock` is set; `hardcore()`; `submit` and `addTasks(…, bool hardcore = false)` set
`request.hardcore`; `carryOut` Unlock → `unlockNow(tr("you said unlock"))`, HardcoreOn → `setSwitch(true)` + the ways
out spoken; `panic()` → `unlockNow`; while locked, a hardcore task's approval is announced whatever
`announceTasks` says), `app/main.cc` (HelperInputLock, `--hardcore`), `app/MainWindow.{h,cc}` (`hardcoreAction_`
checkable beside Share screen, Ctrl+Shift+H, the confirm naming the ways out, `#hardcoreBanner` above the central
area, tray notification on Locked and on `unlocked`), `app/AddTasksDialog.{h,cc}` (`#hardcoreCheck`; `hardcore()`),
`ui/NexaIcons` (`Lock`), `ui/theme.qss` (`#hardcoreBanner`, `#hardcoreSwitch`), tests `tests/AppControllerTest.cc`,
`tests/AddTasksDialogTest.cc`.
- [ ] Tests: "unlock" by voice while Locked → FakeLock unlocked and switch off; panic → Off; a request with the switch
  on makes a hardcore task; an approval of a hardcore task while Locked is announced with announceTasks "off";
  AddTasks with the box ticked → hardcore tasks.
- [ ] Fail → implement → pass; screenshot `--hardcore --ask "run sleep 20" --screenshot` (offscreen, the helper's
  answer shows in the banner); commit.

### Task 7: Phone and web remote

**Files:** Modify `remote/RemoteProtocol.{h,cc}` (`ClientMessage::Type::Unlock`, `Answer` with `id`, `allow`;
`state(mode, nodes, locked, lockText)`; `approval(id, agent, tool, question)`, `settled(id)`; `ServerMessage` gains
`locked`, `lockText`, `id`, `agent`, `tool`, `allow`; client `unlock()`, `answer(id, allow)`),
`remote/IRemoteHost.h` (`isLocked`, `lockText`, `unlock`, `approvals → QList<RemoteApproval{id, agent, tool, question}>`,
`answer(id, allow)`), `remote/RemoteServer.{h,cc}` (`onLockChanged(bool, QString)`, `onApprovalAsked(...)`,
`onApprovalSettled(id)`, the welcome sends pending ones), `app/RemoteBridge.{h,cc}`, `app/main.cc` (wiring),
`remote/web/{index.html,app.js,style.css}` (`#lockBar` + `#unlockButton`, `.approval` cards), `remote/RemoteClient.{h,cc}`
+ `android/ChatPage`/`PhoneWindow` (the bar, the button, the cards), `scripts/preview-web-remote.py` (a locked spell
and one approval); tests `tests/RemoteProtocolTest.cc`, `tests/RemoteServerTest.cc`.
- [ ] Tests: the new messages round-trip and malformed ones are refused (`answer` without an id); an `unlock` frame
  from a paired session calls the host's `unlock` once; `answer` reaches `answer(id, allow)`.
- [ ] Fail → implement → pass; the page in headless Chrome through the preview script; commit.

### Task 8: Docs and verification

**Files:** `ARCHITECTURE.md` (M20 row, status line), `CLAUDE.md` (layout: the new files; the helper's rule now says
it may switch devices off), `README.md` (Hardcore mode).
- [ ] `cmake --build build && ctest --test-dir build --output-on-failure`; `cmake --build build-debug` (-Werror);
  ASan `nexa_tests "*Hardcore*" "*DeviceLock*" "*InputDevices*" "*Remote*"`.
- [ ] Restart the owner's Nexa on the new build. After the owner runs `sudo ./packaging/install-helper.sh` and says
  go: `--hardcore --ask "run sleep 10"` → keyboard and touchpad dead for ~10 s, back after; `journalctl -u nexa-helper`
  shows the lock and its reason; then a lock ended from the web remote's Unlock PC.
- [ ] Commit "M20: hardcore mode".
