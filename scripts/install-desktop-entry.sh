#!/usr/bin/env bash
# Installs Nexa's desktop entry for the current user only (~/.local/share/applications).
#
#   scripts/install-desktop-entry.sh [path-to-nexa-binary]
#   scripts/install-desktop-entry.sh --uninstall
#
# Two reasons it is needed:
#  * global hotkeys — xdg-desktop-portal looks the app id up among the installed entries and
#    refuses one it cannot find ("App info not found"), so Ctrl+Alt+Space would never be granted;
#  * it puts Nexa in the applications grid.
# The id is org.nexa.Assistant, never nexa: that one belongs to Nexa Download Manager.
set -euo pipefail

ID="org.nexa.Assistant"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
TARGET="$APPS/$ID.desktop"

if [[ "${1:-}" == "--uninstall" ]]; then
    rm -f "$TARGET"
    update-desktop-database "$APPS" 2>/dev/null || true
    echo "Removed $TARGET"
    exit 0
fi

BINARY="${1:-$ROOT/build/app/nexa-assistant}"
if [[ ! -x "$BINARY" ]]; then
    echo "No Nexa binary at $BINARY — build it first (cmake --build build) or pass the path." >&2
    exit 1
fi
BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"

mkdir -p "$APPS"
# Quoted, because the project path may contain spaces.
sed "s|^Exec=.*|Exec=\"$BINARY\"|" "$ROOT/packaging/$ID.desktop" > "$TARGET"
chmod 644 "$TARGET"
update-desktop-database "$APPS" 2>/dev/null || true

echo "Installed $TARGET -> $BINARY"
if command -v desktop-file-validate >/dev/null; then desktop-file-validate "$TARGET" && echo "Entry is valid."; fi
