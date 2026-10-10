#!/usr/bin/env bash
# Removes nexa-helper and everything install-helper.sh put in place, so Nexa is left with no
# root-privileged part at all.
#
#   sudo ./packaging/uninstall-helper.sh
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Removing system files needs root:" >&2
    echo "    sudo $0" >&2
    exit 1
fi

FILES=(
    /usr/local/libexec/nexa-helper
    /usr/share/dbus-1/system-services/org.nexa.Helper.service
    /etc/dbus-1/system.d/org.nexa.Helper.conf
    /usr/share/polkit-1/actions/org.nexa.helper.policy
    /etc/systemd/system/nexa-helper.service
)

# Stop it first, or a running daemon keeps the name until it idles out.
systemctl stop nexa-helper.service 2>/dev/null || true

for file in "${FILES[@]}"; do
    if [[ -e "$file" ]]; then
        rm -f "$file"
        echo "Removed $file"
    fi
done

# Where the helper kept the fact that a login had given it a password.
if [[ -d /run/nexa-helper ]]; then
    rm -rf /run/nexa-helper
    echo "Removed /run/nexa-helper"
fi

systemctl daemon-reload
systemctl reload dbus 2>/dev/null || true

if pgrep -x nexa-helper >/dev/null; then
    echo "A nexa-helper process is still running; stopping it." >&2
    pkill -x nexa-helper || true
fi

echo
echo "The root helper is gone. Nexa keeps working, but can no longer run commands as root,"
echo "and on Wayland it can no longer type or click."
