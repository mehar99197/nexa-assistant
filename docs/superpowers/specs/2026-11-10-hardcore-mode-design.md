# M20 — Hardcore mode: the laptop is Nexa's until the task is done

Date: 2026-11-10 · Status: approved by the owner (design in chat) · Branch: `m20-hardcore`

## Why

The owner: "add hardcore work mode in nexa — before starting a task, if set on hardcore work mode, then block access,
even me, to use my own laptop mouse and keyboard till task completion; just screen on, just I can see". A task that
types and clicks (the Screen agent, the browser in the user's Chrome) is easily spoiled by a touch of the touchpad;
in hardcore mode nobody at the laptop can interfere, and the owner watches.

Decided with the owner (2026-11-10):

- **Approvals stay locked**: a hardcore task that needs the owner's yes waits for it by voice ("approve" / "deny") or
  from the phone; the keyboard and mouse do not come back for it.
- **No time limit**: locked until every hardcore task is done, failed, stopped or paused.
- **Ways out early**: the voice ("Nexa, unlock"; "Nexa, stop everything" stops the tasks too) and Nexa Phone / the
  web remote (an **Unlock PC** button). Nexa closing or crashing always unlocks. A keyboard or mouse plugged in
  during the lock is locked too (not an escape).
- **The switch**: "Hardcore" in the input bar beside Share screen (every task started while it is on), the same
  checkbox in Add tasks; off at every start.
- **Revised by the owner (2026-11-10, after the first live run)**: while a hardcore lock is held, what ends that work
  early — unlock, stopping or pausing a hardcore task, "stop everything" — is taken **only from Nexa Phone, the web
  remote, or the owner's own enrolled voice** (the speaker gate said so for that request); anyone else's voice, a
  request typed or scripted on the PC, is refused with a sentence that says so. Hardcore mode is switched on only when
  one of those exists: a paired phone with the web remote on, or an enrolled voice with the wake word on.

## How it locks

The kernel's own **input inhibit** (`/sys/class/input/inputN/inhibited`, Linux 5.11+): a device written `1` delivers
no event to anyone — the compositor, the console, SysRq — and the kernel releases the keys held at that moment, so
nothing sticks. Only root may write it, so the **root helper** does it. Rejected: `EVIOCGRAB` (keys held when it
starts stay down in GNOME, and the helper would open the devices for reading — against its write-only rule) and a
full-screen window that eats input (GNOME's own shortcuts still pass on Wayland, and it would block Nexa's clicks).

**What is locked**: every *physical* device (its sysfs path not under `/devices/virtual/`) that is a **keyboard**
(EV_KEY with KEY_A, KEY_Z, KEY_SPACE and KEY_ENTER) or a **pointer** (EV_REL with REL_X, REL_Y and BTN_LEFT — a
mouse; EV_ABS with ABS_X, ABS_Y and BTN_TOUCH, BTN_TOOL_FINGER or BTN_LEFT — a touchpad, touchscreen or tablet). On
this laptop: "AT Translated Set 2 keyboard" (input0) and "SynPS/2 Synaptics TouchPad" (input6). Left alone: the power
button and the lid (logind's), the video bus' brightness keys, the jacks, the PC speaker, a keyboard's separate
media-key interface (it cannot type or click), and **Nexa's own virtual devices** (uinput, under `/devices/virtual/`) —
so the task still types and clicks, and the portal's pointer (Mutter's) is untouched. A device someone else
inhibited is neither recorded nor released.

## The helper (`helper/`, `proto/org.nexa.Helper.xml`)

- `LockInput() → as names` — privileged (the same polkit action and per-login grant as typing). Inhibits the devices
  above, records each (`/run/nexa-helper/locked-input.json`: sysfs name, device name, physical path) and returns their
  names. **Idempotent**: calling it again while locked rescans (a device plugged in since is locked too) and counts as
  Nexa's heartbeat. Only one login's process holds the lock: a call from another caller while locked is refused.
- `UnlockInput()` — not polkit-privileged (giving up needs no password) but only from the caller that locked, or root.
- `InputUnlocked(s reason)` — the helper ended the lock on its own.
- It ends the lock by itself when: the caller's bus name goes (Nexa quit or crashed); no `LockInput` came for 30 s
  (Nexa hung — it calls every 5 s; a liveness check, not a time limit); the login stops being the one at the screen.
- `nexa-helper --release-input` releases what the record names and exits; the unit runs it as `ExecStopPost`, so a
  crashed or stopped helper never leaves the laptop locked. The helper runs it at start too. It does not idle-exit while
  locked. A device being added (`/dev/input` watched) while locked is locked at once.
- Pure and tested: `helper/InputDevices` (sysfs capability bitmaps → keyboard / pointer / other) and `helper/DeviceLock`
  (lock, rescan, unlock and release-from-record over a sysfs root and a record path, so tests use a fake tree).
- The helper still reads no input: it reads device *metadata* (names, capability bitmaps) and writes `inhibited`.

## Nexa

- `core/mission/IInputLock` (Strategy): `lock(done(Result<QStringList>))`, `unlock()`, `lost(reason)`, and whether it
  can (the helper installed and new enough). `system/input/HelperInputLock` implements it over `HelperClient`: the
  5 s heartbeat, `InputUnlocked`, the helper vanishing; it holds `system/screen/ScreenAwake` (the portal's Inhibit, idle
  and suspend; GNOME's session manager when the portal refuses) while locked — the screen never blanks or locks.
- `core/mission/HardcoreMode` (State; owned by AppController, observes MissionControl): the switch; states **Off**,
  **Armed** (on, nothing to lock), **Counting** (3 s before locking, so the hand leaves the keys and a fast-path task
  never locks), **Locking** (the helper asked; the password prompt may be up), **Locked**, **Failed** (the lock was
  refused — the tasks run unlocked and the banner says why). Locked while any hardcore task is *active*: queued,
  running, awaiting the owner or delegating (not finished, not paused). `unlockNow(reason)`: the switch off, every
  task's hardcore flag cleared, the lock released. A `lost` lock does the same and says why.
- `Task::hardcore`, `TaskRequest::hardcore`: set from the switch (`AppController::submit`, the drawer's follow-up) or
  Add tasks' checkbox; a sub-task inherits its lead's.
- Voice and chat (`core/intent/MissionIntents`): `Control::Unlock` — "unlock", "unlock the pc / computer / laptop /
  keyboard / mouse", "give me my keyboard back", "hardcore off", "turn off / stop / end hardcore (mode)";
  `Control::HardcoreOn` — "hardcore on", "hardcore mode", "turn on hardcore (mode)". "Unlock my phone" is not either.
  "Stop everything" (panic) stops the tasks and turns hardcore off.
- While locked, a hardcore task's approval request is **always spoken** ("CODE-02 needs your approval to run a
  command: … Say approve or deny."), whatever the announce setting, and goes to the phones.
- Window: the **Hardcore** switch (`#hardcoreSwitch`, Ctrl+Shift+H) asks once when turned on and names the ways out
  that exist now (the wake word on → the voice; a phone paired and the remote on → the phone); with neither it warns
  that only the task's end or the power button ends the lock. A banner (`#hardcoreBanner`, every design) says
  "Locking your keyboard and mouse in 3…", "Keyboard and mouse locked until “…” finishes — say “Nexa, unlock” or use
  Nexa Phone", or why it could not lock. A desktop notification at lock and unlock. `--hardcore` turns the switch on
  for a scripted run without asking.

## Phone and web remote

- Server → phone: `state` carries `locked` (and `lockText`); `approval {id, agent, tool, question}` and
  `settled {id}` (pending ones sent after `welcome`).
- Phone → server: `unlock`; `answer {id, allow}`.
- `IRemoteHost` gains `isLocked`, `unlock`, `approvals`, `answer`; `RemoteBridge` implements them over AppController.
- The web page and Nexa Phone show a red **PC locked** bar with **Unlock PC** while locked, and an approval card with
  Approve / Deny per pending question.

## Tests

`InputDevicesTest` (this laptop's own bitmaps), `DeviceLockTest` (fake sysfs: only the keyboard and touchpad, the
record, idempotent rescan, a new device, foreign inhibit left alone, unlock, release from the record),
`HardcoreModeTest` (fake lock + the mission rig: countdown, lock, finish → unlock, queue keeps it, pause → unlock,
resume → lock again, unlockNow, lost, refused → Failed, a sub-task inherits), `IntentRouterTest` (the phrases and
negatives), `AppControllerTest` (unlock by voice, panic, the spoken approval), `RemoteProtocolTest` (the messages),
`RemoteServerTest` (unlock and answer reach the host), `AddTasksDialogTest` (the checkbox). Live, after the owner
reinstalls the helper and says go: a 10-second lock of this laptop (`--hardcore --ask "run sleep 10"`), released
by the task's end, then one released by "Nexa, unlock" from the phone page.
