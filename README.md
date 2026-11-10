# Nexa

A Linux desktop AI assistant written in C++20 with a Qt 6 Widgets interface. It talks to any AI
provider — your own opencode and agy (Google's Antigravity CLI) on their own sign-ins by default, Codex on your
ChatGPT account, a catalogue of 179 key-based and local providers, or OpenAI-compatible APIs, Anthropic and Google
Gemini natively —
acts on the PC through tools, listens and talks back, and works with your Android phone in both
directions. See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and the milestone status.

Not to be confused with Nexa Download Manager: this is a separate program, named `nexa-assistant`
(desktop id `org.nexa.Assistant`).

## What it does

- **Acts on the PC** — opens and closes apps, runs shell commands (and, with the helper, root
  commands), sets the volume, controls media players, reads and writes files (home and /tmp only),
  reports system info, and looks at the screen — but only while you let it.
- **Knows where you are** — say "open terminal" and the commands you ask for next are typed into that
  terminal, where you see them run (unless Nexa's own window has the keyboard, the policy would ask
  first, you asked from your phone, or the command is destructive — then it runs through the usual
  policy, as before). Say "open libreoffice" and "make a presentation
  about solar energy" becomes an Impress file with real slides, "a budget spreadsheet" a Calc sheet with
  working formulas, "a letter to…" a Writer document — created as files and opened, not typed. The status
  bar shows which app Nexa thinks you are in; opening another, closing it, or half an hour of silence moves on.
- **Looks only when you share** — the Share screen button beside the input box (Ctrl+Shift+S, or say
  "share my screen") is off at every start; until you switch it on, the model is not even offered the
  screenshot tool and works through commands and tools alone. Asking "what's on my screen?" yourself
  takes one picture either way. A model that cannot see images gets the words without the picture and
  Nexa says so, instead of every later request failing.
- **Fast path** — common requests ("open firefox", "volume 30", "pause music", "run df -h", "what's on
  my screen?", "on my phone open youtube") run directly in milliseconds, without waiting for a model.
- **Agent** — anything else goes to the model, which calls the same tools step by step.
- **Mission Control: many tasks at once** — every request is a task with an agent of its own, and several run
  at the same time (3 by default, up to 8 in Settings → Mission; the rest wait in a queue). Add a list at once
  with **+ Add tasks** (Ctrl+T, one per line). Each task gets a kind of agent from its words — web, code, screen,
  phone or general — and the model that kind is given: Auto sends code to Codex, opencode or agy and the screen
  and the phone to a provider that can see, or choose one per kind, or for a batch. A lead agent can hand part of
  its task to another kind (`delegate_task`), which works on that kind's model and answers back. The control room
  shows the agents at work, the board (pause, resume, stop, clear; the queue), the live activity by lane, and a
  card for every question the policy asks, with Approve and Deny — no task waits behind a dialog, and stopping
  one stops nothing else. The latency, tokens per second, CPU and memory are measured live. A conversation still
  carries on: the next request sees the last one, on the same warm program.
- **Hardcore mode** — switch on **Hardcore** beside Share screen (Ctrl+Shift+H, or say "hardcore on") and every
  task you start locks this laptop's own keyboard and touchpad until it is done, so nothing typed or clicked here
  spoils what it types and clicks; the screen stays on and you watch. A banner says until when. Its questions are
  answered by voice ("approve" / "deny") or on your phone. While it works, only **Nexa Phone, the web remote or your
  own enrolled voice** can end it early (**Unlock PC**, or "Nexa, unlock" / "stop everything" said by you — Settings →
  Voice → My voice); anyone else's voice is refused, and hardcore mode switches on only when one of those ways exists.
  Nexa closing or hanging always gives the keyboard and mouse back. It needs the root helper (reinstall it once after
  updating Nexa).
- **Types and clicks for you** — in any window, including native Wayland ones. Point at anything you
  can see: coordinates are read straight off the screenshot Nexa last took, and land on the exact
  pixel even with fractional scaling.
- **Talk to it** — hold the Talk button (Ctrl+M in the window, or a global key you assign) and speak.
  Replies to spoken requests are read back sentence by sentence as they stream in, and for a few
  seconds after one it listens for a follow-up without its name. Optionally it answers to a name you
  choose ("Hey Nexa" by default, Settings → Voice) — heard the moment it is said, even mid-sentence or
  over a fan, by an offline keyword spotter, with whisper.cpp as a second net; only offline recognition
  hears the room, and only the request after the name goes to the cloud recogniser. Recognition and the
  voice come from the provider you pick in Settings → Voice (Groq's whisper, for example), with
  whisper.cpp and Piper as the offline fallback. Nexa says when the microphone is overloaded and can
  lower its input volume for you.
- **Listens to you, not the room** — enrol your voice once (Settings → Voice → My voice: read three sentences)
  and Nexa answers hands-free to your voice alone: a TV, someone else or its own speakers saying "Nexa" are
  ignored, and a stranger's sentence is not even transcribed. Pressing Talk always works. A wake word the words
  do not confirm asks nothing, and "next", "Max" or "Rex" are no longer taken for "Nexa". By default only its
  name interrupts Nexa while it speaks, so it never interrupts itself.
- **Voice control** — "stop talking" stops the voice and leaves the work going; "stop" stops the task you are
  looking at, "stop everything" every task. "Pause task two", "resume the code agent", "approve" / "deny" (when a
  task waits for you), "what are you doing?", "add a task: …". With **Keep the conversation going** on, Nexa
  keeps listening to you after each reply until you say "that's all", and tasks on the board say aloud when they
  are done or need you.
- **Controls your phone** (over USB or Wi-Fi, no app on the phone) — opens apps, taps, swipes and
  types, takes screenshots and describes them, reads notifications and text messages, starts calls
  and messages, controls music and volume, and copies files both ways. While a phone is connected, a
  Phone panel beside the chat (Ctrl+P) shows which one it is and its screen, live.
- **Is controlled from your phone** — pair the phone's browser once, then type or hold-to-talk on the
  phone; the PC acts, and the phone shows the conversation and plays the spoken reply. There is also
  a small app for the phone (`android/`), which does the same natively and remembers the pairing.
- **Shows its work** — every action appears in the Actions panel and in
  `~/.local/share/nexa/audit.jsonl`. Esc stops the reply; Ctrl+Alt+Esc (or Ctrl+Shift+Esc) stops
  everything, including commands it started. Settings → Policy can ask before risky actions (root
  commands, typing, calls and messages) or before every change.
- **Stays out of the way** — one copy per user (starting it again brings the window forward), an icon
  in the top bar, and closing the window keeps the hotkeys and web remote running.

Providers: **opencode**, **agy** (Google's Antigravity CLI) and **Codex** (OpenAI's coding agent, on your ChatGPT
plan; add it under Settings → Providers → Add → Programs) — the models you already have there, on their own sign-ins
and quotas, with no key in Nexa: Nexa runs its own `opencode serve`, a warm `agy` process and a warm `codex app-server`,
and every action still goes through Nexa's own tools (theirs stay switched off).

**Sign in from inside Nexa.** Settings → Providers → pick the program → **Sign in…** opens that program's own login in
your browser (the link is shown too, to copy) and reads its status; the program keeps the token, Nexa never sees it.
A program that says nobody is signed in gets a **Sign in…** button in the status bar that opens the same place. The
sign-in is shared with the program in your terminal, so signing out in Nexa signs it out there too (`opencode auth
login`, running `agy` once and `codex login` still work). Claude's subscription login is not offered — Anthropic's terms
do not let a third-party app offer it — so Claude goes through an Anthropic API key.

**A catalogue of 179 providers.** Settings → Providers → **Add** lists them by category (gateways, model makers, inference hosts,
regional and cloud providers, local servers, ones that need no key) and **Browse all…** opens a searchable gallery (a
name, a host, "free" or "nokey"). Each takes a key from its own site, except local servers and the few that need none.
OpenAI, Anthropic, Google Gemini, Groq, OpenRouter, DeepSeek, Mistral, xAI, Together, Ollama, LM Studio, OpenCode's
hosted API (Console and Go) and a Custom OpenAI-compatible endpoint are all in the menu; the catalogue is generated from
OmniRoute's open-source registry (`THIRD_PARTY_NOTICES.md`).

Settings → Providers sets the **first choice** and the order of the rest (▲ ▼): when the first choice cannot answer —
not running, not signed in, out of quota — the next one that allows it answers, with its own model, and the reply
says `via agy`. For a model that cannot call tools (many small local ones), tick "This model cannot call tools" and
Nexa describes the tools in the prompt instead.

## Build

```bash
sudo apt install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-tools-dev \
    qt6-multimedia-dev qt6-speech-dev qt6-websockets-dev qt6-httpserver-dev \
    libpolkit-qt6-1-dev libxkbcommon-dev libqrencode-dev catch2 adb openssl
cmake --preset release                  # fetches whisper.cpp for offline speech recognition
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/nexa-assistant
```

`cmake --preset debug` builds with warnings as errors; `cmake --preset asan` with AddressSanitizer
and UndefinedBehaviorSanitizer.

## Set-up, once

```bash
scripts/install-desktop-entry.sh             # app grid entry and icon; needed for the global hotkeys
sudo ./packaging/install-helper.sh           # typing, clicking and root commands (asks for your password once per login)
scripts/download-whisper-model.sh base.en    # offline speech recognition (148 MB), optional
scripts/download-piper.sh                    # offline voice (26 MB + a 63 MB voice), optional
scripts/download-sherpa.sh                   # wake-name spotter, voice detection, noise reduction (27 MB), optional
```

On first start Nexa offers opencode and agy if they are installed, and installs Playwright — the browser automation that is
part of it — into `~/.local/share/nexa/playwright` by itself (it needs Node.js 18 or newer and npm:
`sudo apt install nodejs npm` if they are missing). Then:

- **Your own browser** — Settings → Browser: pick the Chrome profile Nexa should work in and press
  "Install in Google Chrome…"; add the Playwright extension on the store page that opens. The page notices
  it by itself. Optionally paste the token the extension shows, so it connects without asking. Until then Nexa
  works in a separate Chrome window with its own profile.
- **Global hotkeys** — GNOME Settings → Keyboard → Keyboard Shortcuts → Nexa Assistant: give "Talk to
  Nexa" and "Stop everything" a key.
- **Your phone, controlled from the PC** — on the phone: Settings → About phone → tap Build number seven
  times; then Developer options → Wireless debugging → "Pair device with pairing code". In Nexa:
  Settings → Android, pick the phone, enter the code, Pair, then Connect. Press "Remember this phone"
  and Nexa reconnects to it by itself whenever wireless debugging is on (Nexa finds the phone on the
  Wi-Fi itself — the port changes every time — so the phone and the PC must be on the same network).
  Over USB, turn on USB debugging and tap Allow — nothing else.
- **Nexa, controlled from your phone** — Settings → Remote: tick the box, press OK, then "Pair a phone"
  and scan the QR code with the phone's camera. The first visit warns about the certificate, because
  Nexa made its own: compare the fingerprint shown in Settings with the one the browser shows, then
  continue. A phone connected with adb can open `http://localhost:7777` instead, with no warning.

`sudo ./packaging/uninstall-helper.sh` removes everything the helper install added;
`scripts/install-desktop-entry.sh --uninstall` removes the entry and icon.

## The phone app

`android/` is Nexa's own app for the phone: the same conversation as the browser page, native, with
a hold-to-talk button, and it keeps the pairing. With the phone connected through adb (Settings →
Android), `nexa-assistant --pair-phone` — or Settings → Android → "Pair Nexa Phone over adb" — pairs
the app without typing anything: Nexa starts a code, routes the phone's `localhost:7777` to itself and
starts the app with the address and the code. Otherwise, on the first screen type the address shown under
the QR code (or paste the whole link) and the six-digit code. Over the network the app shows the
fingerprint of Nexa's certificate once — compare it with the one in Settings → Remote and trust it;
after that only that certificate is accepted. Over USB with adb, `localhost:7777` works with no
certificate at all. "Forget this PC" in the ⋮ menu unpairs.

The same app builds for the desktop as part of the normal build (`./build/android/nexa-phone`), which
is how its screens are tested. The APK needs the Qt for Android kit and the Android SDK, neither of
which comes from apt:

```bash
pipx install aqtinstall
aqt install-qt all_os android 6.10.2 android_arm64_v8a -m qtwebsockets qtmultimedia \
    --archives qtbase qtsvg qtwebsockets qtmultimedia -O ~/Qt
# Android SDK: command-line tools from developer.android.com into ~/Android/Sdk/cmdline-tools/latest, then
sdkmanager "platform-tools" "platforms;android-36" "build-tools;36.0.0" "ndk;27.2.12479018"
sudo apt install -y openjdk-21-jdk-headless   # a JDK with javac (the JRE is not enough); or a Temurin JDK in ~/Android/jdk-21
scripts/download-android-openssl.sh           # OpenSSL for the APK (5 MB): Qt for Android has no TLS without it
scripts/build-phone-apk.sh                    # configures with the kit, builds, signs, prints the APK's path
adb install -r build-android/nexa-phone.apk
```

The host tools come from the system's own Qt (it is the same version, and Debian ships
`androiddeployqt`), so no desktop kit is downloaded. The first build fetches Gradle and the Android
plugin (about 600 MB). The APK is signed with the usual Android debug key unless `KEYSTORE`,
`KEYSTORE_ALIAS` and `KEYSTORE_PASS` name your own. It carries OpenSSL 3 (KDAB's build for Android,
fetched at a fixed commit into `~/Android/android_openssl`), because Qt for Android ships only its TLS
plugin: an APK without it answers every `https://` address with "TLS initialization failed" and can
reach Nexa only over adb, so the build refuses to go on without it.

## Security, in brief

- The root helper is the only privileged part. It runs only when needed, exits when idle, can only
  *send* keyboard and mouse input (never read it) — and, for hardcore mode, switch the laptop's own
  keyboards and pointers off and on, giving them back whenever Nexa or the helper itself stops — and
  authorises against what the system bus says about the caller. Your password goes into the desktop's own prompt, never into Nexa; what Nexa
  remembers is only that you said yes, until you log out (Settings → Helper → Forget my password ends
  it sooner).
- The web remote is off until you switch it on. A phone gets in only with a six-digit code shown on
  the PC — single use, five minutes, withdrawn after ten wrong guesses — traded for a token stored
  only as a hash. It speaks HTTPS on the network (plain HTTP only on loopback, for adb), refuses other
  sites' pages, rate-limits everything, and a revoked phone is disconnected at once.
- API keys live in `~/.config/nexa/config.json` (readable only by you) and are never logged or shown. Signing in to
  opencode, agy or Codex is each program's own OAuth: Nexa starts it and reads its status, and the token stays with the
  program — Nexa never reads, stores or shows one.

## The browser, memory and skills

**Browser.** Anything on the web — "open github.com/trending and tell me the top repository", "search for
flights to Lahore", "fill in this form" — goes to Playwright first: it reads pages as text, so it works even
with models that cannot see the screen, in your own Chrome (signed in as you) once its extension is in the
profile you chose. Screen sharing is the second approach, only for what Playwright cannot reach. The status
bar says where it works (`Browser: Chrome · Ahmad`, `Browser: Nexa profile`). Firefox cannot be driven this
way. In confirm-risky mode Nexa asks before it clicks or types on a page.

**Memory.** "Remember that my office is in Multan", "forget that …", "what do you know about me?" — and,
unless you switch it off, Nexa also notes lasting facts it learns. Passwords, PINs, keys and card numbers are
never kept. Settings → Memory shows every fact to correct or delete.

**Skills.** Instructions for particular jobs, in the same SKILL.md format as Claude's skills. Nexa ships one
(`web-browsing`); ask it to "make a skill for my morning routine: …" and it writes one, or add, import and
edit them in Settings → Skills. The model sees each skill's name and description and reads the rest when a
request fits.

## Where things are

| | |
|---|---|
| `~/.config/nexa/config.json` | settings and API keys (0600) |
| `~/.config/nexa/remote-devices.json` | phones paired with the web remote — token hashes only (0600) |
| `~/.config/nexa/tls/` | the web remote's own certificate (key 0600) |
| `~/.local/share/nexa/audit.jsonl` | every action Nexa took (0600) |
| `~/.local/share/nexa/models/`, `piper/` | offline speech engines |
| `~/.local/share/nexa/memory.json` | what Nexa remembers about you (0600) |
| `~/.local/share/nexa/skills/` | your skills, one folder each |
| `~/.local/share/nexa/playwright/` | Playwright, installed by Nexa |
| `~/.local/share/nexa/browser-profile/` | Nexa's own Chrome profile, used until the extension is set up |
| `~/.local/share/nexa/remote-desktop-token` | lets GNOME skip asking again before Nexa moves the pointer (0600) |
| `/run/nexa-helper/` | that this login has authorised the helper (root, 0700; gone at reboot) |

## Command line

```bash
nexa-assistant --ask "open calculator" --ask "volume 50"   # requests, in order, as if typed
nexa-assistant --say "hello" --voice-file request.wav      # speak; feed a recording in as a spoken request
nexa-assistant --new-instance --hear room.wav              # play a recording into the microphone, wake word on
nexa-assistant --pair-phone                                 # pair the Nexa Phone app on the adb-connected phone, no typing
nexa-assistant --hardcore --ask "tidy my downloads folder"   # hardcore mode on without asking: the task locks the laptop
```

If Nexa is already running, these go to the running copy. `--new-instance` starts a separate one
(for testing). `--screenshot <file.png>` saves a picture of the window once the requests are done,
then quits (it works with `QT_QPA_PLATFORM=offscreen`); `--screenshot-settings <dir>` does the same
for every Settings tab; `--theme dark|light|system|<palette>` and `--design <design>` pick the look for
that run only (a palette is one of the 52 in Settings → General, such as `ocean-dark`; a design is one of
the eight there — `studio`, `focus`, `console`, `sidebar`, `orb`, `dashboard`, `gauge`, `paper` — each its
own arrangement, core shape, corners and type), while the galleries there and the sun/moon button in the
toolbar set them for good.

## Connectors (MCP)

Settings → Connectors adds MCP servers (Model Context Protocol): a program Nexa starts and talks to on
its standard streams (`npx -y @modelcontextprotocol/server-filesystem /home/me`, `uvx mcp-server-git`,
…) or a server at a URL with the headers it wants. Their tools join Nexa's as `mcp_<name>_<tool>` and
the model calls them like any other, through the policy, the Actions panel and the audit log; in
confirm-risky mode Nexa asks before each call. Test lists a server's tools before you press OK.

No licence has been chosen yet. Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
