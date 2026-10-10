#!/usr/bin/env bash
# Installs the Piper text-to-speech engine (rhasspy/piper 2023.11.14-2, x86_64) and one voice into
# ~/.local/share/nexa/piper. Nexa uses Piper as its local voice (and as the fallback when the cloud
# voice is unreachable).
#
#   scripts/download-piper.sh [voice]      # default: en_US-lessac-medium (63 MB)
#
# Voices: https://huggingface.co/rhasspy/piper-voices (names look like en_GB-alan-medium).
set -euo pipefail

VOICE="${1:-en_US-lessac-medium}"
RELEASE="2023.11.14-2"
ARCHIVE="piper_linux_x86_64.tar.gz"
ARCHIVE_SHA256="a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992"

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "This script installs the x86_64 build of Piper; this machine is $(uname -m)." >&2
    exit 1
fi
if [[ ! "$VOICE" =~ ^([a-z]{2,3})_([A-Z]{2})-([a-z0-9_]+)-(x_low|low|medium|high)$ ]]; then
    echo "Voice names look like en_US-lessac-medium (see https://huggingface.co/rhasspy/piper-voices)." >&2
    exit 1
fi
FAMILY="${BASH_REMATCH[1]}"
LOCALE="${BASH_REMATCH[1]}_${BASH_REMATCH[2]}"
SPEAKER="${BASH_REMATCH[3]}"
QUALITY="${BASH_REMATCH[4]}"

DEST="${XDG_DATA_HOME:-$HOME/.local/share}/nexa/piper"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/nexa/downloads"
mkdir -p "$DEST/voices" "$CACHE"

# Downloads $1 to $2 unless it is already there, then checks the SHA-256 $3.
fetch() {
    local url="$1" target="$2" sha="$3"
    if [[ ! -f "$target" ]]; then
        echo "Downloading $(basename "$target") …"
        curl -fL --retry 3 --progress-bar -o "$target.part" "$url"
        mv "$target.part" "$target"
    fi
    if ! echo "$sha  $target" | sha256sum --check --quiet -; then
        rm -f "$target"
        echo "Checksum mismatch for $(basename "$target"); the download was removed, try again." >&2
        exit 1
    fi
}

if [[ ! -x "$DEST/piper" ]]; then
    fetch "https://github.com/rhasspy/piper/releases/download/$RELEASE/$ARCHIVE" "$CACHE/$ARCHIVE" "$ARCHIVE_SHA256"
    WORK="$(mktemp -d)"
    trap 'rm -rf "$WORK"' EXIT
    tar -xzf "$CACHE/$ARCHIVE" -C "$WORK"
    cp -a "$WORK/piper/." "$DEST/"
    echo "Installed Piper into $DEST"
fi

MODEL="$DEST/voices/$VOICE.onnx"
if [[ ! -f "$MODEL" ]]; then
    BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/$FAMILY/$LOCALE/$SPEAKER/$QUALITY/$VOICE"
    # Hugging Face sends the SHA-256 of files kept in Git LFS as X-Linked-ETag.
    SHA="$(curl -fsSI "$BASE.onnx" | tr -d '\r' | awk -F'"' 'tolower($1) ~ /^x-linked-etag: /{print $2}')"
    if [[ ! "$SHA" =~ ^[0-9a-f]{64}$ ]]; then
        echo "Voice $VOICE not found (or offline)." >&2
        exit 1
    fi
    fetch "$BASE.onnx" "$CACHE/$VOICE.onnx" "$SHA"
    curl -fsSL --retry 3 -o "$MODEL.json" "$BASE.onnx.json"
    cp "$CACHE/$VOICE.onnx" "$MODEL"
    echo "Installed voice $VOICE"
fi

echo "Try it: echo 'Hello from Nexa.' | \"$DEST/piper\" --model \"$MODEL\" --output_file /tmp/piper-test.wav"
