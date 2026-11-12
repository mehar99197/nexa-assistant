#!/usr/bin/env bash
# Installs a built package in a clean Ubuntu 24.04 container and runs it: the version, a fast-path request with a
# screenshot (offscreen, no session bus, no config), the helper's own version. What scripts/build-deb.sh made is
# only trusted once this passes.
#
#   scripts/test-deb.sh dist/nexa-assistant_1.0.0_amd64.deb
set -euo pipefail

DEB="${1:-}"
[[ -f "$DEB" ]] || { echo "usage: scripts/test-deb.sh <package.deb>" >&2; exit 2; }
DIR="$(cd "$(dirname "$DEB")" && pwd)"
NAME="$(basename "$DEB")"
IMAGE="${NEXA_TEST_IMAGE:-ubuntu:24.04}"

docker run --rm -v "$DIR:/pkg:ro" "$IMAGE" bash -euxc "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq /pkg/$NAME >/dev/null
    nexa-assistant --version
    /usr/libexec/nexa-assistant/nexa-helper --version
    test -f /usr/share/dbus-1/system-services/org.nexa.Helper.service
    grep -q '^Exec=/usr/libexec/nexa-assistant/nexa-helper' /usr/share/dbus-1/system-services/org.nexa.Helper.service
    test -f /usr/lib/nexa-assistant/sherpa/lib/libsherpa-onnx-c-api.so
    test -x /usr/lib/nexa-assistant/piper/piper
    mkdir -p /tmp/xdg && chmod 0700 /tmp/xdg
    export XDG_RUNTIME_DIR=/tmp/xdg QT_QPA_PLATFORM=offscreen HOME=/root
    timeout 120 nexa-assistant --new-instance --ask 'run uname -sr' --screenshot /tmp/shot.png
    test -s /tmp/shot.png
    echo 'package OK'
"
