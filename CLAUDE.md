# Nexa — guide for Claude Code

Linux AI assistant in C++20 + Qt 6 Widgets (Kali, GNOME on Wayland). The full design, the owner's
decisions and the milestone status live in **ARCHITECTURE.md** — read its *Status* section first and
update it at the end of every milestone.

## Build, run, test

The project path contains a space (`Ai Assistant`): always quote paths in shell commands and scripts.

```bash
cmake --preset release                      # = cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/nexa-assistant
```

Debug build (`-Werror`): `cmake --preset debug && cmake --build build-debug`. Sanitizers: `cmake --preset asan && cmake --build build-asan && ./build-asan/tests/nexa_tests`. The tests run
under a QApplication (offscreen), and Qt's GUI plugins leave allocations behind at exit that LeakSanitizer
reports: run with `ASAN_OPTIONS=detect_leaks=0` for the memory-error check, and judge leaks by whether any
frame is `in nexa::` (`… 2>&1 | grep "in nexa::"`).
Verbose logs: `QT_LOGGING_RULES="nexa.*.debug=true" ./build/app/nexa-assistant` (categories `nexa.app`,
`nexa.config`, `nexa.net`, `nexa.ai`, `nexa.voice`, `nexa.mcp`, `nexa.browser`).
Scripted run: `./build/app/nexa-assistant --ask "open calculator" --ask "volume 50"` — submits each request in
turn, exactly as if typed. **If Nexa is already running, the requests go to that copy** (single instance, through a
socket in `$XDG_RUNTIME_DIR`); add `--new-instance` for a separate test copy (to keep a test's memories, skills and
browser profile out of the real ones, or to try another model, give it a private copy:
`XDG_CONFIG_HOME=/tmp/x/config XDG_DATA_HOME=/tmp/x/data`, with `playwright`, `models` and `piper` symlinked into
`/tmp/x/data/nexa`) — it still reads the real config, so
run it with `QT_QPA_PLATFORM=offscreen` to keep it off the user's screen (it still registers the global Talk hotkey and
the web remote's port like the real one, so a press of the hotkey during a scripted run goes to the test copy). `--say "<text>"` speaks, `--voice-file <wav>` feeds a recording in as a spoken
request; the three run in that order, one at a time. **To check the look**, add `--screenshot <file.png>`: once the
requests are done and Nexa is idle, it saves a picture of the window and quits — it works offscreen, so
`QT_QPA_PLATFORM=offscreen ./build/app/nexa-assistant --new-instance --theme light --ask "run uname -sr" --screenshot /tmp/nexa.png`
shows the light scheme with a fast-path reply without touching the screen (`--theme <palette>` and `--design <design>`
are for that run only and never reach the settings; `--design console` shows the terminal design in its own palette);
`--screenshot-settings <dir>` saves every Settings tab the same way, then the Providers tab of each program (agy,
opencode, Codex: its sign-in status read) and the provider gallery (`provider-gallery.png`, and searched for "free");
the pictures of a sign-in under way (the method menu, the login link) need **`NEXA_SNAPSHOT_SIGN_IN=1` and a private
config naming the fake programs** — `XDG_CONFIG_HOME=/tmp/x/config` with providers of kind `agy` and `codex` whose
`baseUrl` is `tests/fake-agy/agy` / `tests/fake-codex/codex`, and `tests/fake-opencode` first on the `PATH` (opencode is
found there; each fake's header says which `FAKE_*` file holds who is signed in) — because without the variable only
status checks run: a started sign-in opens the real browser (agy does it itself) and puts the login link in the
picture, so never set it against the owner's own config. **Without a phone**, `tests/fake-adb/adb` stands in
for adb with a Pixel always connected: `PATH="$PWD/tests/fake-adb:$PATH"` before the command above, plus
`--ask "on my phone open youtube"` so the phone is looked up, shows the Phone panel with a drawn screen in it.
The phone app's screens come out of
`NEXA_TEST_SCREENSHOTS=<dir> ./build/tests/nexa_tests "The phone app pairs*"`, and the web remote's page can be looked
at in any browser without a phone through `scripts/preview-web-remote.py` (a stand-in server that plays a
conversation and walks the core through its states; `--once` for a screenshot, e.g. with
`google-chrome --headless=new --screenshot=… --virtual-time-budget=8000 "http://127.0.0.1:8765/#pair=123456"`).
On the real screen, a copy started with `QT_QPA_PLATFORM=xcb --new-instance` can be captured with `xwd -id` (there is
no `convert` here: decode the XWD in Python) and the tray icon GNOME holds can be read back from
`busctl --user get-property <nexa's bus name> /StatusNotifierItem org.kde.StatusNotifierItem IconPixmap`.
The binary is **nexa-assistant** and the desktop id **org.nexa.Assistant** — `nexa`, `/usr/bin/nexa` and
`nexa.desktop` belong to Nexa Download Manager, a different product. `scripts/install-desktop-entry.sh`
installs the entry for the current user; the global hotkeys need it (the portal rejects an app id it cannot
find, and GNOME insists on a reverse-DNS one). GNOME then wants the keys assigned by hand in Settings →
Keyboard; inspect the result with `dconf dump /org/gnome/settings-daemon/global-shortcuts/`.
To inspect the GUI from a script, run it on XWayland (`QT_QPA_PLATFORM=xcb`) and capture with `xwd -id <window>`; do **not** drive it with `xdotool type` — under Wayland, synthetic keystrokes only reach whichever X11 window has focus.

Voice engines (optional, no sudo): `scripts/download-whisper-model.sh base.en` for offline recognition,
`scripts/download-piper.sh` for the offline voice, and `scripts/download-sherpa.sh` for sherpa-onnx 2.0.1 and its
models (`~/.local/share/nexa/sherpa`: the keyword spotter that hears the wake name, Silero voice detection, GTCRN
noise reduction — loaded at run time with `QLibrary` against the vendored `voice/wake/sherpa/c-api.h`, nothing
linked; without it the energy VAD and the whisper second net work alone), all into `~/.local/share/nexa`. The
cloud recogniser and voice are the provider chosen in Settings → Voice, or automatically the first choice and then
every provider with a speech model (agy, opencode and Codex have none). Local recognition is compiled in only with
`-DNEXA_BUILD_WHISPER=ON` (on in the release and debug presets; whisper.cpp is fetched at configure time).
`--hear <wav>` (with `--new-instance` implied) plays a recording into the microphone with the wake word on, so the
whole wake → request → reply → follow-up path runs as if the room had said it; with
`QT_LOGGING_RULES="nexa.voice.debug=true"` the log says what the spotter heard and why it was taken or ignored.

Root helper (needs sudo — **the user runs it**, as `! sudo ./packaging/install-helper.sh`): installs
`nexa-helper` so Nexa can run commands as root and type/click on Wayland. The script self-checks with
`pkaction` and `busctl`, and `uninstall-helper.sh` removes every file it added. Without it, `run_root_command`
and the input tools report the install command instead of failing quietly. Inspect it with
`busctl --system introspect org.nexa.Helper /org/nexa/Helper` and `journalctl -u nexa-helper`.
It asks for the password once per login: `journalctl -u nexa-helper | grep "asking polkit"` should show one
line per login, whatever the number of calls or Nexa processes. More than one means the authorisation is not
being remembered — check `/run/nexa-helper/grants.json` and `journalctl -u polkit | grep ABRT`.

Build dependencies (one-time, needs sudo — the user runs it):
`sudo apt install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev qt6-speech-dev qt6-tools-dev qt6-websockets-dev qt6-httpserver-dev libpolkit-qt6-1-dev libxkbcommon-dev libqrencode-dev adb catch2`

Runtime: settings in `~/.config/nexa/config.json` (0600, holds API keys). The providers are tried in order as a
`ProviderChain`: the **first choice** (Settings → Providers), then every provider ticked "Use when the providers above cannot
answer", each with its own model; the chain moves on only when one cannot answer (not running, not installed, not
signed in, out of quota) and nothing has streamed, and the reply's header then says `via <provider>`. The defaults
are the owner's own programs, on their own sign-in and quota, no key in Nexa: **opencode** — Nexa starts its own
`opencode serve` on loopback (password made per start, in the child's environment only; folder
`~/.cache/nexa/opencode`; about 10 s to be ready) — and **agy** (Google's
Antigravity CLI, `~/.local/bin/agy`): one warm `agy --input-format stream-json` process per conversation, run with a
home folder of Nexa's own (`~/.cache/nexa/agy/home`, whose `settings.json` denies agy's own tools — print mode would
otherwise read files unasked) and an empty workspace (`~/.cache/nexa/agy/work`); `agy models` takes about 16 s. A third
program, **Codex** (OpenAI's coding agent, on the owner's ChatGPT account), is added from Settings → Providers → Add →
Programs, never a default: one warm `codex app-server` per conversation (`~/.cache/nexa/codex`, run in its empty `work`
folder; found on the PATH, in `~/.local/bin` or `~/.npm-global/bin`), started with its own tools disabled. All three take
Nexa's tools as text (`<tool_call>`), and an empty model is their own default (Codex: the first it lists). **They are
signed in inside Nexa**, by the program's own flow: Settings → Providers → the program → Sign in… opens its login in the
browser and shows the link. The sign-in is the program's own and shared with the terminal one — agy's lives in the
system keyring, opencode's in `auth.json`, Codex's in `CODEX_HOME` — so `opencode auth login`, running `agy` once and
`codex login` still work, and signing out in Nexa signs the terminal out too (so a test copy, whose private XDG
folders do not isolate the keyring or Codex's own folder, must never sign out). For a sign-in that misbehaves,
`QT_LOGGING_RULES="nexa.ai.debug=true"` logs the steps (never a link, a code or an account). OpenCode's hosted API (Console, Go) is a preset with a key, and
Settings → Providers → Add has the catalogue of 179 (a key each, or none for a local server): the menu by category,
**Browse all…** for the searchable gallery. A test copy on private `XDG_CONFIG_HOME` /
`XDG_DATA_HOME` must link `~/.config/opencode` and `~/.local/share/opencode` into them, or opencode sees none of the
user's providers, and needs the wake word off (`voice.wakeWord`), or `--ask` waits for an idle voice forever. A
text-only model refuses images with a 400; the `Agent` then resends without them and remembers the model as blind.

## Layout

- `core/` — no Qt Widgets, no syscalls: `Result.h`, `Logging`, `config/`, `net/` (HttpClient, SseParser),
  `ai/` (OpenAiProvider, AnthropicProvider, GeminiProvider — each maps ChatRequest to its own wire format and
  decodes its own events; SseChatStream is the stream all three share; ToolCallAccumulator; the registry, whose
  `createChain` builds the `ProviderChain`, `createSignIn` the sign-in of a program, and `presets()` is the programs,
  the hand-tuned entries, then the catalogue (187); the programs: `AgyProvider` + `AgySession` + `AgyEvents` (agy's
  stream-json), `OpenCodeProvider` + `OpenCodeServer` + `OpenCodeWire` (opencode's server API), `CodexProvider` +
  `CodexSession` (it also holds `CodexConnection`, one `codex app-server` and its JSON-RPC) + `CodexEvents` +
  `CodexProtocol` (the wire, pure), with `TurnLedger` deciding what a warm session still needs and `TranscriptWriter`
  the text of a turn; `ProgramLookup`; **sign-in**: `SignIn.h` (`ISignIn`, `SignInStatus` — a plan, never an email —
  and `SignInMethod`), `AgySignIn` + `AgyAcp` (`agy acp`), `OpenCodeSignIn` (opencode's `/provider/auth` and OAuth
  routes; an allow-list of the sign-ins a clean opencode offers), `CodexSignIn` (`account/login/*`), and
  `SignInPresenter` (pure: what Settings shows of a sign-in); **the catalogue**: `ProviderCatalog` over the generated
  `ProviderCatalogData.cc` (179 key-based and local providers from OmniRoute 3.8.51, MIT, in
  `THIRD_PARTY_NOTICES.md`; never edit it by hand — `python3 -I scripts/generate-provider-catalog.py`, `--check` to see
  if it is current) and `PresetFilter` (the gallery's search and ranking, pure)),
  `agent/` (Agent loop, Conversation, AppContext = the app Nexa opened last for the user — its kind from the desktop
  entry, a sentence for the model, and the fast path's `run <cmd>` typed into a terminal in front), `tools/` (ToolSpec, ITool, FunctionTool, ToolRegistry, ToolExecutor =
  policy → confirm → invoke → audit, TextToolParser for models without native tools), `intent/` (IntentRouter +
  PC and phone fast-path handlers), `policy/` (PolicyEngine, PanicController), `audit/` (AuditLog), `mcp/`
  (MCP connectors — Model Context Protocol: `McpClient` speaks JSON-RPC 2.0 over an `IMcpTransport`, a server
  process on its standard streams or a URL posted to; `McpConnectors` keeps one client per server in
  `config.mcp.servers` and registers each tool it lists as `mcp_<server>_<tool>`, `privileged`, so confirm-risky
  asks; a built-in server uses an instance of its own with a `Shaping` — its own tool names, flags and risks, and
  filters on the arguments and the result; `tests/fake_mcp_server.py` stands in for a server), `memory/`
  (`MemoryStore` = `~/.local/share/nexa/memory.json`: facts merged when said again, secrets refused by
  `looksSecret`, `promptSection` for the system prompt, `applyEdits` for Settings; `MemoryTools` = `remember`,
  `forget`, `search_memory`, added and removed with `config.memory`), `skills/` (`SkillParser` reads Claude's
  SKILL.md, `SkillCatalog` = Nexa's own skills from the `:/nexa/skills` resource of `nexa_core` plus the user's in
  `~/.local/share/nexa/skills/<name>/`, the user's replacing Nexa's; `SkillTools` = `use_skill`, `save_skill`
  (privileged)); `intent/MemoryIntents` is memory's fast path, right after the stops.
- `system/` — touches the PC: `commands/` (LocalCommandRunner, process groups), `apps/` (DesktopEntry,
  DesktopEntryIndex = IAppCatalog, AppLauncher), `audio/` (wpctl), `media/` (MPRIS), `screen/` (portal
  screenshots), `files/` (FileAccess, confined to home and /tmp), `info/` (SystemInfo); `SystemTools.cc`
  defines every PC tool the model sees; `office/` (`OdfWriter` builds flat OpenDocument XML — Writer from Markdown,
  Impress from slides, Calc from rows with A1 formulas; `OfficeTools` = `create_document`, which writes the file under
  `~/Documents` and opens it in LibreOffice, or has LibreOffice headless convert it to docx/pptx/xlsx/pdf);
  `browser/` (the browser that is part of Nexa: `PlaywrightInstaller` puts `@playwright/mcp` at the pinned
  `kVersion` into `~/.local/share/nexa/playwright` with the user's Node and npm, by itself on the first start;
  `ChromeProfiles` finds the Chrome-family browsers, their profiles by name and where Playwright's extension is;
  `BrowserService` (Facade) plans extension / Nexa's own profile / bundled Chromium, starts the server through a
  `McpConnectors` of its own and shapes its tools — an allow-list, `filename` stripped, results capped — so they are
  `browser_navigate`, `browser_snapshot`, …).
- `ui/` — the look, shared by the desktop and the phone app, so it depends on Qt Widgets alone (nothing of
  `core/`): `NexaCoreSpec` (pure, tested: the nine states — Idle, Wake word, Listening, Transcribing, Thinking,
  Acting, Speaking, Fast path, Offline — their names, wire keys and colour tokens, the timing table, how the
  controller's activity resolves to one state, and where every moving part is at a given moment),
  `NexaCorePainter` (draws a frame: halo, dotted ring, ticks, scan arc, satellites, sonar, comet, the four nodes
  Voice/PC/Tools/Phone, the disc with the N, packets), `NexaCoreWidget` (one widget, three sizes: the presence
  beside the chat, `NexaCoreButton` = the Talk button, `snapshot()` = the tray icon; 60 Hz while something
  happens, 30 Hz idle, stopped while hidden; a state change cross-fades over 350 ms and the fast-path snap stays
  for its 0.9 s ping), `NexaTheme` + `theme.qss` (the tokens for the dark and light schemes; the sheet names which
  widget wears which token and sets widget colours through `qproperty-*`, so no widget hard-codes a colour),
  `NexaThemes` (the catalogue: 26 colour families × dark/light = 52 theme packs generated around a hue with the
  design's own shape of palette and contrast guaranteed, each family wearing one of eight motion styles —
  `CoreStyle`: Classic, Orbit, Pulse, Aurora, Comet, Nebula, Storm, Minimal — which `coreFrame` applies and the sheet
  hands the core as `qproperty-coreStyle`; `config.ui.theme` is `system`, `dark`, `light` or a pack id),
  `NexaDesigns` (the **designs** — a palette is colours, a design is the shape of everything else: a
  `WindowLayout` (Studio, Focus, Console, Sidebar, Orb, Dashboard, Bottom), a `CoreShape` (Ring, Orb, Hex, Bars,
  Iris, Crystal, Arc, Dot — each a `Shape` strategy in `NexaCorePainter` reading the same frames in its own
  geometry), a motion style, the corners, the type face and size, and the transcript's `BubbleStyle`; eight of them
  (studio, focus, console, sidebar, orb, dashboard, gauge, paper), each drawn in a palette of its own;
  `config.ui.design`; the sheet takes `@radius@`, `@radiusSmall@`, `@font@`, `@fontSize@`, `@chatFont@`,
  `@coreShape@`, `@bubbleStyle@`) and `NexaIcons` (line icons painted in the theme's colour).
- `app/` — Qt Widgets GUI (MainWindow: toolbar pills, the core with the state's name beside it, ChatView with
  bubbles and "NEXA fast path · 2 ms" headers, ActionLogDock as cards, PhonePanel = the phone beside the chat while
  one is connected (its screen live, Ctrl+P), the Share screen switch in the input bar (Ctrl+Shift+S), MicButton = the core as the Talk button,
  a monospace status bar, the sun/moon theme toggle; its pieces are built once and `applyDesign()` /
  `rebuildCentral()` arrange them for the design on — header beside or above the chat, a centred column, the
  Actions panel on a side or the bottom or hidden, tiles; SettingsDialog with the design and palette galleries,
  TrayIcon; `ProviderSignInBlock` = the sign-in row of a program's form in Settings → Providers (status line, Sign in… /
  Sign out / Cancel, the login link as selectable text, a code field for a paste-the-code flow; it owns the `ISignIn`
  and waits while a chat turn runs) and `ProviderGalleryDialog` = Add → Browse all… (search, category chips, one row per
  preset); a **Sign in…** button in the status bar while a program provider says nobody is signed in) and the
  AppController mediator,
  which also resolves the core's state from the voice pipeline, running tools, the fast path, the agent and a
  provider out of reach (`coreModeChanged`, `litNodesChanged`), and asks for credentials: a key for an API provider
  (`apiKeyRequired`), a sign-in for a program that says it is not signed in (`signInRequired`, never a key prompt;
  `reloadProviders()` after a sign-in or out); SingleInstance (one copy per user), RemoteBridge
  (the web remote's view of the app); `main.cc` is the composition root and applies the theme first.
- `tests/` — Catch2 v3 (custom `TestMain.cc` with a QCoreApplication), one `<Class>Test.cc` per tested
  class; `FakeHttpServer.h` for provider tests; `AgentTest.cc` uses a scripted fake model; the programs have stand-ins,
  `tests/fake-agy/agy` (print mode and `agy acp`), `tests/fake-opencode/opencode` (`serve`, with the sign-in routes) and
  `tests/fake-codex/codex` (`app-server`) — no test starts the real ones.
- `voice/` — microphone to speech and back: `capture/` (AudioCapture, AudioConverter → 16 kHz mono Int16,
  EnergyVad, MicrophoneCheck = clipped or drowned sound and the steps of "Lower it", RecordedAudioInput for
  `--hear`), `listen/` (what hears speech and the name: `ListeningFront` = the Facade that runs a noise filter, a
  voice detector and the spotter inline or on a worker thread and reports sample positions; `AudioRing` = the raw
  audio requests are cut from; `IVoiceDetector` → EnergyVoiceDetector / SileroVoiceDetector; `IAudioFilter` →
  NoiseReducer), `wake/` (WordPieces = a name in the spotter's SentencePiece pieces, WakePhrases = spellings,
  keyword lines, tuning and the name check, `IWakeSpotter` → KeywordSpotter, SherpaRuntime + the vendored
  `sherpa/c-api.h`), `stt/` (ISpeechRecognizer with word times; CloudRecognizer for the provider, WhisperRecognizer
  for offline, RecognizerChain tries them in order, Transcript cleans the text), `tts/` (ITextToSpeech;
  CloudSynthesizer and PiperSynthesizer produce PCM, SynthesizedSpeech chains them and PcmPlayer plays it,
  SpeechdTtsBackend is the fallback, SentenceChunker splits a streamed reply into speakable sentences, CloudVoices
  = which models hear or speak and their voices), `pipeline/` (VoicePipeline state machine: the spotter's hits, the
  whisper second net, requests, follow-ups; WakeWordDetector = any name, anywhere in a sentence), `VoiceBackends`
  builds what Settings → Voice asks for (one choice of cloud voice for the PC, the phone app and the web remote,
  and the listening front), `VoiceTools` adds `speak`.
- `proto/` — the contract with the root helper: `org.nexa.Helper.xml` (generates the client proxy *and* the
  service adaptor, so they cannot drift), `HelperTypes.h` (bus names, the polkit action id, error names),
  `KeyMap` (characters and key names → Linux key codes; US layout, pure, heavily tested).
- `helper/` — `nexa-helper`, the only part that runs as root: `PolkitAuthorizer` (one check at a time, the
  caller's bus name as the subject), `LoginSessions` (uid from the bus → that user's logind display session),
  `SessionGrants` (which logins have already answered the prompt, in `/run/nexa-helper`), `JobManager` (root
  commands in their own process group, streamed output), `UinputDevice` (three virtual devices, write-only),
  `HelperService` (every privileged method replies late, after polkit). It is started by D-Bus on demand and
  exits after ten idle minutes.
- `system/helper/`, `system/commands/RootCommandRunner`, `system/input/` — the client side:
  `HelperInputBackend` (uinput, reaches every window), `RemoteDesktopSession` + `PortalInputBackend` (the
  desktop's RemoteDesktop portal: no root, GNOME asks once and the restore token is kept in
  `~/.local/share/nexa/remote-desktop-token`), `CombinedInputBackend` (helper types and clicks, portal places
  the pointer — GNOME ignores the helper's absolute device), `XTestInputBackend` (X11 only — on Wayland it
  cannot reach native windows), `InputBackendSelector` (picks one and explains why). Mutter reads portal
  pointer positions in the stream's *pixels*, not the documented logical units (`StreamUnits`).
- `system/android/` — the phone, through adb: `AdbClient` (one adb process per call, timeouts, cancel),
  `AdbDiscovery` (connected phones and mDNS wireless-debugging offers; reconnects the remembered one),
  `MdnsBrowser` + `MdnsPacket` (Nexa's own DNS-SD browser — Debian's adb is built without mDNS, so
  `adb mdns services` never finds anything on this machine; the packet code is pure and tested),
  `AndroidDevice` (which phone, plus every operation), `AndroidTools` (the 18 `android_*` tools), `PhoneMirror` (the
  phone's screen one picture after another for the window's Phone panel, while someone watches), and three pure,
  tested files — `AdbParsers` (adb and shell output), `AdbShell` (command lines; everything from a person or a
  model is single-quoted for the phone's `sh`), `PhoneLookup` (spoken name → app or contact). The phone is
  remembered by hardware serial (`config.android.serial`); `core/intent/PhoneIntents` is its fast path and
  needs the phone named before it acts on a name ("call mom" goes to the model).
- `remote/` — the web remote (Settings → Remote, off by default): `RemoteServer` (QHttpServer: the page from Qt
  resources, `/api/pair`, `/speech/<id>.wav`, and the WebSocket at `/ws`; TLS on `port`, plain HTTP on loopback
  only at `port + 1` for `adb reverse`), `RemoteSession` (one browser: hello with a token within 10 s, rate
  limits), `PairingManager` (six-digit single-use codes → tokens stored as SHA-256 in
  `~/.config/nexa/remote-devices.json`), `RemoteProtocol` (pure JSON messages; a binary frame is one spoken
  utterance), `TlsIdentity` (self-signed cert in `~/.config/nexa/tls`), `QrCode`, and `IRemoteHost` — implemented
  by `app/RemoteBridge`, so remote/ depends only on core. The page itself is `remote/web/` (strict CSP: no inline
  script or style; server text only through `textContent`); it draws the same core as SVG built by `app.js` and
  moved by `style.css` with `NexaCoreSpec`'s timings, and follows the PC's state through the `state` message
  (`mode` = the wire key, `nodes` = the satellites lit), which `welcome` carries too. Phone requests carry the
  source `phone` or `phone-voice`. `RemoteClient` is the phone's end of the same protocol (pair, hello, backoff, trust on first
  use pinned to the certificate's fingerprint); `RemoteProtocol` decodes both directions.
- `android/` — Nexa Phone, the native client: a Qt Widgets app (`PhoneWindow` mediator, `PairPage`, `ChatPage`,
  `PhoneSettings`, `Recorder`, `SpeechPlayer`) over `remote/RemoteClient`, wearing `ui/`'s theme and core (the
  PC's state, except while the phone itself listens, waits for its words or reads a reply aloud; a tap on Talk
  listens until the pause, a hold is push-to-talk). **Pairing it needs no typing**: `nexa-assistant --pair-phone`
  (or Settings → Android → "Pair Nexa Phone over adb") has `app/PhoneLink` start a code, put the adb route up
  (`adb reverse tcp:7777`) and start the app on the connected phone with the address and the code as launch-intent
  extras (`nexa.address`, `nexa.code`, `nexa.name` — read by `android/LaunchIntent`), so it pairs by itself; the
  desktop build takes the same through `--pair <address> --code <code>`. It is a standalone CMake project for
  the APK (`scripts/build-phone-apk.sh`; README → "The phone app"; the APK must carry OpenSSL —
  `scripts/download-android-openssl.sh` fetches KDAB's build into `~/Android/android_openssl`, the CMake refuses to
  configure without it and the build script checks the APK, because Qt for Android has no TLS of its own and an
  APK without it fails every network address with "TLS initialization failed"; on the phone Qt tags log lines by
  category, so the app's own lines and TLS complaints are read with `adb logcat -d -s default:* qt.network.ssl:*`,
  and the app's first line says which OpenSSL it found) and is also built inside the Linux build as
  `nexa_phone_ui` + `./build/android/nexa-phone`, which is how `tests/PhoneWindowTest.cc` drives the real
  screens against the real server. Keep it free of anything Android could not build (no D-Bus, no `app/`).
- `packaging/` — `org.nexa.Assistant.desktop`, the helper's polkit policy, D-Bus and systemd files, and
  `install-helper.sh` / `uninstall-helper.sh`; `scripts/` — model and engine downloads, desktop-entry install.
- Runtime files: `~/.config/nexa/config.json` (settings, 0600), `~/.local/share/nexa/audit.jsonl` (every action,
  0600), `~/.local/share/nexa/models/` (whisper), `~/.local/share/nexa/piper/` (Piper and its voices),
  `~/.local/share/nexa/memory.json` (what Nexa remembers, 0600), `~/.local/share/nexa/skills/` (the user's
  skills), `~/.local/share/nexa/playwright/` (Playwright, its output and its Chromium if any),
  `~/.local/share/nexa/browser-profile/` (Nexa's own Chrome profile, for when the extension is not set up),
  `~/.local/share/nexa/sherpa/` (sherpa-onnx, the wake-name spotter's, Silero's and the noise reducer's models).

## Conventions

- Files PascalCase matching the class, `.h`/`.cc`, `#pragma once`.
- `namespace nexa {` not indented, closed with `} // namespace nexa`; helpers in an anonymous namespace.
- Classes PascalCase, methods/locals camelCase, private members `camelCase_`, brace defaults `int id{0};`.
- 4-space indent, same-line braces, `public:` at column 0.
- Includes: own header, then project headers rooted at the project dir (`"core/ai/IAiProvider.h"`), then Qt/third-party, then std.
- `using namespace Qt::StringLiterals;` in `.cc` files and `u"…"_s` for QString literals; `QStringLiteral` in headers.
- No exceptions: fallible calls return `Result<T>` (`core/Result.h`); async APIs take a `std::function<void(Result<T>)>` callback or emit `failed(nexa::Error)`.
- Smart pointers only; constructor injection, no singletons; wiring happens in `app/main.cc`.
- Name the design pattern in the class comment (Strategy, Factory/Registry, Facade, Builder, Mediator, …).
- Dependencies point downward: `app → remote/voice → core ← system → proto`, and `android → remote/voice`;
  `app → ui` and `android → ui`, where `ui` depends on Qt Widgets alone. `core` never includes QtWidgets.
- Colours come from `ui/theme.qss`: a widget that needs one declares a `Q_PROPERTY(QColor …)` the sheet sets
  (`nexa--ChatView { qproperty-errorColor: … }`), defaulting to the dark tokens. Never colour alone — every state
  of the core also writes its name beside it, and the phone and the web page show the same name.
- Nothing blocks the GUI thread: network via QNetworkAccessManager signals, processes via QProcess signals, heavy work in worker threads.
- QObjects handed out as `shared_ptr` (e.g. `ChatStream`) use a `deleteLater()` deleter, so they can be released from their own signals.
- Never log or display API keys/tokens.
- **Nexa never touches a vendor program's token.** Signing in to agy, opencode or Codex is the program's own OAuth:
  Nexa starts it, shows its link and reads its status (`ISignIn`); the program keeps the token. A status carries a plan,
  never an email; no link, code or account is logged or shown, except the login link itself as one selectable line in
  `ProviderSignInBlock`; `Error::message` of a sign-in is not shown as it is (`SignInPresenter` has a sentence per
  code). An unknown reply is `failed(Parse)`, never "signed out". **No test starts a real vendor program in a way that
  signs in or out** — the fakes in `tests/` stand in, and a probe of a real program only reads. Claude's subscription
  login is not offered (Anthropic's terms forbid a third-party app offering it:
  https://code.claude.com/docs/en/legal-and-compliance): Claude is the Anthropic API-key preset.
- Every action goes through `ToolExecutor::execute` (fast path and agent alike) — never call a tool directly.
- The app the user is in is **what Nexa opened last** (`AppContext`), never read from the desktop: GNOME's
  `org.gnome.Shell.Introspect` is denied to anything but the portals, and the helper is write-only. A command typed
  into that terminal must always be guarded as `AppContext::redirect` does (Nexa's window not focused, typing
  available and not asked about first — a confirmation dialog gives Nexa the keyboard —, not from a phone, not
  destructive, not background, US-keyboard characters only) and shown in the chat as "Into <app>: `cmd`".
  Documents for LibreOffice are made as files with `create_document`, never typed in; a converted file is made
  in a scratch folder and renamed into place, so nothing of the user's is ever written over.
- The model sees the PC screen **only while Share screen is on**: `AppController` withholds `take_screenshot`
  (`ToolRegistry::withhold`) otherwise, so it is in no request and a call to it is refused with the reason; the
  switch is off at every start. The user's own "what's on my screen?" is the one exception (they asked for that
  picture). Never attach a screenshot to a request on Nexa's own initiative. `browser_take_screenshot` is withheld
  with it.
- **Browser work: Playwright first, the screen second.** Anything on the web goes through the `browser_*` tools
  (text snapshots, so models that cannot see manage too), in the user's own Chrome through Playwright's extension —
  Chrome 136+ refuses remote debugging on the default profile, so never try `--remote-debugging-port` on it — and
  in Nexa's own profile until that is set up. The screen (Share screen, `take_screenshot`, the input tools) is only
  for what they cannot reach. The browser tools are an allow-list in `BrowserService`: never register
  `browser_run_code_unsafe` or a page's WebMCP tools, keep `filename` out of every call (it resolves in the user's
  home), and keep the network tools out while signed in as the user. The extension's token travels in the
  server's environment only.
- Memories and skills reach every system prompt: they are labelled data that never outranks the user, the model
  may not save what a page or a tool's output told it to, and `save_skill` is privileged. Tools that a setting
  switches off are **removed** from the registry, not withheld (the fast path and the executor find withheld ones).
- The helper is the only root-privileged code, and it is **write-only** for input: never add a method that
  reads keystrokes, pointer position or window contents. Authorise against what the *bus* says about the
  caller (uid, unique name), never an identity the caller sent — and against the login it resolves to, so the
  password is asked for once per login rather than once per process. Every call re-checks that the login is
  the one at the screen.
- Keep the fast path's arguments matching the tool's schema; a mismatch there fails only at runtime, so
  `tests/IntentRouterTest.cc` asserts the argument shapes.
- Nothing listens to the microphone unless the user asked: `VoicePipeline` opens it for a press of Talk, for the
  follow-up window after a spoken reply (when that setting is on), and continuously while the wake word is on —
  and then **only offline recognition hears the room** (the spotter and the local recogniser,
  `ISpeechRecognizer::isLocal()`). Audio goes to a cloud recogniser only when it is addressed to Nexa: the request
  said with the name, cut from just before the name (never the speech before it), a follow-up in the window, and
  push-to-talk. Nexa changes the system's input volume only when the owner presses Lower it.
- Spoken requests carry the source `"voice"`; `AppController` prefixes them with `[voice]` so the model answers
  in short plain sentences that read well aloud.
- New fast-path phrases go into an `IIntentHandler`; they must not swallow ordinary chat (test negatives too).
  Dictated payloads (text to type, a message) come from `IntentInput::raw`, never `text`: the politeness filter
  would drop a final "now" or "thanks" from them.
- Tool arguments are read with `core/tools/ToolArguments` (forgiving about numbers sent as strings). A
  `privileged` tool also sets `ToolSpec::risk` — the words confirm-risky mode shows when it asks.
- Every source file is listed explicitly in its target's `CMakeLists.txt` (no globbing). Pure logic (parsers, wire-format mappers) gets Catch2 tests.
