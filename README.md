# Nexa

A Linux desktop AI assistant written in C++20 with a Qt 6 Widgets interface. It connects to any
OpenAI-compatible API (OmniRoute by default), acts on the PC through tools, and listens and talks
back. The roadmap adds root-level system control and an Android bridge. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the design and the milestone status.

Not to be confused with Nexa Download Manager: this is a separate program, named `nexa-assistant`.

## Current state (milestones M0–M4)

- **Acts on the PC**: opens and closes apps, runs shell commands, sets the volume, controls media
  players, reads and writes files (home and /tmp only), reports system info, and looks at the screen.
- **Fast path**: common requests ("open firefox", "volume 30", "pause music", "run df -h",
  "what's on my screen?") run directly in milliseconds, without waiting for a model.
- **Agent**: anything else goes to the model, which calls the same tools step by step.
- **Transparency and control**: every action appears in the Actions panel and in
  `~/.local/share/nexa/audit.jsonl`; Esc stops the current reply, Ctrl+Alt+Esc / Ctrl+Shift+Esc stops
  everything (including the commands it started); Settings → Policy can require confirmation.
- **Talk to it**: hold the Talk button (or press Ctrl+M in the window) and speak; tap instead of hold and it listens until you pause. Replies to spoken requests are read back
  sentence by sentence while they stream in. Esc stops the speech, and talking again interrupts it.
  Optionally it answers to "Hey Nexa" — then the microphone stays on, and only the offline recogniser
  ever hears the room. To talk from any window, give "Talk to Nexa" a key in Settings → Keyboard
  (GNOME lists it there once the desktop entry is installed).
- **Speech, cloud or offline**: by default the active provider's models (OmniRoute), with whisper.cpp
  and Piper as the fallback — so voice keeps working with no network. Either can be chosen outright in
  Settings → Voice.
- **Keyboard, mouse and root** (needs the one-time `sudo ./packaging/install-helper.sh`): types and clicks in
  any window, and runs commands as an administrator after asking for your password once per session. Point at
  anything you can see — coordinates are read straight off the screenshot Nexa last took. A small helper does
  this, started only when needed, and it only ever *sends* input; it cannot read your keyboard or screen.
- Chat with streamed Markdown replies; providers: OmniRoute (default), OpenAI, Ollama, LM Studio, Groq,
  OpenRouter, DeepSeek, Mistral, xAI, Together, or any OpenAI-compatible endpoint.
- `nexa-assistant --ask "open calculator"` sends requests from the command line (also `--say` and
  `--voice-file`).

## Build

```bash
sudo apt install -y cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-tools-dev \
    qt6-multimedia-dev qt6-speech-dev catch2            # + wireplumber (wpctl) for volume
cmake --preset release                                  # fetches whisper.cpp for offline speech
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/nexa-assistant
```

Optional, for voice that works without a network — and recommended, since they are also the fallback
when the provider's speech models are unavailable:

```bash
scripts/download-whisper-model.sh base.en    # offline speech recognition (148 MB)
scripts/download-piper.sh                    # offline voice (26 MB + a 63 MB voice)
scripts/install-desktop-entry.sh             # puts Nexa in the app grid; needed for the global hotkey
sudo ./packaging/install-helper.sh           # typing, clicking and root commands
```

The helper installs five files and nothing else; `sudo ./packaging/uninstall-helper.sh` removes them all.

On first start Nexa asks for the OmniRoute API key. Settings are stored in
`~/.config/nexa/config.json`, readable only by you.
