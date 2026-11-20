#!/usr/bin/env bash
# Downloads the two tunnel programs Nexa can use away from home — cloudflared (Cloudflare Tunnel) and ngrok — into
# ~/.local/share/nexa/tunnels, where Settings → Remote → Away from home finds them. No sudo, nothing installed system
# wide; each download is checked against a pinned SHA-256 before it is kept.
#
#   scripts/download-tunnels.sh            both
#   scripts/download-tunnels.sh ngrok      one of them
#
# ngrok publishes only a "stable" link, whose file changes with every release: when it no longer matches the pin, the
# script stops and prints what it got, and NGROK_SHA256=<that> scripts/download-tunnels.sh takes the new one once
# you have checked it (https://ngrok.com/download). Tailscale is not here: it is a system service
# (https://tailscale.com/download/linux).
set -euo pipefail

DEST="${NEXA_TUNNELS_DIR:-$HOME/.local/share/nexa/tunnels}"
CLOUDFLARED_VERSION="2026.10.7"
CLOUDFLARED_SHA256="1f99fb518b83dec2ba8d870a7cc991d82a77e6196295a3d2754ba17ab9f91872"
NGROK_URL="https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-linux-amd64.tgz"
NGROK_SHA256="${NGROK_SHA256:-233e0647849913e6404150c1e9c5e4f31c1368aefee330958d038d7afecb8f2e}"   # ngrok 3.39.13

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "These pins are for x86_64 Linux; on $(uname -m) download the programs from their own sites." >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$DEST"

check() {   # file, expected sha256, name
    local got
    got="$(sha256sum "$1" | cut -d' ' -f1)"
    if [[ "$got" != "$2" ]]; then
        echo "$3: the download is not the one pinned." >&2
        echo "  expected $2" >&2
        echo "  got      $got" >&2
        return 1
    fi
}

want="${1:-all}"

if [[ "$want" == "all" || "$want" == "cloudflared" ]]; then
    echo "cloudflared $CLOUDFLARED_VERSION"
    curl -fsSL -o "$work/cloudflared" \
        "https://github.com/cloudflare/cloudflared/releases/download/$CLOUDFLARED_VERSION/cloudflared-linux-amd64"
    check "$work/cloudflared" "$CLOUDFLARED_SHA256" cloudflared
    install -m 0755 "$work/cloudflared" "$DEST/cloudflared"
    echo "  $("$DEST/cloudflared" --version 2>&1 | head -n 1)"
fi

if [[ "$want" == "all" || "$want" == "ngrok" ]]; then
    echo "ngrok (stable)"
    curl -fsSL -o "$work/ngrok.tgz" "$NGROK_URL"
    if ! check "$work/ngrok.tgz" "$NGROK_SHA256" ngrok; then
        echo "  ngrok has released a new build. Check it, then: NGROK_SHA256=<got> $0 ngrok" >&2
        exit 1
    fi
    tar -xzf "$work/ngrok.tgz" -C "$work" ngrok
    install -m 0755 "$work/ngrok" "$DEST/ngrok"
    echo "  $("$DEST/ngrok" version 2>&1 | head -n 1)"
fi

echo
echo "Installed in $DEST. Once, as yourself, in a terminal:"
echo "  Cloudflare: $DEST/cloudflared tunnel login        (your domain must be on your Cloudflare account)"
echo "  ngrok:      $DEST/ngrok config add-authtoken <token>   (the token from dashboard.ngrok.com — Nexa never sees it)"
echo "Then Settings → Remote → Away from home."
