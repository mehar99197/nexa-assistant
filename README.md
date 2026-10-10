# Nexa

A Linux desktop AI assistant written in C++20 with a Qt 6 Widgets interface. It talks to any AI
provider — OmniRoute by default, or OpenAI-compatible APIs, Anthropic and Google Gemini natively —
acts on the PC through tools, listens and talks back, and works with your Android phone in both
directions. See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and the milestone status.

Not to be confused with Nexa Download Manager: this is a separate program, named `nexa-assistant`
(desktop id `org.nexa.Assistant`).

## What it does

- **Acts on the PC** — opens and closes apps, runs shell commands (and, with the helper, root
  commands), sets the volume, controls media players, reads and writes files (home and /tmp only),
  reports system info, and looks at the screen.
- **Fast path** — common requests ("open firefox", "volume 30", "pause music", "run df -h", "what's on
  my screen?", "on my phone open youtube") run directly in milliseconds, without waiting for a model.
- **Agent** — anything else goes to the model, which calls the same tools step by step.
- **Types and clicks for you** — in any window, including native Wayland ones. Point at anything you
  can see: coordinates are read straight off the screenshot Nexa last took, and land on the exact
  pixel even with fractional scaling.
- **Talk to it** — hold the Talk button (Ctrl+M in the window, or a global key you assign) and speak.
  Replies to spoken requests are read back sentence by sentence as they stream in. Optionally it
  answers to "Hey Nexa", and then only the offline recogniser ever hears the room. Speech uses the
  provider's models by default, with whisper.cpp and Piper as an offline fallback.
- **Controls your phone** (over USB or Wi-Fi, no app on the phone) — opens apps, taps, swipes and
  types, takes screenshots and describes them, reads notifications and text messages, starts calls
  and messages, controls music and volume, and copies files both ways.
- **Is controlled from your phone** — pair the phone's browser once, then type or hold-to-talk on the
  phone; the PC acts, and the phone shows the conversation and plays the spoken reply. There is also
  a small app for the phone (`android/`), which does the same natively and remembers the pairing.
- **Shows its work** — every action appears in the Actions panel and in
  `~/.local/share/nexa/audit.jsonl`. Esc stops the reply; Ctrl+Alt+Esc (or Ctrl+Shift+Esc) stops
  everything, including commands it started. Settings → Policy can ask before risky actions (root
  commands, typing, calls and messages) or before every change.
- **Stays out of the way** — one copy per user (starting it again brings the window forward), an icon
  in the top bar, and closing the window keeps the hotkeys and web remote running.

Providers: OmniRoute (default), OpenAI, Anthropic, Google Gemini, Ollama, LM Studio, Groq, OpenRouter,
DeepSeek, Mistral, xAI, Together, or any OpenAI-compatible endpoint. For a model that cannot call tools
(many small local ones), tick "This model cannot call tools" in Settings → Providers and Nexa describes
the tools in the prompt instead.

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
```

On first start Nexa asks for the OmniRoute API key. Then:

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
a hold-to-talk button, and it keeps the pairing. On the first screen type the address shown under
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
sdkmanager "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018"
sudo apt install -y openjdk-21-jdk-headless   # a JDK with javac (the JRE is not enough); or a Temurin JDK in ~/Android/jdk-21
scripts/build-phone-apk.sh                    # configures with the kit, builds, signs, prints the APK's path
adb install -r build-android/nexa-phone.apk
```

The host tools come from the system's own Qt (it is the same version, and Debian ships
`androiddeployqt`), so no desktop kit is downloaded. The first build fetches Gradle and the Android
plugin (about 600 MB). The APK is signed with the usual Android debug key unless `KEYSTORE`,
`KEYSTORE_ALIAS` and `KEYSTORE_PASS` name your own.

## Security, in brief

- The root helper is the only privileged part. It runs only when needed, exits when idle, can only
  *send* keyboard and mouse input (never read it), and authorises against what the system bus says
  about the caller. Your password goes into the desktop's own prompt, never into Nexa; what Nexa
  remembers is only that you said yes, until you log out (Settings → Helper → Forget my password ends
  it sooner).
- The web remote is off until you switch it on. A phone gets in only with a six-digit code shown on
  the PC — single use, five minutes, withdrawn after ten wrong guesses — traded for a token stored
  only as a hash. It speaks HTTPS on the network (plain HTTP only on loopback, for adb), refuses other
  sites' pages, rate-limits everything, and a revoked phone is disconnected at once.
- API keys live in `~/.config/nexa/config.json` (readable only by you) and are never logged or shown.

## Where things are

| | |
|---|---|
| `~/.config/nexa/config.json` | settings and API keys (0600) |
| `~/.config/nexa/remote-devices.json` | phones paired with the web remote — token hashes only (0600) |
| `~/.config/nexa/tls/` | the web remote's own certificate (key 0600) |
| `~/.local/share/nexa/audit.jsonl` | every action Nexa took (0600) |
| `~/.local/share/nexa/models/`, `piper/` | offline speech engines |
| `~/.local/share/nexa/remote-desktop-token` | lets GNOME skip asking again before Nexa moves the pointer (0600) |
| `/run/nexa-helper/` | that this login has authorised the helper (root, 0700; gone at reboot) |

## Command line

```bash
nexa-assistant --ask "open calculator" --ask "volume 50"   # requests, in order, as if typed
nexa-assistant --say "hello" --voice-file request.wav      # speak; feed a recording in as a spoken request
```

If Nexa is already running, these go to the running copy. `--new-instance` starts a separate one
(for testing).

No licence has been chosen yet.
