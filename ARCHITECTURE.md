# Nexa — Linux AI Assistant in C++20 / Qt6 Widgets (+ Android bridge)

> Living document: the approved plan plus a **Status** section. Update the status table at the end of every milestone.

## Status (updated 2026-10-10, M3+M4)

| Milestone | State |
|---|---|
| M0 — scaffold + docs | ✅ done — CMake skeleton, `ARCHITECTURE.md`, `CLAUDE.md`, presets, `.gitignore` |
| M1 — streaming chat on OmniRoute | ✅ done 2026-10-09 — verified live: OmniRoute `/v1/models` (3838 models), streamed Markdown replies with styled code, Esc cancel, Settings → Test connection; 51 unit/HTTP tests green (also under ASan/UBSan) |
| M2 — tools, fast path, agent loop, PC screen | ✅ done 2026-10-10 — verified live: fast path `open calculator` 2 ms, `volume 100` 45 ms, `run uname -sr` 17 ms, `close calculator` 8 ms (no model); compound request → agent ran `open_app` + `get_volume` + `run_command` in one round, all in the Actions panel and `audit.jsonl` (0600); `take a screenshot` → GNOME portal (no prompt, ~4 s) → vision model described the screen; SIGTERM mid-command → clean exit, process group killed; 99 test cases green (also ASan/UBSan, `-Werror`) |
| M3 — root helper + input injection | 🔄 verified in part 2026-10-10 — `nexa-helper` (polkit + uinput + job manager), `proto/` contract with generated proxy *and* adaptor, `HelperClient`, `RootCommandRunner`, the three input backends, six new tools and `packaging/`; 149 test cases green (also ASan/UBSan, `-Werror`). Live: installed, `busctl` shows the contract, `run_root_command ["id"]` → `uid=0(root)`, typed text landed in a native Wayland window. The password was being asked for on every call — now one polkit action and the answer remembered against the login (`SessionGrants`): verified live after the reinstall, six privileged calls from six separate processes (root commands and input alike) cost exactly one prompt, and the journal shows one `asking polkit` line. Absolute positioning moved to the RemoteDesktop portal and verified to the pixel (400,300 / 1500,800 / 960,540), with GNOME asking once. Nothing open |
| M4 — voice | ✅ done 2026-10-10 — verified live: a spoken "open the calculator" acted on it (OmniRoute `anyapi/google/gemini-2.5-flash-lite` heard it, `gtts/default` spoke the reply); offline whisper.cpp `base.en` + Piper did the same end to end in **611 ms**; "what is the capital of France?" went to the agent and was read back; 129 test cases green |
| M5 — Android bridge (ADB) | 🔄 built 2026-10-10 — `AdbClient`, `AdbDiscovery`, `AdbParsers`, `AdbShell`, `PhoneLookup`, `AndroidDevice`, the 18 `android_*` tools, `PhoneIntent`, Settings → Android (discovery list, pair with code, connect, remember, auto-reconnect), a *See phone* button and a phone indicator in the status bar, `adb reverse tcp:7777` at connect; 189 test cases green (also ASan/UBSan, `-Werror`). Verified without a phone: "see my phone", "on my phone open youtube" and "call 0300 1234567" take the fast path to the right tools, fail with how to connect one, and are audited. **Not yet verified live**: needs the user's phone with Wireless debugging on |
| M6 — web remote (phone → Linux) | 🔄 built 2026-10-10, paired live — `RemoteServer` (QHttpServer + WebSocket upgrade on the same ports), `RemoteSession`, `PairingManager`, `RemoteProtocol`, `TlsIdentity`, `QrCode`, `TtsFileRenderer`, the PWA in `remote/web/`, `RemoteBridge` in app/, Settings → Remote (QR, fingerprint, paired phones, revoke) and a status-bar indicator; 212 test cases green (also ASan/UBSan, `-Werror`), five of them driving the whole flow over real TLS and WebSockets. Live: a real phone (Chrome on Android) paired over HTTPS and connected; the server answered with every security header and the certificate covered host, localhost and the LAN IP. **Not yet seen live**: a request from the phone running a tool, and push-to-talk from the phone |
| M7 — breadth + polish | ✅ built 2026-10-10 — `AnthropicProvider` and `GeminiProvider` (native wire formats, streamed tool use), `SseChatStream` shared by all three providers, `TextToolParser` with a per-provider "cannot call tools" switch, `TrayIcon`, single instance, Nexa's own icon, hotkeys in Settings → General, README; 231 test cases green (also ASan/UBSan, `-Werror`). Live: a real tool conversation with Claude Sonnet 4.6 through `AnthropicProvider` (OmniRoute's `/v1/messages`) — the `tool_use` streamed and assembled, the result fed back as `tool_result`, the right answer; a second start handed its request to the running copy in 308 ms. **Not seen live**: Gemini against Google itself (no key here; covered end to end against a fake server), the tray icon on screen |
| M8 — native Qt-for-Android client | 🔄 built 2026-10-10, PC side verified — `remote/RemoteClient` (the phone's end: pair over HTTPS, WebSocket with the token, reconnect with backoff, trust on first use pinned to the certificate's SHA-256) and `android/` (Qt Widgets app `nexa-phone`: pair page, chat page with hold-to-talk, `Recorder` = QAudioSource → Nexa's `AudioConverter`, `SpeechPlayer` = Nexa's `wav::decode` → QAudioSink). It builds inside the Linux build too, so `tests/RemoteClientTest.cc` and `tests/PhoneWindowTest.cc` drive the real screens against the real server. APK: Qt 6.10.2 Android kit installed; the Android SDK/NDK download was still running when this was written — see README → "The phone app" |

### Deviations from the original plan
- Libraries join the build when their milestone starts (no empty placeholder targets). Today: `nexa_core`, `nexa_system` (static), `nexa` (app), `nexa_tests`.
- `Result<T>` carries an `ErrorCode` enum (not an int) and uses `isOk()`; `Result<void>` is the success-only variant.
- `Conversation` (planned for M2) landed in M1 as a minimal history + trimming class; M2 extends it with attachments and tool results.
- M1 stores the chosen model per provider (`ProviderConfig::defaultModel`); the fast/smart model slots arrive with the agent in M2.
- `CMakePresets.json` added: `release` → `build/`, `debug` → `build-debug/` (`-Werror`), `asan` → `build-asan/` (ASan + UBSan).
- OmniRoute preset starts on `agy/claude-sonnet-4-6` (fastest concrete model measured: ~2.3 s for a short reply); OmniRoute sends keep-alive chunks with an empty delta, which the decoder ignores.
- No `LICENSE` yet — to be chosen by the owner.
- M2: the PC intent handlers share one file (`core/intent/PcIntents.*`, one class per chain link); tools are `FunctionTool` adapters defined in `system/SystemTools.cc` instead of one class per tool; `XdgAppLauncher` + `ProcessCloser` became `system/apps/AppLauncher`, `FileTools` became `system/files/FileAccess`.
- M2: tool calls from one model turn run **in order**, not in parallel — desktop actions depend on each other ("open the editor, then type").
- M2: the desktop-entry index is rescanned on a worker thread at start-up (101 apps in ~30–60 ms), so the planned `~/.cache/nexa/apps.json` cache is not needed yet.
- M2: fast-path safety — `run <x>` only matches when `<x>` is an installed program (`$ <x>` always does), and bare chat words (`next`, `back`, `continue`) never trigger media control; compound requests always go to the agent.
- M2: `close_app` matches processes by exact name/executable (the kernel's 15-character `comm` is ambiguous, e.g. `gnome-calculator` vs `gnome-calculator-search-provider`). Apps whose launcher hands off to another process (e.g. `gnome-terminal` → `gnome-terminal-server`) cannot be closed by name; the agent falls back to `run_command`.
- Added (not in the plan): `nexa --ask "<text>"` (repeatable) submits requests at start-up — used for scripted verification, later for hotkeys; `UnixSignals` turns SIGTERM/SIGINT/SIGHUP into a clean quit so running commands' process groups are killed, never orphaned.
- Policy modes are named `autonomous` (default), `confirm-risky` (root + destructive) and `confirm-all`; confirmations are non-modal dialogs.
- **M3 was deferred and M4 built first.** The root helper and input injection remain planned; the voice milestone did not depend on them, so it went first.
- M4: the binary is **`nexa-assistant`** and the desktop id **`org.nexa.Assistant`**, not `nexa`. This machine already has Nexa Download Manager, which owns `/usr/bin/nexa`, `nexa.desktop`, the WM class `nexa` and `~/.config/Nexa/`; lowercase `~/.config/nexa/` and `~/.local/share/nexa/` stay ours. (Found when a spoken "open Nexa download manager" showed the assistant claiming NDM's desktop id.)
- M4: the planned `sttBackend`/`ttsBackend` live in `config.voice`, but the **cloud model names belong to the provider** (`ProviderConfig::sttModel/ttsModel/ttsVoice`) — they differ per endpoint, like `defaultModel`. Config version 2; `ProviderRegistry::upgrade()` fills the new fields in for known presets.
- M4: recognition and speech are **chains**, not single backends: `RecognizerChain` and `SynthesizedSpeech` try the provider first and the local engine next, skipping a link that just failed for a minute. Voice therefore survives OmniRoute being stopped or out of credits.
- M4: OmniRoute's whisper routes need paid credits (402 from `/v1/audio/transcriptions`) and `openai/*` has no key there, so the working defaults are a **Gemini chat model for recognition** (the recording goes in as `input_audio`; `CloudRecognizer` picks the endpoint from the model name) and **`gtts/default`** for the voice, which needs no account and takes the language as its `voice`. `edgetts/*` answers 403.
- M4: `WhisperRecognizer` sizes whisper's encoder window to the clip instead of its fixed 30 s (`audio_ctx`) — measured 4× faster on spoken commands (2.3 s → 0.5 s with `base.en`) for identical transcripts; `EnergyVad::trimSilence` cuts the quiet off first. Four threads, one per physical core (eight was 1.7× slower).
- M4: Piper runs as one long-lived process fed a sentence per line, writing a WAV per sentence into a private temporary directory (`--output_dir`), so no process starts per sentence. `PcmPlayer` pushes clips through a single `QAudioSink` so sentences play gapless, and `SentenceChunker` hands them over while the reply is still streaming.
- M4: `QTextToSpeech` is only the last-resort backend (`speechd`). The cloud and Piper return audio that `PcmPlayer` plays, which also lets M6 render a reply to a file for the phone. The planned `CloudTtsBackend`/`PiperTtsBackend` pair became `CloudSynthesizer`/`PiperSynthesizer` (PCM producers) plus `SynthesizedSpeech`; `MicButton` doubles as the pipeline's status light, so no separate indicator.
- M4: the global hotkeys use the portal's `GlobalShortcuts` — the compositor owns the keys and Nexa never reads the keyboard. GNOME 50 needed three things: a D-Bus connection of Nexa's own (the portal binds one app id per connection and Qt has already used the shared one), `org.freedesktop.host.portal.Registry.Register` to declare that id, and a **reverse-DNS** id backed by an installed `.desktop` file — anything else is discarded. Hence `scripts/install-desktop-entry.sh`. GNOME then ignores `preferred_trigger` and lists only shortcuts that already carry a key, so `BindShortcuts` succeeding with an empty list means "registered, assign the keys in Settings → Keyboard"; `ShortcutsChanged` delivers them later, without a restart. Ctrl+M, Esc and Ctrl+Alt+Esc work inside the window regardless.
- M4: the wake word accepts only a **local** recogniser (`ISpeechRecognizer::isLocal()`), so continuous listening never sends room audio to a server, and utterances without the wake phrase are dropped unlogged. `WakeWordDetector` matches the phrase in a transcript rather than spotting a keyword in audio.
- M4 added `--say "<text>"` and `--voice-file <wav>` beside `--ask` for scripted verification of the voice path. `voice/tts/TtsFileRenderer` waits for M6.
- M3: `proto/org.nexa.Helper.xml` generates **both** sides, the client proxy (`qt_add_dbus_interface`) and the service adaptor (`qt_add_dbus_adaptor`), so a method the helper does not implement breaks the build rather than failing at runtime. An array of integers (`au`, PressKeys) needs an explicit `org.qtproject.QtDBus.QtTypeName` annotation and `qDBusRegisterMetaType` on both sides; qdbusxml2cpp rejects it otherwise.
- M3: `IInputBackend` is **asynchronous**, not the planned `Result<void>`: the first call of a session waits for polkit's password prompt, which can take as long as the user does, and nothing may block the GUI thread.
- M3: the helper's absolute pointer is a **virtual-machine-style absolute device** with a fixed 0…65535 axis, not `INPUT_PROP_DIRECT` as sketched. A fixed range means the device does not care what resolution the caller measured on, and an absolute device is what makes "point at this pixel" possible at all — relative motion is reshaped by the compositor's pointer acceleration, so it cannot land on a given pixel.
- M3: **on GNOME the pointer is placed by the desktop's RemoteDesktop portal, not the helper.** Measured with a fullscreen X11 probe: GNOME 50 took the helper's clicks but never moved the pointer for its absolute device (with or without `INPUT_PROP_DIRECT` + `BTN_TOUCH` — udev still classifies a device with `BTN_LEFT` as an absolute *mouse*). `RemoteDesktopSession` opens a RemoteDesktop session with a ScreenCast monitor stream (the stream is what gives `NotifyPointerMotionAbsolute` a coordinate space; nothing reads it), registers `org.nexa.Assistant` with the host portal registry first, asks with `persist_mode` 2 and keeps the restore token in `~/.local/share/nexa/remote-desktop-token` (0600) — so GNOME asks once, ever — and closes after 20 idle seconds so the screen-sharing indicator does not linger. `CombinedInputBackend` sends only `mouseMoveAbs` there; the helper still types and clicks. Without the helper, `PortalInputBackend` does everything through the portal (evdev key and button codes, libinput scroll direction).
- M3: **mutter reads `NotifyPointerMotionAbsolute` in the stream's *pixels*, not the logical coordinates the portal documents** — it divides by the monitor scale, while reporting the stream's size in logical units. Sent logical coordinates at 125 %, every click landed at 80 % of the intended spot; sent pixels, 400,300 / 1500,800 / 960,540 landed exactly. `RemoteDesktopSession::unitsFor()` picks pixels on GNOME and the documented logical units elsewhere.
- M3: **three** uinput devices (keyboard, relative mouse, absolute pointer) rather than two. libinput decides what a device *is* from the events it claims, and a device with both relative and absolute axes is classified unpredictably.
- M3: `mouse_click` takes an optional position and moves there first, so the agent needs one call instead of two; `mouse_move_abs` and `mouse_click` read coordinates in the **last screenshot's** pixels by default, scaling with the shot's own factor (`ScreenCapture` remembers it), because that is the space the model can actually see.
- M3: `run_command` refuses a command that starts with `sudo`, `pkexec` or `doas` and points at `run_root_command`. Left alone, the model reached for sudo whenever the helper was missing, and sudo then waits for a password on a terminal that does not exist until the command times out.
- M3: a missing helper is `ErrorCode::NotSupported`, not `NotFound`. The fast path treats `NotFound` as "I could not find what you named" and quietly re-asks the model, which is right for an unknown app name and wrong for a dead end the user has to fix.
- M3: Nexa decides what it can do from whether the helper's D-Bus activation file **exists**, and only activates the daemon when a privileged action is actually requested — so launching Nexa never starts root-owned code.
- M3: `KeyMap` is US-layout only by design (uinput sends key codes and the compositor applies the user's own layout). Its tests caught three real faults: F11 and F12 computed from `KEY_F1` landed on **Num Lock and Scroll Lock** (the F-key codes are not contiguous), modifier order depended on how the chord was written, and "Ctrl+A" was read as Ctrl+Shift+A because of the capital letter.
- M3: the fast path's `press_keys` sent `["ctrl","s"]` while the tool's schema asks for `"ctrl+s"`; the intent now joins them, and the tool accepts either. `type …` no longer matches a noun phrase such as "type of file".
- M3: input tools are marked `privileged`, so `confirm-risky` mode asks before synthetic keystrokes even though nothing runs as root. The helper exposes **no** method that reads a key, a button, the pointer or the screen — it is an output device only, and the D-Bus contract says so.
- M3: `packaging/org.nexa.Helper.conf` lets any local context *send* to the helper, because D-Bus can only answer for the service as a whole. polkit is the real gate: `allow_any`/`allow_inactive` are `no`, so only the person at the active seat can authorise, per action.
- M3: **one** polkit action (`org.nexa.helper.control`), not the planned `run-command` + `inject-input`. Anything that can type into any window can open a terminal and run what it likes, so two actions were the same power asked for twice — and meant two password prompts.
- M3: `auth_admin_keep` alone does not deliver "ask once". polkit remembers its answer against the **process** that asked, and Nexa is many short-lived processes (every `--ask` run, every restart), so each one was a stranger to it and got a fresh prompt — the journal showed ten prompts for ten calls, each from a different bus name. Asking polkit about a `unix-session` subject instead would have the right lifetime but **aborts polkitd 127**, and each crash loses every authorisation it held. So the helper remembers it itself: `SessionGrants` records the login's id and start time in `/run/nexa-helper/grants.json` (tmpfs, 0700/0600, root), which survives the ten-minute idle exit and a polkitd restart but not a logout or a reboot. Only the *fact* that the user said yes is stored — the password goes to the desktop's own agent and is never seen by Nexa.
- M3: the caller's identity for all of this is its **uid as the bus reports it** (`serviceUid`), resolved through logind to that user's *display* session — the same fallback polkit itself uses, and necessary because a process started outside the login (from a terminal, or a scripted `--ask` run) belongs to no logind session of its own. Every call re-checks that the session is `Active` and not `Remote`, so the privilege goes away while another user is at the screen, not just when the password is first asked for.
- M5: two pure files beside `AdbParsers`, both unit-tested: `AdbShell` builds every command line sent to the phone and single-quotes anything that came from a person or a model (a message containing `; reboot` stays text), and `PhoneLookup` maps a spoken name to an app or a contact.
- M5: apps are listed with `cmd package query-activities --components … LAUNCHER` and started with `am start -n` — no `resolve-activity` round trip. Generic names ("camera", "messages", "calendar") become intents such as `CATEGORY_APP_MESSAGING`, so every vendor's own app answers; brand names (Gmail, WhatsApp) carry only their package, so a phone without Gmail says so rather than quietly opening another mail app.
- M5: `content query` columns are separated by `:` and the free-text column goes last (`address:date:body`), because values contain ", " and line breaks; the parser lets the last column run to the end of the row.
- M5: `android_sms_send` **composes** (SENDTO with the body filled in) and says so; sending takes a tap on Send, which the agent can do from a screenshot. `android_call`, `android_sms_send` and `android_install_apk` are `privileged`, so confirm-risky asks first — and `ToolSpec::risk` now gives the reason in words ("places a phone call", "types on your keyboard") instead of the old blanket "runs as root".
- M5: the phone is remembered by its **hardware serial** (`ro.serialno`), which is also inside its mDNS service name (`adb-<serial>-<suffix>`); the wireless debugging port changes every time it is switched on, so an address would go stale. `AdbDiscovery` polls only while Settings → Android is open (2.5 s) or while a remembered phone should be reconnected (15 s) — otherwise Nexa never starts adb until a phone tool is used.
- M5: the fast path names the phone before acting on a name: `call 0300 1234567` is a call, `call mom on my phone` is a call, `call mom` goes to the model, and `text <name>` needs both "on my phone" and "saying …". A wrong match here dials someone. `PhoneIntent` runs before the PC handlers so "open youtube on my phone" is not taken for a PC app.
- M5: found while testing the SMS phrases: the router's politeness filter strips a trailing "now", "thanks" or "for me" — so "type see you now" typed "see you". `IntentInput::raw` keeps the text as said (minus the wake word and opening politeness), and dictated payloads — `type …` and SMS bodies — come from it.
- M6: **two listeners**: TLS on all addresses at `port`, and plain HTTP on **loopback only** at `port + 1`. The plain one is for `adb reverse`: the phone opens `http://localhost:7777`, which adb carries (encrypted over wireless debugging) to the PC's `port + 1` — a secure context for the microphone with no certificate warning. Plain HTTP is never reachable from the network.
- M6: the WebSocket is served by **QHttpServer itself** (Qt 6.10's upgrade verifier) at `/ws`, on the same ports as the page — no second server. The verifier accepts only `/ws` and only a same-origin page (Origin's host:port = Host), so another site open in the phone's browser cannot drive the PC; requests also need the token.
- M6: paired phones live in their **own file**, `~/.config/nexa/remote-devices.json` (0600), not in config.json — only SHA-256 hashes of the tokens, so the file lets nobody in. Codes are six digits, valid five minutes and for one pairing, withdrawn after ten wrong guesses; `/api/pair` takes five attempts a minute per address and twenty in all. A revoked phone is disconnected at once. A code shown in Settings dies when the dialog closes.
- M6: a spoken request is **one binary WebSocket frame** (16 kHz mono Int16, ≤ 60 s) recorded in the page while the button is held (AudioWorklet, box-filter downsampling), not a stream of base64 `audio` messages. It is transcribed by `RemoteBridge`'s own recogniser and submitted with source **`phone-voice`**: the model gets the `[voice]` marker, the PC stays silent, and `TtsFileRenderer` renders the answer (or, for a fast-path action, what was done) as a WAV fetched from `/speech/<128-bit id>.wav` — a capability URL that expires in ten minutes.
- M6: the certificate is made in-process with the openssl CLI (EC P-256, 825 days, SAN = host name, localhost, 127.0.0.1 and the LAN address; key created under umask 077), not by a script, and made again only when it expires. Settings shows its SHA-256 fingerprint to compare with the browser's.
- M6: `remote/` depends only on core: `IRemoteHost` is what it needs from the rest of Nexa, and `app/RemoteBridge` implements it over AppController and the voice backends (`VoiceBackends::createSynthesizers` now builds the chain both the PC's voice and the phone's WAVs use). The page has a strict CSP (`default-src 'self'`, no inline script or style) and sets all server text with `textContent`.
- M7: one stream class for every provider. `SseChatStream` (Template Method) decodes events as they arrive with the provider's decoder, buffers error bodies and servers that ignore the stream flag, and completes a stream that closes without saying so; OpenAI moved onto it unchanged. A reply that made calls finishes as `ToolCalls` whatever its stop reason — Gemini says `STOP`.
- M7: `ToolCall::signature` carries Gemini's `thoughtSignature` through the conversation untouched; thinking models refuse the next turn of a tool conversation without it.
- M7: Anthropic and Gemini both need the conversation to open with the user, so history trimmed to fit drops whatever comes before the first user turn (an orphaned `tool_result` would be refused); consecutive turns of one role are merged, which also puts several tool results in one turn. Keys go in headers — `x-api-key`, `x-goog-api-key`, never the URL — plus a bearer token when the host is a gateway rather than the vendor (OmniRoute serves the Anthropic format at `/v1/messages`). Gemini's tool schemas are cut down to the OpenAPI subset it accepts, and a tool without arguments sends no `parameters` at all.
- M7: `TextToolParser`'s convention is `<tool_call>{"name": …, "arguments": {…}}</tool_call>`. For a provider marked "cannot call tools" the tools are listed in the system prompt, earlier calls and results are written into the history as text (such servers refuse `tool` messages), and the reply is shown when complete rather than streamed, so the markup never flashes up. Calls a native model writes as text are made too.
- M7: single instance through a `QLocalServer` socket in `$XDG_RUNTIME_DIR` (user-only): a second start hands over its `--ask`/`--say`/`--voice-file` and exits (the window comes forward only when nothing was handed over, so a scripted request never takes the focus); `--new-instance` starts a separate copy for testing. The tray icon is a StatusNotifierItem, which GNOME shows through its AppIndicator extension; with it present, closing the window hides it.
- Live use (2026-10-10): the agent's cap of **8 tool rounds** stopped real phone tasks ("Stopped after 8 tool rounds" four times in one evening: screenshot → tap → screenshot …). It is `agent.maxToolRounds` in config.json now, default 20, applied without a restart.
- M7: **no `theme.qss`**. Qt 6.10 follows GNOME's light/dark setting through the settings portal; a stylesheet of Nexa's own would fight the user's theme instead of matching it. Nexa got its own icon instead (`packaging/org.nexa.Assistant.svg`, shared by the window, the tray, the desktop entry and the phone page).
- M3: calls arriving while the password prompt is on screen **queue behind it** instead of each starting their own; the helper answers them all from the one decision. An agent turn with half a dozen keystrokes and clicks would otherwise mean half a dozen prompts.


## Context

`Ai Assistant.txt` asks for a complete Linux AI assistant: pluggable AI APIs (all providers/models), control of keyboard, mouse, speakers and screen, root-privileged help, voice in/out, running commands, opening any app, Qt6 frontend, written in C++ with design patterns and a modular approach (`methods_to_writecode.txt`). The user then added: connect with their Android phone in **both directions** (control Nexa from the phone; control the phone from Nexa), use **OmniRoute on `localhost:20128`** as the AI API, and keep the **plan/architecture document inside the project folder**.

The folder contains only the two notes, so this is a greenfield project created in `/home/muslimhackers/Desktop/Work/CPP-Projects/Ai Assistant/` (own `git init`; the home-dir backup repo only tracks `.claude`, so nesting is harmless). The path has a space, so every script quotes paths; if the user prefers the space-free name they typed (`AI-Assistant`), rename the folder before M0 — nothing below depends on the name.

### Decisions made by the user
- Name **Nexa**: namespace `nexa`, binary `nexa`, helper `nexa-helper`, D-Bus `org.nexa.Helper`, config `~/.config/nexa/`.
- UI: **Qt Widgets** (pure C++, no QML).
- Root: **root helper daemon on the D-Bus system bus guarded by polkit** (`auth_admin_keep` → password once per session). Helper also owns `/dev/uinput`.
- Voice: **local (whisper.cpp + speech-dispatcher/Piper) and cloud (OpenAI-compatible audio endpoints) both in v1**, push-to-talk + mic button, optional wake-word toggle (off by default).
- Safety: **fully autonomous** (no confirmation dialogs) as the default policy mode; still ships a policy toggle, a panic hotkey and an audit log.
- Screen: **in scope** — screenshots on demand ("see this"), model-initiated `take_screenshot`, attached to vision-capable models.
- Performance mandate: open apps / run commands / act **immediately**; call the LLM only when the request needs reasoning; memory- and speed-optimised.
- AI API: **OmniRoute (`http://localhost:20128/v1`, OpenAI-compatible) is the default provider**; other providers remain pluggable.
- Android: **Linux → phone via an ADB bridge** (no phone app needed); **phone → Linux via a web remote served by Nexa** (PWA in Chrome), native Qt-for-Android C++ app as a later milestone; v1 phone capabilities = automation (open apps/tap/type/keys), phone screenshot to vision model, notifications + SMS + calls, media + files.
- Docs: `ARCHITECTURE.md` (this plan) and `CLAUDE.md` (conventions + build commands) at the **project root**, kept updated per milestone.

### Verified environment (Kali 2026.2)
GNOME 50.4 on **Wayland** (XWayland `:0`), Qt **6.10.2** runtime only (dev pkgs installable), g++ 16.2, **no cmake/ninja yet**, PipeWire 1.6.6 (`wpctl` yes, `pactl` no), speech-dispatcher 0.12.1 (espeak-ng module only), mic + sink present, `/dev/uinput` root-only, polkitd + `pkexec`, user in `sudo` (polkit admin group), portals: Screenshot v2, RemoteDesktop v2, GlobalShortcuts v1, InputCapture v1. `libxkbcommon-dev`, `libx11-dev`, `libxtst-dev`, `libssl-dev` installed. apt has `libpolkit-qt6-1-dev`, `catch2`, `qt6-{base,multimedia,speech,tools,websockets,httpserver}-dev`, `adb` 1:34.0.5, `libqrencode-dev`; **whisper.cpp not in apt** (FetchContent `ggml-org/whisper.cpp` tag `v1.9.5`); **apt `piper` is the libratbag mouse GUI, not Piper TTS** (use rhasspy/piper prebuilt `2023.11.14-2`). No Android SDK / Qt-for-Android kit, no adb key yet, Java 25 present. Display eDP-1 1920×1080 at **1.25× fractional scale** (logical 1536×864). Keyboard layout `us`. OmniRoute at `localhost:20128` answers 401 on `/v1/models` (up, key required). LAN: `wlan0 192.168.18.44/24`, nftables present, avahi inactive (adb has its own mDNS). 216 `.desktop` entries. 8 cores, 7.6 GB RAM, Intel UHD 620 (CPU-only inference).

### Code conventions (mirror sibling `naturanza-food`)
PascalCase files matching the class, `.h`/`.cc`, `#pragma once`, namespace `nexa` not indented and closed with `} // namespace nexa`, classes PascalCase, methods/locals camelCase, private members `camelCase_`, 4-space indent, same-line braces, `public:` at column 0, brace member defaults (`int id{0};`), include order own → project (`"core/ai/IAiProvider.h"`) → third-party → std, smart pointers only, **no exceptions** (`Result<T>` + Qt error signals), CMake ≥3.16 with `CMAKE_EXPORT_COMPILE_COMMANDS ON`. C++20.

---

## Architecture

```
app (Qt Widgets GUI, composition root)                 helper (root daemon, system bus, uinput)
  │                                                       │
  ├──► remote (HTTPS + WebSocket server, embedded PWA)    ▼
  ▼                                                     proto (D-Bus XML + KeyMap shared by both sides)
voice ──► core ◄── system (commands, apps, audio, input, screen, android/ADB, helper client) ──► proto
```
Five static libs (`proto`, `core`, `system`, `voice`, `remote`) + two executables (`nexa`, `nexa-helper`) + `tests`. Dependencies strictly downward; `core` has no Qt Widgets and no syscalls. The phone is reached two ways: **outbound** through `system/android` (ADB over Wi‑Fi/USB) and **inbound** through `remote` (phone browser → Nexa).

**Patterns (named in code comments):** Factory/Registry (`ProviderRegistry`, `ToolRegistry`, backend factories), Strategy (`IInputBackend`, `ISpeechRecognizer`, `ITextToSpeech`, `ICommandRunner`), Chain of Responsibility (`IntentRouter` fast-path handlers), Command (every action is an `ITool` invocation with `cancel()`), Observer (Qt signals), Facade (`HelperClient` over D-Bus, `AndroidDevice` over adb), State machine (`VoicePipeline`), Builder (`ChatRequestBuilder`), Adapter (`RemoteSession` adapts the WS protocol to `AppController`), constructor dependency injection everywhere (no singletons).

### Fast path first, LLM second (the speed mandate)
`core/intent/IntentRouter` runs **before** the agent on every utterance/text (from the GUI, voice or the phone). Deterministic handlers (regex over a normalised string) map directly to `ToolRegistry` invocations — no network, target < 50 ms:

| Phrase pattern | Tool |
|---|---|
| `open/launch/start <app>` | `open_app` (fuzzy match on cached desktop index: name, generic name, keywords, exec basename; threshold ≥ 0.8 else LLM) |
| `close/kill <app>` | `close_app` |
| `volume up/down [N]`, `set volume to N`, `mute/unmute` | `set_volume` / `mute` |
| `play/pause/resume/next/previous/stop music` | `media_control` |
| `type <text>`, `press <key chord>` | `type_text` / `press_keys` |
| `run/execute <cmd> [as root]` | `run_command` / `run_root_command` |
| `screenshot`, `see this`, `look at (this/my screen)`, `what's on my screen` | `take_screenshot` → attach image and forward to LLM |
| `on my phone open <app>`, `phone volume N`, `phone play/pause` | `android_open_app` / `android_volume` / `android_media` |
| `see my phone`, `what's on my phone` | `android_screenshot` → attach and forward to LLM |
| `call <number/contact>`, `text/sms <contact> <message>` | `android_call` / `android_sms_send` |
| `stop/cancel/panic` | `PanicController` |
| anything else / low confidence | `Agent` (LLM via OmniRoute by default) |

Fast-path actions go through the same `ToolRegistry`, so audit log and policy apply uniformly.

### Performance & memory principles
- Desktop-entry index built once in a worker thread at startup, cached at `~/.cache/nexa/apps.json` (invalidate on dir mtime); launch = `QProcess::startDetached` on the parsed `Exec=` (field codes stripped, `Terminal=true` honoured) or D-Bus activation for `DBusActivatable=true`; `gio launch` only as fallback.
- Commands: `QProcess` without a shell unless the string contains shell syntax (`| > < & ; $ * ?`), own process group, streaming output.
- LLM: streaming always, parse on `readyRead`, HTTP/2 keep-alive via one shared `QNetworkAccessManager`, compact tool schemas, small system prompt, "fast" and "smart" model slots in settings (OmniRoute: pick concrete models — its `auto/*` routing combos are slow).
- TTS starts on the first sentence boundary of the streamed reply (sentence-chunked), not after the full answer.
- whisper: `base.en` default (`small.en` opt-in), `n_threads = cores`, greedy, `single_segment`, `no_context`, model kept loaded while voice is enabled, unloaded after a configurable idle timeout; inference in a worker `QThread`.
- Screenshots (PC and phone) downscaled (long side ≤ 1280 px) and JPEG-encoded before attaching; deleted after use. The tool result states the image size and scale, and `mouse_move_abs` / `android_tap` accept coordinates in the last screenshot's image space and scale them back to real pixels.
- Phone: one persistent `adb` server, one `adb shell` per call (no interactive shell), package list cached; `adb reverse` set up once at connect.
- No QML/WebEngine, static libs, `-O2` Release, helper daemon exits when idle (D-Bus activated), single-instance app via `QLocalServer` (hotkey re-raises the window).

---

## File tree

```
Ai Assistant/
├── CMakeLists.txt                 C++20, options NEXA_BUILD_WHISPER / NEXA_BUILD_TESTS, find_package(Qt6 Core Gui Widgets DBus Network Multimedia TextToSpeech WebSockets HttpServer), subdirs
├── cmake/WhisperCpp.cmake         FetchContent ggml-org/whisper.cpp v1.9.5; WHISPER_BUILD_{TESTS,EXAMPLES,SERVER} OFF, BUILD_SHARED_LIBS OFF, GGML_NATIVE OFF (CACHE vars set before fetch)
├── ARCHITECTURE.md                this plan (architecture, milestones, status) — updated per milestone
├── CLAUDE.md                      conventions, build/run/test commands, where things live
├── README.md, .gitignore, LICENSE
├── proto/
│   ├── org.nexa.Helper.xml        D-Bus contract (single source of truth; qt_add_dbus_interface + qt_add_dbus_adaptor)
│   ├── KeyMap.h/.cc               char/key-name → (KEY_*, needsShift) for US layout; pure, unit-tested
│   └── HelperTypes.h              JobStream, MouseButton, error codes
├── core/
│   ├── Result.h                   Error{code,message} + Result<T> (no exceptions)
│   ├── Logging.h/.cc              QLoggingCategory per module
│   ├── config/Config.h/.cc, ConfigStore.h/.cc     typed settings ↔ ~/.config/nexa/config.json (dir 0700, file 0600, atomic write)
│   ├── net/HttpClient.h/.cc       QNetworkAccessManager wrapper, cancellable, streaming readyRead
│   ├── net/SseParser.h/.cc        incremental text/event-stream framing (pure)
│   ├── ai/IAiProvider.h, ChatRequest.h/.cc (messages, text+image content, ChatRequestBuilder), ChatStream.h/.cc (cancellable; signals textDelta, toolCallDelta, finished, failed)
│   ├── ai/ProviderRegistry.h/.cc  factory by kind + presets (OmniRoute http://localhost:20128/v1 = default, OpenAI, Ollama :11434, LM Studio :1234, Groq, OpenRouter, DeepSeek, Mistral, xAI, Together, Anthropic, Gemini)
│   ├── ai/OpenAiProvider.h/.cc, AnthropicProvider.h/.cc, GeminiProvider.h/.cc
│   ├── ai/ToolCallAccumulator.h/.cc   merges streamed tool-call fragments per provider (pure)
│   ├── tools/ToolSpec.h/.cc       name/description/JSON schema + toOpenAi()/toAnthropic()/toGemini()
│   ├── tools/ITool.h, ToolRegistry.h/.cc (lookup, schema export, parallel dispatch of independent calls), TextToolParser.h/.cc (JSON-block fallback for models without tool support)
│   ├── intent/IIntentHandler.h, IntentRouter.h/.cc, handlers/{AppIntent,AudioIntent,MediaIntent,InputIntent,CommandIntent,ScreenIntent,PhoneIntent,ControlIntent}.h/.cc
│   ├── agent/Agent.h/.cc          loop: stream → collect tool calls → execute → append results → repeat → final text
│   ├── agent/Conversation.h/.cc   history, trimming, system prompt assembly, pending image attachments (+ last-screenshot scale per source)
│   ├── policy/PolicyEngine.h/.cc  mode autonomous (default) / confirm-root-destructive / confirm-all; destructive regex list
│   ├── policy/PanicController.h/.cc   cancels streams, tools, child process groups, TTS, voice, adb jobs
│   └── audit/AuditLog.h/.cc       append-only JSONL ~/.local/share/nexa/audit.jsonl (never logs API keys/tokens)
├── system/
│   ├── commands/ICommandRunner.h, LocalCommandRunner.h/.cc (QProcess, setsid process group, killpg), RootCommandRunner.h/.cc (via HelperClient)
│   ├── apps/IAppLauncher.h, DesktopEntryIndex.h/.cc (XDG_DATA_DIRS parse, cache, fuzzy score), XdgAppLauncher.h/.cc, ProcessCloser.h/.cc
│   ├── audio/IAudioController.h, WpctlAudioController.h/.cc (wpctl get/set-volume, set-mute @DEFAULT_AUDIO_SINK@)
│   ├── media/MprisController.h/.cc   org.mpris.MediaPlayer2.* enumerate, Play/Pause/Next/Previous
│   ├── input/IInputBackend.h, HelperInputBackend.h/.cc (uinput via helper; primary), RemoteDesktopSession.h/.cc (RemoteDesktop + ScreenCast portal session: one-time consent kept as a restore token, idle close, pixel-exact NotifyPointerMotionAbsolute), PortalInputBackend.h/.cc (everything through that portal, no root), CombinedInputBackend.h/.cc (helper types and clicks, portal places the pointer), XTestInputBackend.h/.cc (x11 sessions only), InputBackendSelector.h/.cc (probe + human-readable reasons)
│   ├── screen/ScreenCapture.h/.cc  portal org.freedesktop.portal.Screenshot (interactive=false, Request/Response), downscale+encode, returns {image, scale, realSize}
│   ├── android/AdbClient.h/.cc     QProcess facade over `adb`: devices -l, mdns services, pair, connect, shell, exec-out (binary), push/pull, install, reverse; per-call timeouts; cancel
│   ├── android/AdbDiscovery.h/.cc  polls `adb mdns services` for _adb-tls-pairing/_adb-tls-connect, auto-connect to the known phone's current port, USB detection
│   ├── android/AndroidDevice.h/.cc high-level ops: tap/swipe/typeText/key, openApp (cmd package resolve-activity → am start), listApps (pm list packages + label cache), screenshot (exec-out screencap -p), screenSize (wm size), notifications (dumpsys notification), smsRead (content query content://sms), smsSend (SENDTO composer intent + send key; documented OEM caveat), call (am start -a CALL), media/volume (input keyevent / cmd media_session), push/pull/install
│   ├── android/AndroidTools.h/.cc  ITool implementations: android_status, android_open_app, android_list_apps, android_tap, android_swipe, android_type, android_key, android_screenshot, android_notifications, android_sms_read, android_sms_send, android_call, android_media, android_volume, android_push_file, android_pull_file, android_install_apk, android_shell
│   ├── android/AdbParsers.h/.cc    pure parsers for devices -l, mdns services, wm size, dumpsys notification, content query rows (unit-tested)
│   ├── android/AdbShell.h/.cc      pure command builders: quoting for the phone's sh, input text (%s), key names → KEYCODE_*, call/SMS intents (unit-tested)
│   ├── android/PhoneLookup.h/.cc   pure matching: spoken app name → launcher component or generic intent, spoken name → contact (unit-tested)
│   ├── helper/HelperClient.h/.cc   facade: availability, version, calls, job signal routing
│   ├── info/SystemInfo.h/.cc      distro/kernel/CPU/mem/session type
│   └── files/FileTools.h/.cc      read_file / write_file (path allow-list under $HOME by default)
├── voice/                         (M4, built)
│   ├── Wav.h/.cc                  RIFF/WAVE encode + decode to Int16 (8/16/24/32-bit, float, unknown data size); pure
│   ├── VoiceBackends.h/.cc        factory for Settings → Voice: provider models first, local engines behind them; knows ~/.local/share/nexa paths
│   ├── VoiceTools.h/.cc           the `speak` tool
│   ├── capture/IAudioInput.h, AudioCapture.h/.cc (QAudioSource, re-reads the negotiated format), AudioConverter.h/.cc (any format → 16 kHz mono Int16, box-filter down / linear up, chunk-boundary safe; pure), EnergyVad.h/.cc (noise floor = quietest frame of the last 1.5 s; pre-roll, hangover, trimSilence, containsSpeech; pure)
│   ├── stt/ISpeechRecognizer.h (isLocal() gates the wake word), CloudRecognizer.h/.cc (/audio/transcriptions for whisper-like models, else the clip as chat `input_audio`), WhisperRecognizer.h/.cc (worker thread, model unloaded after 10 min idle, abort callback, audio_ctx sized to the clip; NEXA_BUILD_WHISPER), RecognizerChain.h/.cc (tries each in turn), Transcript.h/.cc (drops "[BLANK_AUDIO]", "(music)", "♪"; pure)
│   ├── tts/ITextToSpeech.h, ISpeechSynthesizer.h, CloudSynthesizer.h/.cc (/audio/speech, asks for WAV), PiperSynthesizer.h/.cc (one long-lived process, a sentence per line, --output_dir), AudioFileDecoder.h/.cc (MP3/Ogg → PCM via QAudioDecoder), PcmPlayer.h/.cc (one QAudioSink, gapless queue, instant stop), SynthesizedSpeech.h/.cc (chain + two sentences of lookahead), SpeechdTtsBackend.h/.cc (QTextToSpeech, last resort), SentenceChunker.h/.cc (streamed Markdown → speakable sentences; pure)
│   └── pipeline/VoicePipeline.h/.cc (idle→listening→transcribing→thinking→speaking; push-to-talk, tap for hands-free, barge-in, wake word, reply slots), WakeWordDetector.h/.cc (wake phrase + command out of a transcript; pure)
├── remote/
│   ├── RemoteServer.h/.cc         QHttpServer (static PWA from Qt resources + /api/pair + /tts/<id>.wav) + QWebSocketServer (SecureMode with self-signed cert from ~/.config/nexa/tls/, generated via `openssl req -x509` on first run); binds 0.0.0.0:7777 (configurable)
│   ├── RemoteSession.h/.cc        one WS client: token check, JSON protocol ↔ AppController signals (text/audio in; state/delta/final/action/tts out), rate limits
│   ├── PairingManager.h/.cc       6-digit code + QR (libqrencode) shown in Settings/Android tab; code → long-lived token (stored hashed in config); revoke list
│   ├── RemoteProtocol.h/.cc       message structs + encode/decode, both directions (pure, unit-tested)
│   ├── RemoteClient.h/.cc         the phone's end (M8): RemoteEndpoint (address/invitation parsing; loopback = plain HTTP), probe → fingerprint, pair, WebSocket with hello/ping/backoff, trust on first use pinned to the certificate's SHA-256
│   └── web/index.html, app.js, style.css, manifest.json, sw.js   PWA: chat with streamed replies, action log, push-to-talk (AudioWorklet → 16 kHz Int16 PCM over WS; optional Web Speech API text mode), reply audio playback, reconnect with stored token
├── android/                       Nexa Phone (M8): a Qt Widgets app, built for the desktop inside this build (nexa_phone_ui + nexa-phone) and on its own for the APK
│   ├── CMakeLists.txt             standalone project with the Qt for Android kit; in-tree it links nexa_remote/nexa_voice, standalone it compiles the shared sources by path
│   ├── main.cc                    composition root
│   ├── PhoneWindow.h/.cc          Mediator: pages, client events → conversation lines, user → requests, the fingerprint dialog
│   ├── PairPage.h/.cc, ChatPage.h/.cc   address + code + name; the list, the line, hold-to-talk, ⋮ → forget this PC
│   ├── PhoneSettings.h/.cc        QSettings: endpoint, token, trusted fingerprint, device name
│   ├── Recorder.h/.cc, SpeechPlayer.h/.cc   QAudioSource → AudioConverter → one utterance; wav::decode → QAudioSink
│   └── manifest/res/drawable-*/icon.png    launcher icons rendered from packaging/org.nexa.Assistant.svg
├── helper/
│   ├── main.cc                    QCoreApplication, register org.nexa.Helper on system bus, idle-exit timer
│   ├── HelperService.h/.cc        adaptor impl: authorise (delayed reply) then dispatch; calls arriving while the prompt is up queue behind it
│   ├── PolkitAuthorizer.h/.cc     PolkitQt1::Authority::checkAuthorization(SystemBusNameSubject(sender), AllowUserInteraction)
│   ├── LoginSession.h             the caller's graphical login: uid, logind session id + start time, active, remote
│   ├── LoginSessions.h/.cc        logind adapter: uid (from the bus) → that user's display session, and whether it is the one at the screen
│   ├── SessionGrants.h/.cc        which logins have already been authorised (/run/nexa-helper/grants.json, 0600) — polkit remembers per *process*, which is the wrong lifetime
│   ├── JobManager.h/.cc           child processes in own process groups, OutputChunk/Finished signals, cancel = killpg
│   └── UinputDevice.h/.cc         virtual keyboard + relative mouse device, and absolute-pointer device (EV_ABS + INPUT_PROP_DIRECT)
├── app/
│   ├── main.cc                    composition root (wires everything, single-instance guard)
│   ├── AppController.h/.cc        owns Agent/IntentRouter/VoicePipeline/Policy/RemoteServer/AndroidDevice; mediates GUI + remote ↔ core
│   ├── MainWindow.h/.cc, ChatView.h/.cc (markdown via QTextDocument::setMarkdown, in-place streaming, 10 Hz re-render), ActionLogDock.h/.cc, MicButton.h/.cc (click = hands-free, hold = push-to-talk, level meter, state colour), SettingsDialog.h/.cc (tabs: Providers, Voice, Policy, General; later Helper, Android, Remote), GlobalShortcuts.h/.cc (portal GlobalShortcuts: own D-Bus connection, Registry.Register, reverse-DNS app id), UnixSignals.h/.cc, TrayIcon.h/.cc (M7)
│   └── resources/theme.qss, icons/
├── tests/                         Catch2 v3: SseParser (adversarial splits), ToolCallAccumulator ×3 providers, ToolSpec mapping, TextToolParser, KeyMap, IntentRouter grammar (incl. phone phrases), DesktopEntry Exec parsing, PolicyEngine, ConfigStore round-trip, Agent (scripted model), VoicePure (Wav, AudioConverter, EnergyVad, SentenceChunker, WakeWordDetector, Transcript, MultipartForm, cloud request/reply mapping), VoicePipeline (fake microphone/recogniser/voice), AdbParsers, RemoteProtocol, PairingManager, RemoteServerIntegration and RemoteClient (real TLS sockets), PhoneWindow (the phone app's screens driven against the real server; TestMain runs a QApplication on the offscreen platform)
├── packaging/
│   ├── org.nexa.Assistant.desktop desktop entry (id must be reverse-DNS: the portal needs it to grant global hotkeys)
│   ├── org.nexa.helper.policy     one polkit action org.nexa.helper.control, auth_admin_keep, allow_any/allow_inactive = no
│   ├── org.nexa.Helper.conf       /etc/dbus-1/system.d: root owns; default context may send/receive
│   ├── org.nexa.Helper.service    /usr/share/dbus-1/system-services: User=root, SystemdService=nexa-helper.service
│   ├── nexa-helper.service        systemd Type=dbus, BusName=org.nexa.Helper, ExecStart=/usr/local/libexec/nexa-helper (light hardening only — it must run arbitrary root commands and reach /dev/uinput)
│   ├── install-helper.sh (copies files, `systemctl daemon-reload`, `busctl` self-check), uninstall-helper.sh
└── scripts/download-whisper-model.sh (ggml-<model>.bin → ~/.local/share/nexa/models, SHA-256 from Hugging Face's X-Linked-ETag), download-piper.sh (rhasspy/piper 2023.11.14-2 x86_64 + one voice → ~/.local/share/nexa/piper, pinned hash), install-desktop-entry.sh (per-user entry, --uninstall), gen-tls-cert.sh (M6: self-signed cert, SAN = LAN IP + hostname)
```

---

## Key interfaces

```cpp
// core/Result.h
struct Error { int code{0}; QString message; };
template <typename T> class Result { /* ok(T) / fail(Error); bool ok() const; T& value(); const Error& error() const; */ };

// core/ai/IAiProvider.h
class IAiProvider : public QObject {
public:
    struct Capabilities { bool streaming{true}; bool nativeTools{true}; bool vision{false}; bool audioStt{false}; bool audioTts{false}; };
    virtual Capabilities capabilities() const = 0;
    virtual void listModels(std::function<void(Result<QStringList>)> cb) = 0;
    virtual std::shared_ptr<ChatStream> chat(const ChatRequest &request) = 0;   // streams textDelta / toolCallDelta / finished(FinishReason) / failed(Error)
};

// core/tools/ITool.h
class ITool {
public:
    virtual const ToolSpec& spec() const = 0;                                    // name, description, QJsonObject parametersSchema, bool privileged, bool mutating
    virtual void invoke(const QJsonObject &args, std::function<void(Result<ToolResult>)> done) = 0;   // ToolResult{ QString text; QImage image; double imageScale; }
    virtual void cancel() = 0;
};

// core/intent/IIntentHandler.h
struct IntentMatch { QString toolName; QJsonObject args; double confidence{0}; bool attachScreenshot{false}; bool attachPhoneScreenshot{false}; bool forwardToLlm{false}; };
class IIntentHandler { public: virtual std::optional<IntentMatch> match(const QString &normalisedText) const = 0; };

// system/input/IInputBackend.h
class IInputBackend {
public:
    virtual QString name() const = 0;
    virtual Result<void> typeText(const QString &text) = 0;
    virtual Result<void> pressKeys(const QStringList &chord) = 0;            // {"ctrl","shift","t"}
    virtual Result<void> mouseMove(int dx, int dy) = 0;
    virtual Result<void> mouseMoveAbs(int x, int y) = 0;                      // physical pixels of the primary output
    virtual Result<void> mouseButton(MouseButton b, bool down) = 0;
    virtual Result<void> mouseScroll(int dx, int dy) = 0;
};

// system/android/AdbClient.h  (all async; signals finished(Result<AdbOutput>))
class AdbClient : public QObject {
public:
    void run(const QStringList &args, int timeoutMs, std::function<void(Result<QByteArray>)> done);   // adb [-s serial] args...
    void shell(const QString &cmd, int timeoutMs, std::function<void(Result<QString>)> done);
    void execOut(const QString &cmd, int timeoutMs, std::function<void(Result<QByteArray>)> done);     // binary-safe (screencap)
    void pair(const QString &hostPort, const QString &code, std::function<void(Result<void>)> done);
    void connectTo(const QString &hostPort, std::function<void(Result<void>)> done);
    void reverse(int phonePort, int pcPort, std::function<void(Result<void>)> done);
    void cancelAll();
};

// remote/RemoteProtocol.h — JSON over WebSocket
// client→server: {"t":"hello","token"} {"t":"text","text"} {"t":"ptt","down":bool} {"t":"audio","pcm16k":base64} {"t":"cancel"} {"t":"ping"}
// server→client: {"t":"welcome","version","voice":bool} {"t":"state","voice":"listening"|...} {"t":"delta","text"} {"t":"final","text"} {"t":"action","tool","args","result","ms"} {"t":"tts","url"} {"t":"error","message"}

// voice/stt/ISpeechRecognizer.h — Strategy; isLocal() is what lets the wake word listen continuously
class ISpeechRecognizer : public QObject {
public:
    using Done = std::function<void(Result<QString>)>;
    virtual QString name() const = 0;
    virtual bool isLocal() const = 0;                                             // audio never leaves the machine
    virtual void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) = 0;   // 16 kHz mono Int16
    virtual void cancel() = 0;
    virtual void warmUp() {}
};

// voice/tts/ITextToSpeech.h — speaks; speak() queues, so sentences can arrive while one is playing
class ITextToSpeech : public QObject {
public:
    virtual QString name() const = 0;
    virtual void speak(const QString &text) = 0;
    virtual void stop() = 0;                      // silences now, drops the queue, emits no finished()
    virtual bool isSpeaking() const = 0;
    virtual void warmUp() {}
signals: void started(); void finished(); void failed(const nexa::Error &);
};

// voice/tts/ISpeechSynthesizer.h — engines that return audio instead of playing it (cloud, Piper);
// SynthesizedSpeech chains them and PcmPlayer plays the clips back to back.
class ISpeechSynthesizer : public QObject {
public:
    using Done = std::function<void(Result<PcmAudio>)>;                           // Int16 PCM + rate + channels
    virtual void synthesize(const QString &text, Done done) = 0;
    virtual void cancel() = 0;
};

// voice/pipeline/VoicePipeline.h — the state machine; AppController wires request() to submit()
// and the reply signals back in, so the GUI, voice and later the phone are peers.
enum class VoiceState { Idle, Listening, Transcribing, Thinking, Speaking };
```

Tools registered: PC — `run_command`, `run_root_command`, `open_app`, `close_app`, `list_apps`, `type_text`, `press_keys`, `mouse_move`, `mouse_move_abs`, `mouse_click`, `mouse_scroll`, `set_volume`, `mute`, `media_control`, `speak`, `take_screenshot`, `read_file`, `write_file`, `get_system_info`. Phone — the 18 `android_*` tools listed in the file tree.

---

## Root helper (`nexa-helper`)

**D-Bus contract** (`proto/org.nexa.Helper.xml`, generated proxy + adaptor so both sides can't drift):
```
Ping() · Version() → s · IsAuthorized() → b · ForgetAuthorization()
RunCommand(as argv, s cwd, as env "K=V", i timeoutMs) → t jobId     CancelJob(t jobId)
signal OutputChunk(t jobId, y stream, ay data) · signal Finished(t jobId, i exitCode, s status)
TypeText(s utf8) · PressKeys(au keycodes)  [press in order, release in reverse]
MouseMove(i dx, i dy) · MouseMoveAbs(i x, i y, i screenW, i screenH) · MouseButton(u button, b down) · MouseScroll(i dx, i dy)
```
**Authorisation — the password is asked for once per *login*, not once per caller.** On each privileged call: `setDelayedReply(true)`; ask the bus for the caller's uid (`serviceUid(message().service())` — never a caller-supplied identity); ask logind for that uid's display session and refuse unless it is `Active` and not `Remote` (the same rule as `allow_inactive`/`allow_any` = no, enforced on *every* call, including ones that never reach polkit); if `SessionGrants` already holds that login, do the work; otherwise `checkAuthorization("org.nexa.helper.control", SystemBusNameSubject(sender), AllowUserInteraction)` and record the login on success. Calls that arrive while the prompt is on screen queue behind it, so six quick actions are one prompt. Log every polkit decision to the journal.

One action, not two: anything that can type into any window can open a terminal, so `run-command` and `inject-input` were the same power asked for twice. `auth_admin_keep` on its own is **not** enough — polkit ties what it remembers to the asking *process*, and Nexa is many short-lived processes over a day, so the prompt came back every time (and asking polkit about a `unix-session` subject instead makes polkitd 127 abort, losing everything it had remembered). Hence `SessionGrants`: the *fact* that the user said yes, against the login's id + start time, in `/run` (tmpfs, 0700/0600 root) so it survives the helper's idle exit but not a logout or a reboot. Revoked by Settings → Helper → Forget my password (`ForgetAuthorization`, which also clears polkit's own grant), or by logging out.

**uinput:** open `/dev/uinput` once; keyboard device: `UI_SET_EVBIT EV_KEY` + every `KEY_*` in KeyMap, `EV_REL` with `REL_X/REL_Y/REL_WHEEL/REL_HWHEEL`, `BTN_LEFT/RIGHT/MIDDLE`; `UI_DEV_SETUP` + `UI_DEV_CREATE`. Each logical action = `input_event` writes + `EV_SYN/SYN_REPORT`; ~3 ms between key events. Second device for absolute pointer: `EV_ABS` with `UI_ABS_SETUP` ranges `0..screenW-1`, `0..screenH-1` and `INPUT_PROP_DIRECT` so Mutter maps it to the whole output — coordinates are **physical pixels** (same space as the portal screenshot), which sidesteps the 1.25× scale factor; multi-monitor targets the primary output (documented). `UI_DEV_DESTROY` on exit.

**Keymap:** uinput emits keycodes; the compositor applies the user's XKB layout. `proto/KeyMap` covers printable ASCII (`(KEY_*, shift)`) and named keys (`enter tab esc backspace delete space up/down/left/right home end pageup/pagedown f1..f12 ctrl alt shift super`). Non-ASCII text → best-effort clipboard + Ctrl+V (`QClipboard` from the GUI; on Wayland this only works while Nexa has focus, so the tool reports the limitation in its result instead of failing silently).

**Lifecycle:** D-Bus activation + `Type=dbus` systemd unit; idle-exit after 10 min without calls. Install via `packaging/install-helper.sh` (needs the user's sudo: run as `! sudo ./packaging/install-helper.sh`).

---

## Screen vision ("see this" / "see my phone")

PC: `system/screen/ScreenCapture` calls `org.freedesktop.portal.Screenshot.Screenshot("", {handle_token, interactive:false})` and waits for `org.freedesktop.portal.Request.Response` → `uri`. GNOME asks once ("Allow Nexa to take screenshots?" with remember) and persists. Phone: `AndroidDevice::screenshot()` via `adb exec-out screencap -p`. Both → `QImage`, downscale to ≤1280 px, JPEG → base64 → image content block (OpenAI `image_url`, Anthropic `image/base64`, Gemini `inlineData`), and the tool text reports `image WxH, scale s, source pc|phone` so `mouse_move_abs` / `android_tap` can map image coordinates back to real pixels. Triggers: `ScreenIntent`/`PhoneIntent` phrases, the model's `take_screenshot` / `android_screenshot` tools, camera buttons in the UI. If the active model lacks `vision`, the chat shows an actionable error (switch model) rather than silently dropping the image.

---

## Android bridge (Linux → phone, `system/android`)

- **Setup (one-time, in Settings → Android):** phone: Developer options → Wireless debugging → "Pair device with pairing code". Nexa: `AdbDiscovery` lists `_adb-tls-pairing._tcp` services from `adb mdns services`; user enters the 6-digit code → `adb pair host:port code` → `adb connect` to the `_adb-tls-connect` port. USB works without pairing (just "Allow USB debugging"). Serial + model + Android version shown; the phone's serial is remembered and auto-reconnected (wireless port changes per session — discovery handles it; after a phone reboot the user re-enables wireless debugging).
- **Automation:** `input tap/swipe/text/keyevent` (`input text` is ASCII-only and slow per char — documented; spaces as `%s`), `cmd package resolve-activity --brief -c android.intent.category.LAUNCHER <pkg>` + `am start -n`, app matching = package-name tokens + cached labels, `wm size` for bounds.
- **Notifications / SMS / calls:** `dumpsys notification --noredact` parsed into (app, title, text, when); `content query --uri content://sms/inbox --projection address,body,date --sort "date DESC"`; send = `am start -a android.intent.action.SENDTO -d sms:<num> --es sms_body "<text>"` then `android_screenshot` + tap "Send" (agentic; direct `service call isms` is version-specific, so not used); call = `am start -a android.intent.action.CALL -d tel:<num>`; contacts via `content query --uri content://com.android.contacts/data/phones`.
- **Media / files:** `input keyevent KEYCODE_MEDIA_PLAY_PAUSE|NEXT|PREVIOUS`, `media volume --show --stream 3 --set N` (then `cmd media_session volume`, which prints nothing on some phones; then `dumpsys audio`'s STREAM_MUSIC block for the position and `KEYCODE_VOLUME_UP/DOWN` to walk there), `adb push/pull`, `adb install -r`.
- `adb reverse tcp:7777 tcp:7777` is issued at connect so the phone can open the web remote at `http://localhost:7777` (secure context, no certificate warnings).
- All ADB actions are `ITool`s → audited; `PanicController::cancelAll()` kills in-flight adb processes.

## Web remote (phone → Linux, `remote/`)

- `RemoteServer` on `0.0.0.0:7777`: HTTPS (self-signed cert generated at first run; SAN = LAN IP + hostname) serving the embedded PWA, and a WSS endpoint. Reachable at `https://192.168.18.44:7777` (one-time "proceed" in Chrome) or `http://localhost:7777` through `adb reverse`.
- **Pairing:** Settings → Remote shows a QR (`https://<ip>:7777/#pair=<code>`) and the 6-digit code; the page exchanges it for a token (`/api/pair`, 5 attempts/min), stores it in `localStorage`, and sends `hello` on every WS connect. Tokens are stored hashed; revocable in settings.
- **PWA:** chat with streamed deltas, action log, PTT button (AudioWorklet → 16 kHz Int16 PCM frames → `audio` messages → `VoicePipeline` → same STT backends), optional Web Speech API text mode, playback of `tts` wav rendered by `TtsFileRenderer`, auto-reconnect, "Add to Home Screen" manifest.
- Each `RemoteSession` is a peer of the GUI: it submits utterances to `AppController` (IntentRouter → fast path or Agent) and mirrors `state/delta/final/action` events. nftables note in README: `nft list ruleset` and allow tcp/7777 if a drop policy exists.
- **The phone app (M8, `android/`):** a native Qt Widgets client speaking the same `RemoteProtocol` through `remote/RemoteClient`. Pairing types the address (or pastes the QR link) and the code; over TLS the app first *probes* the certificate and shows its SHA-256 fingerprint for the user to compare with Settings → Remote — trust on first use, pinned: afterwards every TLS error must be about exactly that certificate, anything else is refused before a byte is sent. A loopback address means plain HTTP (adb reverse). The microphone is `QAudioSource` → Nexa's `AudioConverter` → one binary frame; the spoken reply is fetched under the same trust and played with `QAudioSink` from `wav::decode`, so no media framework has to trust the certificate on its own. Needs the Qt 6.10 Android kit (aqt), the Android SDK + NDK r27, a JDK 17+; the system Qt serves as host (`QT_HOST_PATH=/usr`).

---

## Voice pipeline

Capture 16 kHz mono Int16 via `QAudioSource` (verify the negotiated format; resample with a simple linear resampler if the device refuses) → `EnergyVad` (RMS threshold, 400 ms hangover) → segment → STT (`WhisperRecognizer` in a worker thread, or `CloudRecognizer`) → text → `IntentRouter` → fast path or `Agent` → reply → `SentenceChunker` → TTS (`SpeechdTtsBackend` default; `PiperTtsBackend` if `~/.local/share/nexa/piper/piper` exists; `CloudTtsBackend`). Remote PCM frames enter the same pipeline (one active source at a time). Modes: **push-to-talk** (hold/toggle hotkey via portal GlobalShortcuts, mic button, or the phone's PTT) and **wake word** (continuous VAD; each short segment transcribed and matched against "hey nexa"/"nexa"). Barge-in: VAD speech during `speaking` → `stop()` TTS → `listening`. Panic hotkey (default `Ctrl+Alt+Escape`, registered in portal and in-app) and the words "stop/cancel" → `PanicController`.

---

## AI providers

Shared `HttpClient` (one `QNetworkAccessManager`, `setTransferTimeout(0)` for streams, read on `readyRead`) + `SseParser` (stateful, chunk-boundary safe — the most-tested class in the repo).
- **Default = OmniRoute** (`kind: openai-compatible`, `baseUrl: http://localhost:20128/v1`): first run asks for the API key, then `GET /v1/models` fills the model combos (401 → "key missing/invalid" banner; connection refused → "OmniRoute not running"). Fast slot and smart slot are both OmniRoute models by default; avoid `auto/*` combos.
- **OpenAI-compatible:** `POST {base}/chat/completions` `stream:true` (+ `stream_options.include_usage`), `tool_calls` deltas merged per `index`; images via `image_url` data URLs; STT `/v1/audio/transcriptions`, TTS `/v1/audio/speech`. Presets for OpenAI, Ollama, LM Studio, Groq, OpenRouter, DeepSeek, Mistral, xAI, Together.
- **Anthropic:** `/v1/messages` with `anthropic-version`, typed SSE events, `tool_use` + `input_json_delta` accumulation by block index, `/v1/models`.
- **Gemini:** `models/{m}:streamGenerateContent?alt=sse`, `functionDeclarations` / `functionCall` / `functionResponse`, `inlineData` images, `/v1beta/models`.
- Models without native tools → `TextToolParser` fallback with a system-prompt convention; the provider's `Capabilities` decide the path.
- Config `~/.config/nexa/config.json` (0600): providers[] {kind, name, baseUrl, apiKey, defaultModel}, activeProvider, fastModel/smartModel, voice {sttBackend, whisperModel, ttsBackend, voice, wakeWord, pttHotkey}, policy {mode, destructivePatterns}, hotkeys, screenshots {maxSide, deleteAfterUse}, android {serial, autoReconnect}, remote {enabled, port, tokens[]}.

---

## GUI (Qt Widgets)

`MainWindow`: chat transcript (`ChatView`, streaming tokens mutate the last item; markdown re-render on a 100 ms timer), input row (text field, send, mic button with level meter + pipeline state colour, PC-camera and phone-camera buttons), toolbar combos for provider/model + fast-model, `ActionLogDock` (tool, args, result, ms, source = gui/voice/phone), status bar (helper status, input backend, STT/TTS backend, phone connected, remote clients), `TrayIcon` (listen toggle, panic, settings, quit), `SettingsDialog` tabs: Providers (keys/base URLs/model refresh), Voice, Policy, Hotkeys, Helper (install status + "why not" reasons from `InputBackendSelector`), Android (discovery list, pair code entry, status), Remote (enable, port, QR + code, paired devices, revoke). Dark `theme.qss`.

---

## Setup & build

```bash
# user runs (needs password):
#   ! sudo apt install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev qt6-speech-dev qt6-tools-dev qt6-websockets-dev qt6-httpserver-dev libpolkit-qt6-1-dev libxkbcommon-dev libqrencode-dev adb catch2
cmake --preset release      # = -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEXA_BUILD_WHISPER=ON -DNEXA_BUILD_TESTS=ON
cmake --build build              # first whisper build is multi-minute; day-to-day builds stay fast
./scripts/download-whisper-model.sh base.en          # offline speech recognition
./scripts/download-piper.sh en_US-lessac-medium      # offline voice
./scripts/install-desktop-entry.sh                   # per-user entry; the global hotkeys need it
sudo ./packaging/install-helper.sh                   # M3: nexa-helper + polkit/dbus/systemd files
./build/app/nexa-assistant
```

---

## Milestones (each buildable and demoable)

**M0 — project scaffold + docs in the repo.** `git init`, `.gitignore` (build/, models, config), `ARCHITECTURE.md` (this plan), `CLAUDE.md` (conventions, build/test commands, layout), top-level + per-lib `CMakeLists.txt` with empty targets that configure and build. Done: `cmake --build build` succeeds with the stub targets; `ARCHITECTURE.md` and `CLAUDE.md` exist at the project root.

**M1 — streaming chat on OmniRoute.** `Result`, `Logging`, `Config/ConfigStore`, `HttpClient`, `SseParser`, `ChatRequest/ChatStream`, `OpenAiProvider`, `ProviderRegistry` (OmniRoute default preset + first-run key prompt), minimal `MainWindow` + `ChatView` + `AppController`, settings dialog (Providers tab). Done: a message to OmniRoute streams tokens into the window; model combo filled from `/v1/models`; cancel mid-stream works. Tests: `SseParser`, `ConfigStore`, OpenAI `ToolCallAccumulator`.

**M2 — tools, fast path, agent loop, PC screen.** `ToolSpec/ITool/ToolRegistry`, `Agent`, `Conversation`, `PolicyEngine`, `PanicController`, `AuditLog`, `IntentRouter` + PC handlers, `LocalCommandRunner`, `DesktopEntryIndex/XdgAppLauncher/ProcessCloser`, `WpctlAudioController`, `MprisController`, `SystemInfo`, `FileTools`, `ScreenCapture`, `ActionLogDock`. Done: "open firefox" launches in < 50 ms without the LLM; "open a terminal and set volume to 30%" goes through the agent with every step in the action log + `audit.jsonl`; "see this" attaches a screenshot and a vision model describes the screen. Tests: `IntentRouter`, `DesktopEntry`, `PolicyEngine`, `ToolSpec` mapping.

**M3 — root helper + input injection.** `proto/` XML + generation, `KeyMap`, `helper/` (`PolkitAuthorizer`, `JobManager`, `UinputDevice`, `HelperService`), `HelperClient`, `RootCommandRunner`, `HelperInputBackend`, `XTestInputBackend`, `InputBackendSelector`, `packaging/` + install script. Done: `busctl --system list | grep org.nexa` shows the service; a root command prompts once, then runs silently; `type_text`/`press_keys` land in a native Wayland window (GNOME Text Editor); `mouse_move_abs` from screenshot coordinates clicks the right spot. Tests: `KeyMap`.

**M4 — voice.** `AudioCapture`, `EnergyVad`, `WhisperRecognizer`, `CloudRecognizer`, three TTS backends, `SentenceChunker`, `VoicePipeline`, `WakeWordDetector`, `MicButton`, `GlobalShortcuts` (portal + fallback), Voice settings tab, model/piper download scripts. Done: PTT → transcript → action/answer → spoken reply; barge-in cuts speech; GUI stays responsive during inference; panic kills an in-flight child process group; wake word toggle works. Tests: `EnergyVad`.

**M5 — Android bridge (ADB).** `AdbClient`, `AdbDiscovery`, `AdbParsers`, `AndroidDevice`, `AndroidTools` (18 tools), `PhoneIntent`, Settings → Android tab, phone-camera button, `adb reverse` at connect. Done: pair + connect from settings over Wi‑Fi; "on my phone open youtube" launches it in < 300 ms; "see my phone" → screenshot described by the model; `android_tap` from image coordinates hits the right element; notifications and SMS inbox are read; a call and an SMS composer are started; play/pause + volume work; push/pull a file. Tests: `AdbParsers`, `IntentRouter` phone phrases.

**M6 — web remote (phone → Linux).** `RemoteServer`, `RemoteSession`, `PairingManager`, `RemoteProtocol`, `TtsFileRenderer`, `web/` PWA, Settings → Remote tab with QR, `gen-tls-cert.sh`. Done: phone opens `https://192.168.18.44:7777` (or `http://localhost:7777` via adb reverse), pairs with the code, sends "open firefox" → PC opens Firefox and the phone shows the action; PTT from the phone → transcript → streamed reply + spoken wav on the phone; token survives reload; revoked token is rejected. Tests: `RemoteProtocol`, `PairingManager`.

**M7 — breadth + polish.** `AnthropicProvider`, `GeminiProvider`, `TextToolParser`, remaining accumulator tests, `TrayIcon`, single-instance, theme, full `SettingsDialog`, README, `.desktop`, `ARCHITECTURE.md` status update. Done: the same tool-using conversation works on OmniRoute, Anthropic and Gemini; `ctest` green.

**M8 (later) — native Qt-for-Android client** on the same `RemoteProtocol` (requires Android SDK/NDK, Qt 6.10 Android kit, JDK 17).

---

## Verification

- Unit: `ctest --test-dir build` (Catch2). Highest-value: adversarially split SSE streams, per-provider tool-call accumulation, KeyMap round-trip, IntentRouter grammar, AdbParsers, RemoteProtocol.
- Chat: OmniRoute key entered in settings → model list appears → incremental tokens; cancel mid-stream; `curl -s -H "Authorization: Bearer $KEY" http://localhost:20128/v1/models` as the reference.
- Fast path: time `open firefox` / `volume 30` / `on my phone open youtube` in the action log (< 50 ms PC, < 300 ms phone); confirm no LLM request was made.
- Helper: `busctl --system introspect org.nexa.Helper /org/nexa/Helper`, `pkaction --action-id org.nexa.helper.control --verbose`, `journalctl -u nexa-helper -f` while running `run_root_command ["id"]` → one password prompt then `uid=0`.
- Asked once, not again: after that first prompt, run several privileged actions from *separate* Nexa processes (`--ask` runs count) and from both tool families — `journalctl -u nexa-helper | grep "asking polkit"` must show exactly one line per login. `sudo systemctl stop nexa-helper` and repeat: still no prompt (the grant is in `/run`, not in memory). Settings → Helper → Forget my password, then one more action → exactly one new prompt.
- Input: type a shifted ASCII sentence into GNOME Text Editor via `type_text`; `press_keys ctrl+a`; `mouse_move_abs` to a screenshot-derived coordinate and click.
- Screen: "see this" → `audit.jsonl` shows `take_screenshot`, the model describes the screen, the file is removed afterwards; "see my phone" likewise via `screencap`.
- Voice: `--voice-file <wav>` replays a recording as a spoken request (Piper can make one); PTT hotkey from another focused app; wake word "hey nexa, open files"; barge-in while TTS is speaking; panic hotkey during `run_command ["sleep","60"]` kills it (`pgrep sleep` empty). The granted hotkeys appear in Settings → Keyboard once `scripts/install-desktop-entry.sh` has run.
- Android: `adb devices -l` shows the phone after pairing from settings; after toggling the phone's Wi‑Fi, discovery reconnects; `android_notifications` lists a test notification; `android_sms_read` shows the inbox; `android_call` opens the dialer with the number.
- Remote: from the phone, pair via QR; send text and PTT; kill Wi‑Fi briefly and confirm auto-reconnect; revoke the token in settings and confirm the phone is disconnected; `nft list ruleset` has no rule dropping tcp/7777.
- MPRIS: play/pause against the running Chrome player.

## Risks & gotchas
**Found in M4:** the portal grants global hotkeys only to an app id that is reverse-DNS *and* backed by an installed `.desktop` file, on a D-Bus connection that has not already talked to the portal (Qt has, at start-up) — all three fail silently as "refused", so check `journalctl --user | grep -i shortcut` and `dconf dump /org/gnome/settings-daemon/global-shortcuts/`. The grant dialog and the key assignment both need a human; nothing can script them, so the hotkey cannot be verified unattended. Microphone format: `QAudioSource` may ignore the requested 16 kHz mono (re-read `source->format()` after `start()`, as `AudioCapture` does). Barge-in without headphones makes Nexa hear its own voice, so it is off by default. whisper's default 30 s encoder window costs 4× the time on a one-second command. OmniRoute's audio model ids come from its own registry, not `/v1/models`, and its whisper routes need credits.

SSE chunk boundaries and provider-specific tool-call deltas (pure, tested first); Wayland input only via uinput (XTest reaches XWayland apps only); non-ASCII typing limited to clipboard best-effort on the PC and absent on the phone (`input text` ASCII-only); fractional scaling and downscaled screenshots — keep all pointer math in real pixels via the recorded scale; polkit/D-Bus install errors surface as opaque `AccessDenied` (install script self-checks with `busctl`/`pkaction`); whisper first build time + model download (option-gated, clear GUI error when the model is missing); `QAudioSource` silently negotiating a different rate (re-read format); `QNetworkReply` buffering (parse on `readyRead`); child cleanup needs process groups (`setsid` + `killpg`); the space in the project path (quote everything); `apt piper` is not Piper TTS; keep inference, process I/O and adb calls off the GUI thread; wireless debugging port changes per session and switches off after a phone reboot (mDNS discovery + USB fallback); `content query` SMS / `dumpsys notification` output varies by OEM (parsers tolerant, unit-tested on captured samples); secure screens (banking, DRM) screenshot black; browser mic needs a secure context (HTTPS self-signed one-time accept, or `adb reverse` + `http://localhost`); nftables may block 7777; WS reconnect on flaky Wi‑Fi (token + backoff); OmniRoute `auto/*` routing is slow — default to concrete models; fully-autonomous mode means `destructivePatterns` only annotate the audit log unless the user flips the policy mode.
