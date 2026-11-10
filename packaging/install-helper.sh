#!/usr/bin/env bash
# Installs nexa-helper, the small root-privileged part of Nexa, and checks that it works.
#
#   sudo ./packaging/install-helper.sh [path-to-nexa-helper]
#
# What goes where:
#   /usr/local/libexec/nexa-helper                                   the daemon
#   /usr/share/dbus-1/system-services/org.nexa.Helper.service        start it on demand
#   /etc/dbus-1/system.d/org.nexa.Helper.conf                        only root may own the name
#   /usr/share/polkit-1/actions/org.nexa.helper.policy               what needs a password
#   /etc/systemd/system/nexa-helper.service                          the unit D-Bus hands over to
#
# Remove it all again with packaging/uninstall-helper.sh. Nothing is installed anywhere else, and
# no file outside these five paths is touched.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "This script installs system files, so it needs root:" >&2
    echo "    sudo $0 $*" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-$ROOT/build/helper/nexa-helper}"
LIBEXEC="/usr/local/libexec"

if [[ ! -x "$BINARY" ]]; then
    echo "No helper binary at $BINARY." >&2
    echo "Build it first:  cmake --build build --target nexa_helper" >&2
    exit 1
fi
if ! file -b "$BINARY" | grep -q ELF; then
    echo "$BINARY is not a program." >&2
    exit 1
fi

echo "Installing the helper from $BINARY"
install -D -m 0755 "$BINARY" "$LIBEXEC/nexa-helper"
install -D -m 0644 "$ROOT/packaging/org.nexa.Helper.service" \
    /usr/share/dbus-1/system-services/org.nexa.Helper.service
install -D -m 0644 "$ROOT/packaging/org.nexa.Helper.conf" /etc/dbus-1/system.d/org.nexa.Helper.conf
install -D -m 0644 "$ROOT/packaging/org.nexa.helper.policy" \
    /usr/share/polkit-1/actions/org.nexa.helper.policy
install -D -m 0644 "$ROOT/packaging/nexa-helper.service" /etc/systemd/system/nexa-helper.service

systemctl daemon-reload
# The bus rereads /etc/dbus-1/system.d on a reload; without this the first call is refused.
systemctl reload dbus 2>/dev/null || kill -HUP "$(pgrep -o dbus-daemon)" 2>/dev/null || true

# An already-running helper keeps the bus name for up to ten idle minutes, so without this the old
# binary would go on answering — and an older one asks polkit about actions this policy file no
# longer declares, which polkit can only refuse. D-Bus starts the new one on the next call.
if systemctl is-active --quiet nexa-helper.service || pgrep -x nexa-helper >/dev/null; then
    systemctl stop nexa-helper.service 2>/dev/null || true
    pkill -x nexa-helper 2>/dev/null || true
    echo "Stopped the previous helper; the bus will start the new one on demand."
fi

echo
echo "Checking the installation:"

ok=0
if pkaction --action-id org.nexa.helper.control >/dev/null 2>&1; then
    echo "  polkit knows org.nexa.helper.control"
else
    echo "  polkit does NOT know the action — is polkitd running?" >&2
    ok=1
fi

# A password remembered on behalf of a helper that has just been replaced is of no use to anyone:
# this installation starts by asking once.
if [[ -d /run/nexa-helper ]]; then
    rm -rf /run/nexa-helper
    echo "  cleared the authorisation the previous helper had been given"
fi

# Starts the helper through D-Bus activation and asks it for its version. Version() needs no
# authorisation, so this must not produce a password prompt.
if version="$(timeout 25 busctl --system call org.nexa.Helper /org/nexa/Helper org.nexa.Helper Version 2>&1)"; then
    echo "  the helper starts and answers:${version#s }"
else
    echo "  the helper did not answer: $version" >&2
    echo "  look at:  journalctl -u nexa-helper -n 40" >&2
    ok=1
fi

if grep -q -- "--release-input" /etc/systemd/system/nexa-helper.service; then
    echo "  a stopped or crashed helper gives the keyboard and mouse back (hardcore mode)"
else
    echo "  the unit lacks ExecStopPost=--release-input — hardcore mode would not be safe" >&2
    ok=1
fi

if [[ -e /dev/uinput ]]; then
    echo "  /dev/uinput is present, so typing and clicking will work"
else
    echo "  /dev/uinput is missing — run 'modprobe uinput' (typing and clicking need it)" >&2
fi

echo
if [[ "$ok" -eq 0 ]]; then
    echo "Done. Nexa can now run commands as root, type for you, and lock the keyboard and mouse"
    echo "in hardcore mode. It asks for your password once per login — the desktop's own prompt,"
    echo "never Nexa's — and the helper exits on its own after ten idle minutes. To be asked again:"
    echo "Settings → Helper → Forget my password, or log out."
else
    echo "Installed, but the checks above did not all pass." >&2
fi
exit "$ok"
