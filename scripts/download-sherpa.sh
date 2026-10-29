#!/usr/bin/env bash
# Installs sherpa-onnx (k2-fsa, v2.0.1, x86_64) and the models Nexa uses with it into ~/.local/share/nexa/sherpa:
# the keyword spotter that hears the wake name (gigaspeech 3.3M, int8), the Silero voice detector, and the GTCRN
# noise reducer. Nexa loads the library at run time; without it, the wake name is found by whisper alone and
# speech is told by loudness. No sudo.
#
#   scripts/download-sherpa.sh              # install (about 27 MB of downloads)
#   scripts/download-sherpa.sh --uninstall  # remove ~/.local/share/nexa/sherpa
set -euo pipefail

DEST="${XDG_DATA_HOME:-$HOME/.local/share}/nexa/sherpa"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/nexa/downloads"

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -rf "$DEST"
    echo "Removed $DEST"
    exit 0
fi
if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "This script installs the x86_64 build of sherpa-onnx; this machine is $(uname -m)." >&2
    exit 1
fi

RELEASES="https://github.com/k2-fsa/sherpa-onnx/releases/download"
LIB_ARCHIVE="sherpa-onnx-v2.0.1-linux-x64-shared-lib.tar.bz2"
LIB_SHA256="532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120"
KWS_NAME="sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01"
KWS_ARCHIVE="$KWS_NAME.tar.bz2"
KWS_SHA256="f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a"
SILERO_SHA256="9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6"
GTCRN_SHA256="e77603ac0c23dac3227dd2d7135b3a585cbee2679048aecfa886657d3ae1b534"
KWS_FILES=(encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx
           joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx tokens.txt bpe.model)

mkdir -p "$DEST/lib" "$DEST/kws" "$CACHE"

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

# Archives are unpacked into an empty folder of their own; only the files Nexa needs are copied out.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fetch "$RELEASES/v2.0.1/$LIB_ARCHIVE" "$CACHE/$LIB_ARCHIVE" "$LIB_SHA256"
mkdir -p "$WORK/lib"
tar -xjf "$CACHE/$LIB_ARCHIVE" -C "$WORK/lib"
for file in libsherpa-onnx-c-api.so libonnxruntime.so; do
    install -m 0644 "$WORK/lib/sherpa-onnx-v2.0.1-linux-x64-shared-lib/lib/$file" "$DEST/lib/$file"
done

fetch "$RELEASES/kws-models/$KWS_ARCHIVE" "$CACHE/$KWS_ARCHIVE" "$KWS_SHA256"
mkdir -p "$WORK/kws"
tar -xjf "$CACHE/$KWS_ARCHIVE" -C "$WORK/kws"
for file in "${KWS_FILES[@]}"; do
    install -m 0644 "$WORK/kws/$KWS_NAME/$file" "$DEST/kws/$file"
done

fetch "$RELEASES/asr-models/silero_vad.onnx" "$CACHE/silero_vad.onnx" "$SILERO_SHA256"
install -m 0644 "$CACHE/silero_vad.onnx" "$DEST/silero_vad.onnx"
fetch "$RELEASES/speech-enhancement-models/gtcrn_simple.onnx" "$CACHE/gtcrn_simple.onnx" "$GTCRN_SHA256"
install -m 0644 "$CACHE/gtcrn_simple.onnx" "$DEST/gtcrn_simple.onnx"

echo "Installed sherpa-onnx 2.0.1 and its models into $DEST"
echo "Restart Nexa (or close Settings with OK) to use instant name spotting, Silero and noise reduction."
