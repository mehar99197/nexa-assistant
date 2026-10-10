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

Debug build (`-Werror`): `cmake --preset debug && cmake --build build-debug`. Sanitizers: `cmake --preset asan && cmake --build build-asan && ./build-asan/tests/nexa_tests`.
Verbose logs: `QT_LOGGING_RULES="nexa.*.debug=true" ./build/app/nexa-assistant` (categories `nexa.app`,
`nexa.config`, `nexa.net`, `nexa.ai`, `nexa.voice`).
Scripted run: `./build/app/nexa-assistant --ask "open calculator" --ask "volume 50"` — submits each request in
turn, exactly as if typed. `--say "<text>"` speaks, `--voice-file <wav>` feeds a recording in as a spoken
request; the three run in that order, one at a time.
The binary is **nexa-assistant** and the desktop id **org.nexa.Assistant** — `nexa`, `/usr/bin/nexa` and
`nexa.desktop` belong to Nexa Download Manager, a different product. `scripts/install-desktop-entry.sh`
installs the entry for the current user; the global hotkeys need it (the portal rejects an app id it cannot
find, and GNOME insists on a reverse-DNS one). GNOME then wants the keys assigned by hand in Settings →
Keyboard; inspect the result with `dconf dump /org/gnome/settings-daemon/global-shortcuts/`.
To inspect the GUI from a script, run it on XWayland (`QT_QPA_PLATFORM=xcb`) and capture with `xwd -id <window>`; do **not** drive it with `xdotool type` — under Wayland, synthetic keystrokes only reach whichever X11 window has focus.

Voice engines (optional, no sudo): `scripts/download-whisper-model.sh base.en` for offline recognition and
`scripts/download-piper.sh` for the offline voice, both into `~/.local/share/nexa`. Without them Nexa uses the
provider's speech models (OmniRoute by default). Local recognition is compiled in only with
`-DNEXA_BUILD_WHISPER=ON` (on in the release and debug presets; whisper.cpp is fetched at configure time).

Build dependencies (one-time, needs sudo — the user runs it):
`sudo apt install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev qt6-speech-dev qt6-tools-dev qt6-websockets-dev qt6-httpserver-dev libpolkit-qt6-1-dev libxkbcommon-dev libqrencode-dev adb catch2`

Runtime: settings in `~/.config/nexa/config.json` (0600, holds API keys). The default provider is
OmniRoute at `http://localhost:20128/v1` (OpenAI-compatible, needs its API key; prefer concrete
models over its slow `auto/*` combos).

## Layout

- `core/` — no Qt Widgets, no syscalls: `Result.h`, `Logging`, `config/`, `net/` (HttpClient, SseParser),
  `ai/` (providers, streams, registry), `agent/` (Agent loop, Conversation), `tools/` (ToolSpec, ITool,
  FunctionTool, ToolRegistry, ToolExecutor = policy → confirm → invoke → audit), `intent/` (IntentRouter +
  PC fast-path handlers), `policy/` (PolicyEngine, PanicController), `audit/` (AuditLog).
- `system/` — touches the PC: `commands/` (LocalCommandRunner, process groups), `apps/` (DesktopEntry,
  DesktopEntryIndex = IAppCatalog, AppLauncher), `audio/` (wpctl), `media/` (MPRIS), `screen/` (portal
  screenshots), `files/` (FileAccess, confined to home and /tmp), `info/` (SystemInfo); `SystemTools.cc`
  defines every PC tool the model sees.
- `app/` — Qt Widgets GUI (MainWindow, ChatView, ActionLogDock, SettingsDialog) and the AppController
  mediator; `main.cc` is the composition root.
- `tests/` — Catch2 v3 (custom `TestMain.cc` with a QCoreApplication), one `<Class>Test.cc` per tested
  class; `FakeHttpServer.h` for provider tests; `AgentTest.cc` uses a scripted fake model.
- `voice/` — microphone to speech and back: `capture/` (AudioCapture, AudioConverter → 16 kHz mono Int16,
  EnergyVad), `stt/` (ISpeechRecognizer; CloudRecognizer for the provider, WhisperRecognizer for offline,
  RecognizerChain tries them in order, Transcript cleans the text), `tts/` (ITextToSpeech; CloudSynthesizer and
  PiperSynthesizer produce PCM, SynthesizedSpeech chains them and PcmPlayer plays it, SpeechdTtsBackend is the
  fallback, SentenceChunker splits a streamed reply into speakable sentences), `pipeline/` (VoicePipeline state
  machine + WakeWordDetector), `VoiceBackends` builds what Settings → Voice asks for, `VoiceTools` adds `speak`.
- `proto/ remote/ helper/` arrive with M3, M5 and M6 (see ARCHITECTURE.md → File tree).
- `packaging/` — `org.nexa.Assistant.desktop`; `scripts/` — model and engine downloads, desktop-entry install.
- Runtime files: `~/.config/nexa/config.json` (settings, 0600), `~/.local/share/nexa/audit.jsonl` (every action,
  0600), `~/.local/share/nexa/models/` (whisper), `~/.local/share/nexa/piper/` (Piper and its voices).

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
- Dependencies point downward: `app → remote/voice → core ← system → proto`. `core` never includes QtWidgets.
- Nothing blocks the GUI thread: network via QNetworkAccessManager signals, processes via QProcess signals, heavy work in worker threads.
- QObjects handed out as `shared_ptr` (e.g. `ChatStream`) use a `deleteLater()` deleter, so they can be released from their own signals.
- Never log or display API keys/tokens.
- Every action goes through `ToolExecutor::execute` (fast path and agent alike) — never call a tool directly.
- Nothing listens to the microphone unless the user asked: `VoicePipeline` opens it for a press of Talk, and
  continuously only while the wake word is on — and then only a **local** recogniser sees the audio
  (`ISpeechRecognizer::isLocal()`), so room sound never reaches a server.
- Spoken requests carry the source `"voice"`; `AppController` prefixes them with `[voice]` so the model answers
  in short plain sentences that read well aloud.
- New fast-path phrases go into an `IIntentHandler`; they must not swallow ordinary chat (test negatives too).
- Every source file is listed explicitly in its target's `CMakeLists.txt` (no globbing). Pure logic (parsers, wire-format mappers) gets Catch2 tests.
