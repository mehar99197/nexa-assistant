#!/usr/bin/env bash
# Builds the self-contained Debian package of Nexa Assistant: nexa-assistant_<version>_amd64.deb.
#
#   scripts/build-deb.sh [--build-dir build-deb] [--out dist] [--skip-configure]
#
# The same script on a laptop and in CI (.github/workflows/build.yml). It configures a packaged build
# (NEXA_PACKAGED=ON: Qt's plugins through the built-in qt.conf, the speech engines under /usr/lib/nexa-assistant),
# installs it into a staging tree, bundles the complete library closure of nexa-assistant and nexa-helper with
# linuxdeploy (glibc, libstdc++/libgcc and the GL stack stay the host's), adds the pinned sherpa-onnx and Piper
# engines, patches RPATHs, and builds the .deb with dpkg-deb. The models are not in the package: the app downloads
# them into the user's folder (Settings → Voice). scripts/test-deb.sh installs the result in a clean container.
#
# Needs: cmake, ninja, a Qt 6.6+ (qmake6 or qmake on PATH, or QMAKE=…), patchelf, dpkg-deb, curl, tar, file.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-deb"
OUT="$ROOT/dist"
CONFIGURE=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD="$(cd "$(dirname "$2")" 2>/dev/null && pwd)/$(basename "$2")"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --skip-configure) CONFIGURE=0; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# ── Pins (every third-party download is checked before use) ─────────────────────────────────────────────────
LINUXDEPLOY_TAG='1-alpha-20251107-1'
LINUXDEPLOY_SHA256='c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d'
LINUXDEPLOY_QT_TAG='1-alpha-20250213-1'
LINUXDEPLOY_QT_SHA256='15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724'
SHERPA_URL='https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-linux-x64-shared-lib.tar.bz2'
SHERPA_SHA256='532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120'
SHERPA_TOP='sherpa-onnx-v2.0.1-linux-x64-shared-lib'
PIPER_URL='https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz'
PIPER_SHA256='a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992'
# Never bundled — the host's: glibc (tied to the kernel), libstdc++/libgcc (forward compatible, safer from the
# host) and the GPU/GL driver stack (a driver bundled for one GPU crashes on another).
DENY='^(ld-linux.*|libc|libm|libmvec|libanl|libdl|libpthread|librt|libresolv|libnss_[a-z]+|libstdc\+\+|libgcc_s|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libdrm|libgbm|libglapi)\.so'

VERSION="$(sed -n 's/^\s*VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
[[ -n "$VERSION" ]] || { echo "could not read the version from CMakeLists.txt" >&2; exit 1; }
echo "== Nexa Assistant $VERSION"

# ── Build ────────────────────────────────────────────────────────────────────────────────────────────────────
if [[ $CONFIGURE -eq 1 || ! -f "$BUILD/build.ninja" ]]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DNEXA_PACKAGED=ON -DNEXA_BUILD_WHISPER=ON -DNEXA_BUILD_TESTS=OFF -DNEXA_BUILD_PHONE=OFF \
        -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON
fi
grep -q 'NEXA_PACKAGED:BOOL=ON' "$BUILD/CMakeCache.txt" || { echo "$BUILD is not a packaged build (NEXA_PACKAGED=ON)" >&2; exit 1; }
grep -q 'CMAKE_INSTALL_PREFIX:PATH=/usr$' "$BUILD/CMakeCache.txt" || { echo "$BUILD must be configured with CMAKE_INSTALL_PREFIX=/usr" >&2; exit 1; }
cmake --build "$BUILD"

# ── Stage ────────────────────────────────────────────────────────────────────────────────────────────────────
# The work happens in a folder without spaces: the repo may live in one ("Ai Assistant"), and linuxdeploy's own
# ldd parsing, like a naive one, breaks on it. The tools are kept between runs; the stage and the AppDir are not.
WORK="${NEXA_DEB_WORK:-${TMPDIR:-/tmp}/nexa-deb}"
STAGE="$WORK/stage"
rm -rf "$STAGE" "$WORK/AppDir"
mkdir -p "$WORK/tools" "$STAGE"
DESTDIR="$STAGE" cmake --install "$BUILD" >/dev/null
APP="$STAGE/usr/bin/nexa-assistant"
HELPER="$STAGE/usr/libexec/nexa-assistant/nexa-helper"
LIBDIR="$STAGE/usr/lib/nexa-assistant"
test -x "$APP" && test -x "$HELPER"
strip --strip-unneeded "$APP" "$HELPER"

# ── The library closure (linuxdeploy + its Qt plugin, extracted: no FUSE needed) ─────────────────────────────
fetch() {   # url target sha256
    if [[ ! -f "$2" ]]; then
        curl -fL --retry 3 -sS -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    echo "$3  $2" | sha256sum --check --quiet - || { rm -f "$2"; echo "checksum mismatch: $2" >&2; exit 1; }
}
cd "$WORK/tools"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage" linuxdeploy "$LINUXDEPLOY_SHA256"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT_TAG/linuxdeploy-plugin-qt-x86_64.AppImage" linuxdeploy-plugin-qt "$LINUXDEPLOY_QT_SHA256"
chmod +x linuxdeploy linuxdeploy-plugin-qt
[[ -d ld ]] || { ./linuxdeploy --appimage-extract >/dev/null && mv squashfs-root ld; }
[[ -d ldqt ]] || { ./linuxdeploy-plugin-qt --appimage-extract >/dev/null && mv squashfs-root ldqt; }
ln -sf "$PWD/ldqt/AppRun" ld/usr/bin/linuxdeploy-plugin-qt
export PATH="$PWD/ld/usr/bin:$PATH"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
[[ -n "$QMAKE" ]] || { echo "qmake6 not found: put Qt's bin on the PATH or set QMAKE" >&2; exit 1; }
# xcb serves X11 and XWayland; the Wayland platform plugins are bundled too, whichever this Qt has (one
# libqwayland.so from 6.10 on, libqwayland-generic.so and libqwayland-egl.so before), so GNOME gets a native
# Wayland window — the app id, the portals and the global shortcuts all prefer it.
QT_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
WAYLAND_PLUGINS="$(cd "$QT_PLUGINS/platforms" 2>/dev/null && ls libqwayland*.so 2>/dev/null | paste -sd ';' - || true)"
# offscreen goes in too: --screenshot runs, the tests and the package's own container test use it.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so"
if [[ -n "$WAYLAND_PLUGINS" ]]; then
    EXTRA_PLATFORM_PLUGINS="$EXTRA_PLATFORM_PLUGINS;$WAYLAND_PLUGINS"
    echo "== Wayland platform plugins: $WAYLAND_PLUGINS"
else
    echo "== this Qt has no Wayland platform plugin; xcb (XWayland) only"
fi
cd "$WORK"
mkdir -p AppDir
"$WORK/tools/ld/AppRun" --appdir AppDir -e "$APP" -e "$HELPER" --plugin qt > "$WORK/linuxdeploy.log" 2>&1 \
    || { tail -40 "$WORK/linuxdeploy.log"; echo "linuxdeploy failed (full log: $WORK/linuxdeploy.log)" >&2; exit 1; }
test -d AppDir/usr/lib || { echo "linuxdeploy produced no AppDir/usr/lib" >&2; exit 1; }
# A Wayland window needs the shell, graphics and decoration integration plugins beside the platform plugin; their
# libraries join the closure in the passes below.
if [[ -n "$WAYLAND_PLUGINS" ]]; then
    for d in wayland-shell-integration wayland-graphics-integration-client wayland-decoration-client; do
        [[ -d "$QT_PLUGINS/$d" ]] && cp -a "$QT_PLUGINS/$d" AppDir/usr/plugins/
    done
fi

# linuxdeploy keeps a few libraries host-side by its own list (fontconfig, freetype, harfbuzz, X11…). For a
# package that runs on a clean machine the whole closure is wanted: a few passes copy what it left behind until
# nothing new appears.
for pass in 1 2 3 4 5 6; do
    added=0
    while IFS= read -r lib; do
        base="$(basename "$lib")"
        echo "$base" | grep -qE "$DENY" && continue
        [[ -e "AppDir/usr/lib/$base" ]] && continue
        cp -Ln "$lib" AppDir/usr/lib/ && added=$((added + 1))
    done < <({ find AppDir/usr/lib AppDir/usr/plugins -name '*.so*' -type f; echo "$APP"; echo "$HELPER"; } \
             | while IFS= read -r obj; do ldd "$obj" 2>/dev/null | sed -n 's/^.* => \(\/.*\) (0x[0-9a-f]*)$/\1/p'; done | sort -u)
    [[ $added -eq 0 ]] && break
done
for f in AppDir/usr/lib/*; do
    echo "$(basename "$f")" | grep -qE "$DENY" && rm -f "$f"
done
test -f AppDir/usr/plugins/platforms/libqxcb.so || { echo "the xcb platform plugin was not bundled" >&2; exit 1; }
test -n "$(find AppDir/usr/plugins/tls -name '*.so' 2>/dev/null)" || { echo "no TLS backend was bundled" >&2; exit 1; }
echo "== $(ls AppDir/usr/lib | wc -l) libraries, $(find AppDir/usr/plugins -name '*.so' | wc -l) Qt plugins bundled"
ls AppDir/usr/plugins/platforms

mkdir -p "$LIBDIR/lib" "$LIBDIR/plugins"
cp -a AppDir/usr/lib/. "$LIBDIR/lib/"
cp -a AppDir/usr/plugins/. "$LIBDIR/plugins/"

# ── RPATHs: everything finds the bundled libraries from where it lies ────────────────────────────────────────
patchelf --set-rpath '$ORIGIN/../lib/nexa-assistant/lib' "$APP"
patchelf --set-rpath '$ORIGIN/../../lib/nexa-assistant/lib' "$HELPER"
find "$LIBDIR/lib" -name '*.so*' -type f -exec patchelf --set-rpath '$ORIGIN' {} +
find "$LIBDIR/plugins" -name '*.so' -type f -exec patchelf --set-rpath '$ORIGIN/../../lib' {} +

# ── The speech engines (the models download inside the app) ─────────────────────────────────────────────────
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/nexa/downloads"
mkdir -p "$CACHE" "$LIBDIR/sherpa/lib" "$LIBDIR/piper"
fetch "$SHERPA_URL" "$CACHE/$(basename "$SHERPA_URL")" "$SHERPA_SHA256"
tar -xjf "$CACHE/$(basename "$SHERPA_URL")" -C "$WORK" "$SHERPA_TOP/lib/libsherpa-onnx-c-api.so" "$SHERPA_TOP/lib/libonnxruntime.so"
install -m 0644 "$WORK/$SHERPA_TOP/lib/libsherpa-onnx-c-api.so" "$WORK/$SHERPA_TOP/lib/libonnxruntime.so" "$LIBDIR/sherpa/lib/"
rm -rf "$WORK/$SHERPA_TOP"
fetch "$PIPER_URL" "$CACHE/$(basename "$PIPER_URL")" "$PIPER_SHA256"
rm -rf "$WORK/piper" && tar -xzf "$CACHE/$(basename "$PIPER_URL")" -C "$WORK"
cp -a "$WORK/piper/." "$LIBDIR/piper/"
rm -rf "$LIBDIR/piper/pkgconfig" "$WORK/piper"
test -x "$LIBDIR/piper/piper" && test -x "$LIBDIR/piper/piper_phonemize" && test -d "$LIBDIR/piper/espeak-ng-data"

# ── Docs and the package metadata ────────────────────────────────────────────────────────────────────────────
install -m 0644 "$ROOT/packaging/debian/copyright" "$STAGE/usr/share/doc/nexa-assistant/copyright"
mkdir -p "$STAGE/DEBIAN"
INSTALLED_SIZE="$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)"
# The glibc the package needs is the newest symbol version any shipped ELF asks for — the build host's, in practice
# (a package built on Ubuntu 24.04 says 2.39; one from a rolling machine says more, and installs on fewer systems).
GLIBC="$( { echo "$APP"; echo "$HELPER"; find "$LIBDIR" -name '*.so*' -type f; } \
    | xargs -d '\n' objdump -T 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n | tail -1)"
[[ -n "$GLIBC" ]] || GLIBC="2.39"
echo "== needs glibc $GLIBC"
sed "s/@VERSION@/$VERSION/; s/@INSTALLED_SIZE@/$INSTALLED_SIZE/; s/@GLIBC@/$GLIBC/" "$ROOT/packaging/debian/control.in" > "$STAGE/DEBIAN/control"
for script in postinst prerm postrm; do
    install -m 0755 "$ROOT/packaging/debian/$script" "$STAGE/DEBIAN/$script"
done
find "$STAGE" -type d -exec chmod 0755 {} +

# ── Nothing unresolved but the host's own libraries ──────────────────────────────────────────────────────────
for bin in "$APP" "$HELPER"; do
    if LD_LIBRARY_PATH= ldd "$bin" | grep "not found"; then
        echo "$bin has unresolved libraries" >&2
        exit 1
    fi
done

DEB="$OUT/nexa-assistant_${VERSION}_amd64.deb"
rm -f "$DEB"
dpkg-deb --build -Zgzip --root-owner-group "$STAGE" "$DEB" >/dev/null
echo "== built $DEB ($(du -h "$DEB" | cut -f1))"
dpkg-deb --info "$DEB" | sed -n '1,12p'
echo "== $(dpkg-deb --contents "$DEB" | wc -l) entries"
