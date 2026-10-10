#!/usr/bin/env bash
# Downloads a whisper.cpp speech-recognition model into ~/.local/share/nexa/models and checks it
# against the SHA-256 that Hugging Face publishes for the file.
#
#   scripts/download-whisper-model.sh [model]     # default: base.en (148 MB)
#
# English-only models (*.en) are faster and more accurate for English; tiny.en is the quickest,
# small.en the most accurate that still runs comfortably on a laptop CPU.
set -euo pipefail

MODEL="${1:-base.en}"
if [[ ! "$MODEL" =~ ^(tiny|base|small|medium|large-v[123](-turbo)?)(\.en)?(-q[0-9]_[0-9])?$ ]]; then
    echo "Unknown model '$MODEL'. Examples: tiny.en, base.en, small.en, base.en-q5_1, large-v3-turbo" >&2
    exit 1
fi

DEST="${XDG_DATA_HOME:-$HOME/.local/share}/nexa/models"
TARGET="$DEST/ggml-$MODEL.bin"
URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"

if [[ -f "$TARGET" ]]; then
    echo "Already installed: $TARGET"
    exit 0
fi

# For files stored in Git LFS, Hugging Face sends the SHA-256 as X-Linked-ETag.
EXPECTED="$(curl -fsSI "$URL" | tr -d '\r' | awk -F'"' 'tolower($1) ~ /^x-linked-etag: /{print $2}')"
if [[ ! "$EXPECTED" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Could not get the checksum of $URL (offline, or the model does not exist)." >&2
    exit 1
fi

mkdir -p "$DEST"
PART="$TARGET.part"
trap 'rm -f "$PART"' EXIT
echo "Downloading ggml-$MODEL.bin …"
curl -fL --retry 3 --progress-bar -o "$PART" "$URL"
echo "$EXPECTED  $PART" | sha256sum --check --quiet -
mv "$PART" "$TARGET"
trap - EXIT
echo "Installed $TARGET"
