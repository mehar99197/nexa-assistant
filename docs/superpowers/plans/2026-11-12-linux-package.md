# M28 — The Linux package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A self-contained `nexa-assistant_1.0.0_amd64.deb` that installs on a clean Ubuntu 24.04 with the root
helper wired up, finds its bundled speech engines, downloads its models from inside Settings, and comes out of a
GitHub Actions run.

**Architecture:** CMake `install()` rules give the package its layout from one source; `scripts/build-deb.sh` turns a
`cmake --install` staging tree into the `.deb` (linuxdeploy's library closure + the pinned sherpa-onnx and Piper
archives, RPATHs patched, `dpkg-deb`), the same script on a laptop and in CI; `scripts/test-deb.sh` installs it in a
clean container. In the app, `EngineLocator` finds engines in the user's folder or the package's, `EngineAssets` is
the pure manifest of the models, `AssetDownloader` fetches them with resume and verification, and Settings → Voice
drives it.

**Tech Stack:** CMake 3.16+, Qt 6.8 (aqtinstall on the runner; 6.11 here), whisper.cpp 1.9.5 static, polkit-qt-1
0.201.1 built from source on the runner, linuxdeploy `1-alpha-20251107-1` + plugin-qt `1-alpha-20250213-1`, dpkg-deb,
Docker (local smoke test), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-12-ship-and-accounts-design.md` (section M28)

## Global Constraints

- Version `1.0.0` in `project(Nexa VERSION …)`; `nexa-assistant --version` prints `nexa-assistant 1.0.0` without a display.
- Package layout exactly as the spec's table: `/usr/bin/nexa-assistant`, `/usr/libexec/nexa-assistant/nexa-helper`,
  `/usr/lib/nexa-assistant/{lib,plugins,sherpa/lib,piper}`, `/usr/share/applications/org.nexa.Assistant.desktop`,
  `/usr/share/icons/hicolor/scalable/apps/org.nexa.Assistant.svg`, `/usr/share/dbus-1/system-services/org.nexa.Helper.service`,
  `/etc/dbus-1/system.d/org.nexa.Helper.conf`, `/usr/share/polkit-1/actions/org.nexa.helper.policy`,
  `/usr/lib/systemd/system/nexa-helper.service`, `/usr/share/doc/nexa-assistant/`.
- Engines looked up in the user's data folder first (`~/.local/share/nexa/{sherpa,piper}`), then `NEXA_ENGINE_DIR`
  (`/usr/lib/nexa-assistant` in a packaged build); models and voices always under the user's data folder.
- Pins: sherpa-onnx `v2.0.1` lib archive sha `532c45bbd41036d83cd70606797529aa8086c2053395a58967736c727625b120`; Piper
  `2023.11.14-2` `piper_linux_x86_64.tar.gz` sha `a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992`;
  polkit-qt-1 `0.201.1` sha `95d8e2cae530c546d9437459deb8a1bea38d572f0f9772cc2860dc143637f256`; linuxdeploy sha
  `c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d`, plugin-qt sha
  `15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724`.
- whisper.cpp in a packaged build: `GGML_NATIVE=OFF`, `GGML_AVX=ON GGML_AVX2=ON GGML_FMA=ON GGML_F16C=ON` (any x86-64
  CPU since 2013); the download page will say so.
- Libraries never bundled (from the host): glibc, libstdc++, libgcc, the GL/EGL/DRM stack — NDM's `DENY` list.
- Every new source file in its `CMakeLists.txt`; conventions of CLAUDE.md; the helper, its policy and D-Bus rule
  unchanged in content, only their paths templated.

## Review Focus

1. A `.deb` installed while the source-built helper from `install-helper.sh` is present (`/usr/local/libexec`, the
   same bus name) → the package's `postinst` must not fight it: it reloads D-Bus and systemd and says nothing;
   which `nexa-helper` answers the name is whichever unit file systemd loads last (`/etc/systemd/system` wins over
   `/usr/lib/systemd/system`, so the source one). Task 5's `test-deb.sh` does not cover this; `README` documents
   "uninstall-helper.sh before installing the package".
2. A machine without `tar` or `bzip2` (minimal containers) → `AssetDownloader` reports "tar is needed to unpack
   the models" rather than hanging (Task 4 test: a fake tar path that does not exist).
3. A download interrupted at exactly the full size with a wrong hash (a changed upstream file) → the part file is
   removed and the asset reported, never installed (Task 4 test).
4. `--ask` inside the clean container with no session bus and no config → the app must still start, run the fast
   path and save the screenshot (Task 5's smoke test is exactly this; if `QDBusConnection::sessionBus()` is not
   connected, every bus user already checks `isConnected()` — Task 5 proves it).
5. The user's own `~/.local/share/nexa/sherpa/lib` from an older script beside the package's newer one → the user's
   wins (Task 2's test pins it), and Settings → Voice says which is in use.

---

### Task 1: Version, `--version`, the install layout and the templated helper files

**Files:** Modify `CMakeLists.txt`, `app/CMakeLists.txt`, `app/main.cc`, `helper/CMakeLists.txt`,
`packaging/install-helper.sh`; rename `packaging/org.nexa.Helper.service` → `packaging/org.nexa.Helper.service.in`
and `packaging/nexa-helper.service` → `packaging/nexa-helper.service.in` (`Exec=`/`ExecStart=`/`ExecStopPost=`
take `@NEXA_HELPER_PATH@`); create `packaging/qt.conf` (`[Paths]` `Prefix=../lib/nexa-assistant` `Plugins=plugins`),
`packaging/debian/control.in`, `packaging/debian/postinst`, `packaging/debian/prerm`, `packaging/debian/postrm`,
`packaging/debian/copyright`.

**Interfaces — Produces:** CMake option `NEXA_PACKAGED` (OFF): ON embeds `packaging/qt.conf` as the `:/qt/etc/qt.conf`
resource of `nexa_assistant` and sets `NEXA_ENGINE_DIR` to `${CMAKE_INSTALL_FULL_LIBDIR}/nexa-assistant`; cache
variable `NEXA_ENGINE_DIR` (a compile definition on `nexa_voice`, the string `""` when not packaged); the
configured `${CMAKE_BINARY_DIR}/packaging/org.nexa.Helper.service` and `nexa-helper.service`.

- [ ] `project(Nexa VERSION 1.0.0 …)`; `include(GNUInstallDirs)`; the option and the cache variable; `install(TARGETS nexa_assistant RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})`; in `helper/CMakeLists.txt` `install(TARGETS nexa_helper RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/nexa-assistant)`; `set(NEXA_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/nexa-assistant/nexa-helper")`, `configure_file` of the two `.in` files, `install(FILES …)` of the service to `${CMAKE_INSTALL_DATADIR}/dbus-1/system-services`, the conf to `/etc/dbus-1/system.d` (absolute), the policy to `${CMAKE_INSTALL_DATADIR}/polkit-1/actions`, the unit to `${CMAKE_INSTALL_LIBDIR}/systemd/system`, the desktop entry to `${CMAKE_INSTALL_DATADIR}/applications`, the svg to `${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps`, `README.md` and `THIRD_PARTY_NOTICES.md` to `${CMAKE_INSTALL_DOCDIR}` (`/usr/share/doc/nexa-assistant`).
- [ ] `app/main.cc`: before `QApplication` is built, `if (argc == 2 && (argv[1] == "--version"sv || argv[1] == "-v"sv)) { std::puts("nexa-assistant " NEXA_VERSION); return 0; }`; `QCoreApplication::setApplicationVersion(NEXA_VERSION)` after it. In `app/CMakeLists.txt`: `if(NEXA_PACKAGED) qt_add_resources(nexa_assistant "nexa_qtconf" PREFIX "/qt/etc" FILES packaging/qt.conf (alias qt.conf))`.
- [ ] `packaging/install-helper.sh`: generate the two files with `sed "s|@NEXA_HELPER_PATH@|/usr/local/libexec/nexa-helper|"` from the `.in` sources into a temp dir, install from there (the rest unchanged).
- [ ] `packaging/debian/control.in` (fields: `Package: nexa-assistant`, `Version: @VERSION@`, `Architecture: amd64`, `Maintainer: Nexa Maintainers <tauseeqtufail649@gmail.com>`, `Installed-Size: @INSTALLED_SIZE@`, `Depends: libc6 (>= 2.39), libstdc++6, libgcc-s1, libgl1, libegl1, dbus, polkitd | policykit-1, systemd`, `Recommends: adb, wireplumber, libreoffice-writer, poppler-utils`, `Section: utils`, `Priority: optional`, `Homepage: https://assistant.nexadownloadmanager.com`, `Description: Nexa Assistant — a Linux AI assistant …` two lines); `postinst` (`configure`: `systemctl daemon-reload`, `systemctl reload dbus || kill -HUP dbus-daemon`, `update-desktop-database`, `gtk-update-icon-cache`, all `|| true`); `prerm` (`remove`: `systemctl stop nexa-helper.service || true`); `postrm` (`remove|purge`: `systemctl daemon-reload`, desktop db, icon cache); `copyright` (the owner's name, 2026, "all rights reserved" unless a `LICENSE` exists — then its text).
- [ ] Build: `cmake --build build && ./build/app/nexa-assistant --version` prints `nexa-assistant 1.0.0` with `DISPLAY=` unset; `cmake --install build --prefix /tmp/nexa-stage/usr` lays the tree out (`find /tmp/nexa-stage -type f`), `grep Exec /tmp/nexa-stage/usr/share/dbus-1/system-services/org.nexa.Helper.service` shows `/tmp/nexa-stage/usr/libexec/nexa-assistant/nexa-helper`… (note: `CMAKE_INSTALL_FULL_LIBEXECDIR` is baked at configure time: configure the staging build with `-DCMAKE_INSTALL_PREFIX=/usr` and install with `DESTDIR=/tmp/nexa-stage` instead, so the baked path is `/usr/libexec/…`).
- [ ] `ctest` still green (the helper's install path is a configure-time string; `HelperClient` reads the service file's existence, unchanged). Commit `M28: version 1.0.0, --version, the install layout, the templated helper files`.

### Task 2: `EngineLocator` — engines from the user's folder or the package

**Files:** Create `voice/EngineLocator.h`, `voice/EngineLocator.cc`; test `tests/EngineLocatorTest.cc`; modify
`voice/wake/SherpaRuntime.h/.cc`, `voice/VoiceBackends.h/.cc`, `app/main.cc`, `app/SettingsDialog.cc` (the "not
installed" sentences), `tests/VoiceBackendsTest.cc` / `tests/SherpaTest.cc` where they build `SherpaRuntime`,
`voice/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
// Where the voice engines live (Strategy by place): the user's own copies first — what the download scripts fill —
// then the package's, NEXA_ENGINE_DIR. Models and voices are the user's always.
class EngineLocator {
public:
    EngineLocator(QString userDataDir = VoiceBackends::dataDir(), QString packageDir = packageEngineDir());
    static QString packageEngineDir();          // NEXA_ENGINE_DIR, or empty
    QString sherpaLibraryDir() const;           // the first of user/sherpa, package/sherpa holding lib/libsherpa-onnx-c-api.so; else user/sherpa
    QString sherpaModelsDir() const;            // user/sherpa
    QString piperDir() const;                   // the first holding an executable `piper`; else user/piper
    QString piperVoicesDir() const;             // user/piper/voices
    QString modelsDir() const;                  // user/models
    bool sherpaFromPackage() const; bool piperFromPackage() const;   // for Settings' sentences
};
```
`SherpaRuntime(QString modelsDir = …, QString libraryDir = …)`: `load()` opens `libraryDir/lib/libsherpa-onnx-c-api.so`,
the model paths come from `modelsDir`. `VoiceBackends::piperDir()`/`piperBinary()`/`piperVoices()` take their folders
from an `EngineLocator` (`VoiceBackends::locator()` — a function-local default built from the data dir and the
package dir); `piperVoices()` lists `piperVoicesDir()` first, then `piperDir()/voices` without duplicates.

- [ ] `tests/EngineLocatorTest.cc`: `"The user's engine wins over the package's, the package stands in, and nothing means the user's folder"` (three temp-dir arrangements; `sherpaFromPackage()`), `"Models and voices are the user's wherever the engine is"`, `"No package dir compiled in means no second place"` (`packageDir` empty). Run → fails.
- [ ] Implement; wire `main.cc` (`EngineLocator engines; SherpaRuntime sherpa(engines.sherpaModelsDir(), engines.sherpaLibraryDir());`), the Settings sentences: "Instant spotting: installed (from the package)" / "(your copy)"; the Piper status likewise.
- [ ] `./build/tests/nexa_tests "*EngineLocator*,*VoiceBackends*,*Sherpa*"` green; `ctest` green. Commit `M28: EngineLocator — engines from the user's folder or the package`.

### Task 3: `EngineAssets` — the manifest of the models (pure)

**Files:** Create `system/setup/EngineAssets.h`, `system/setup/EngineAssets.cc`; test `tests/EngineAssetsTest.cc`;
`system/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
namespace nexa::engineassets {
struct Asset {
    QString id;             // "whisper-base.en", "piper-en_US-lessac-medium", "silero", "gtcrn", "speaker", "kws", "zipvoice", "vocos"
    QString group;          // "listening" | "speaker" | "voice" | "own-voice"
    QString url;
    qint64 bytes{0};        // of the download (the archive, for one)
    QString sha256;         // of the download
    QString installDir;     // relative to the data dir: "models", "piper/voices", "sherpa", "sherpa/speaker", "sherpa/kws", "sherpa/zipvoice"
    QStringList files;      // installed names; for an archive, the members under its top folder; one entry for a plain file
    QString archiveTop;     // the archive's top folder ("sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01"), empty for a plain file
};
QList<Asset> all();
QList<Asset> missing(const QString &dataDir);        // an asset is present when every installed file exists
qint64 totalBytes(const QList<Asset> &assets);
QString groupTitle(const QString &group);           // "Listening (wake name, speech)", "Your voice (who is speaking)", "Speaking (Piper voice)", "Speak in my voice (ZipVoice)"
}
```
Values: whisper `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin` 147964211
`a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002` → `models/ggml-base.en.bin`; Piper voice
`https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx` 63201294
`5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f` and its `.onnx.json` 4885
`efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0` → `piper/voices/`; silero
`…/asr-models/silero_vad.onnx` 643854 `9e2449e1…1fd6`; gtcrn `…/speech-enhancement-models/gtcrn_simple.onnx` 535638
`e77603ac…b534`; speaker `…/speaker-recongition-models/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx` 26485263
`c5915837…2db5` → `sherpa/speaker`; kws archive `…/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2`
(bytes from a HEAD request, pinned) `f170013b…561a`, files the five of `KWS_FILES` → `sherpa/kws`; zipvoice archive
`…/tts-models/sherpa-onnx-zipvoice-distill-int8-zh-en-emilia.tar.bz2` 109162785 `77219c8b…2803`, files
`encoder.int8.onnx decoder.int8.onnx tokens.txt lexicon.txt` → `sherpa/zipvoice`; vocos
`…/vocoder-models/vocos_24khz.onnx` 54157409 `bcb3b970…3cc0` → `sherpa/zipvoice`. (`…` = the sherpa-onnx releases
base `https://github.com/k2-fsa/sherpa-onnx/releases/download`; full hashes in the scripts.)

- [ ] `tests/EngineAssetsTest.cc`: `"The manifest pins what the download scripts pin"` (reads `scripts/download-sherpa.sh` and `scripts/download-piper.sh` from `NEXA_TESTS_DIR/..`, extracts every `*_SHA256="…"` and checks each appears in `all()`), `"missing() sees what is installed"` (a temp data dir with the whisper file and the kws files → those two absent from `missing()`, the rest present; `totalBytes`), `"Every asset has a url, a hash, an install dir and at least one file"`. Run → fails. Implement. Green. Commit `M28: EngineAssets — the pinned manifest of the speech models`.

### Task 4: `AssetDownloader` and Settings → Voice → Speech models

**Files:** Create `system/setup/AssetDownloader.h`, `system/setup/AssetDownloader.cc`; test
`tests/AssetDownloaderTest.cc`; modify `tests/FakeHttpServer.h` (Range), `app/SettingsDialog.h/.cc`,
`app/SystemStatus.h` (`AssetDownloader *assets`, `EngineLocator *engines`), `app/main.cc`, `app/MainWindow.cc`
(status-bar hint), CMake lists.

**Interfaces — Produces:**
```cpp
// Fetches the models EngineAssets lists into the data folder: one at a time, into <file>.part, resumed with a Range
// request, SHA-256 checked before the file is renamed into place; an archive is unpacked with `tar` into a scratch
// folder and only the listed members installed. Nothing on the GUI thread but the signals.
class AssetDownloader : public QObject {
    Q_OBJECT
public:
    explicit AssetDownloader(QString dataDir, QObject *parent = nullptr);
    void setTarProgram(const QString &path);        // tests: a missing one
    void start(const QList<engineassets::Asset> &assets);
    void cancel();
    bool isRunning() const;
signals:
    void progress(const QString &assetId, qint64 received, qint64 total);
    void assetInstalled(const QString &assetId);
    void finished();                                // every asset installed
    void failed(const QString &assetId, const nexa::Error &error);   // and stopped
};
```
`FakeHttpServer::Response` gains `QByteArray body` + `bool honourRange` (a `Range: bytes=N-` request answers 206 with
`Content-Range` and the tail; `Content-Length` always sent) and `QList<QByteArray> requestHeads` (every request's head).

- [ ] `tests/AssetDownloaderTest.cc`: `"A plain file downloads, verifies and lands in its folder"`, `"A part file resumes from where it stopped"` (write a `.part` of the first 100 bytes; the server sees `Range: bytes=100-`; the result verifies), `"A wrong hash removes the part and fails the asset"`, `"An archive unpacks and only the listed members are installed"` (the test makes a `tar.bz2` with `tar -cjf` through QProcess), `"Without tar the archive asset fails in words"` (`setTarProgram("/nonexistent/tar")`), `"cancel() stops and leaves the part for next time"`. Run → fails. Implement. Green.
- [ ] Settings → Voice gains a group **Speech models** above *Listening*: one line per group of `EngineAssets` — "Listening (wake name, speech): installed" / "missing, 153 MB" — a **Download (N MB)** button for what is missing, a `QProgressBar` and a label with the current file; the whisper/spotter/Piper status sentences now say "Download it above" instead of naming scripts; the Settings dialog refuses OK while a download runs? No — the downloader lives in `main.cc` (owned there, handed through `SystemStatus`), so closing Settings does not stop it, and the status bar shows "Downloading speech models: 42 %". `MainWindow` status bar: "Speech models missing — Settings → Voice" while `missing()` is not empty and nothing downloads.
- [ ] Offscreen check: `--screenshot-settings /tmp/m28` shows the group on the Voice tab (the test copy with a private data dir sees everything missing). `ctest` green. Commit `M28: AssetDownloader and Settings → Voice → Speech models`.

### Task 5: `build-deb.sh`, `test-deb.sh`, the workflow

**Files:** Create `scripts/build-deb.sh`, `scripts/test-deb.sh`, `.github/workflows/build.yml`.

- [ ] `scripts/build-deb.sh [--build-dir build] [--out dist] [--skip-configure]`: (1) version from `project()`; (2) configure if needed: `-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DNEXA_PACKAGED=ON -DNEXA_BUILD_WHISPER=ON -DNEXA_BUILD_TESTS=OFF -DNEXA_BUILD_PHONE=OFF -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON`, build; (3) `DESTDIR=$STAGE cmake --install`; (4) linuxdeploy + plugin-qt (pinned, checksummed, `--appimage-extract`), `QMAKE` from `qmake6` on PATH or `Qt6_DIR`, `AppRun --appdir AppDir -e $STAGE/usr/bin/nexa-assistant -e $STAGE/usr/libexec/nexa-assistant/nexa-helper --plugin qt`, the closure passes with the DENY list (NDM's loop), copy `AppDir/usr/lib` → `$STAGE/usr/lib/nexa-assistant/lib`, `AppDir/usr/plugins` → `…/plugins`; (5) `patchelf --set-rpath '$ORIGIN/../lib/nexa-assistant/lib' nexa-assistant`, `'$ORIGIN/../../lib/nexa-assistant/lib'` for the helper, `'$ORIGIN'` for every lib, `'$ORIGIN/../..'` for every plugin (plugins sit in `plugins/<type>/`, libs in `lib/` — so `'$ORIGIN/../../lib'`); (6) the sherpa lib archive and the Piper archive fetched into `~/.cache/nexa/downloads` (the scripts' pins), `sherpa/lib/{libsherpa-onnx-c-api.so,libonnxruntime.so}` and the whole `piper/` folder (without `pkgconfig`) into `$STAGE/usr/lib/nexa-assistant/`; (7) `DEBIAN/control` from `control.in` (`Installed-Size` = `du -sk`), the three scripts `0755`, `dpkg-deb --build --root-owner-group $STAGE $OUT/nexa-assistant_${VERSION}_amd64.deb`; (8) `dpkg-deb --info`, `dpkg-deb --contents | wc -l`, and `ldd` of the two binaries from the stage with `LD_LIBRARY_PATH` unset reports no "not found" except the DENY set.
- [ ] `scripts/test-deb.sh dist/nexa-assistant_*.deb`: `docker run --rm -v "$PWD/dist:/pkg:ro" ubuntu:24.04 bash -euxc 'apt-get update -qq && apt-get install -y -qq /pkg/*.deb && nexa-assistant --version && mkdir -p /tmp/xdg && XDG_RUNTIME_DIR=/tmp/xdg QT_QPA_PLATFORM=offscreen nexa-assistant --new-instance --ask "run uname -sr" --screenshot /tmp/shot.png && test -s /tmp/shot.png && test -x /usr/libexec/nexa-assistant/nexa-helper && /usr/libexec/nexa-assistant/nexa-helper --version'` (the helper's `--version`: add it if missing — it must run as a plain user and print `nexa-helper 1.0.0`).
- [ ] Run both here: `scripts/build-deb.sh --build-dir build-deb --out dist` (a fresh build dir configured packaged; this laptop's Qt 6.11 is fine for the local check) then `scripts/test-deb.sh`. Fix what the container shows.
- [ ] `.github/workflows/build.yml`: `on: push` (all branches), `pull_request`, `workflow_dispatch`; `permissions: contents: write`; job `linux` on `ubuntu-24.04`, `timeout-minutes: 60`: checkout; apt `cmake ninja-build pkg-config patchelf file dpkg-dev libxkbcommon-dev libqrencode-dev libpolkit-gobject-1-dev libpolkit-agent-1-dev extra-cmake-modules libgl-dev libegl-dev libxcb*-dev libfontconfig-dev libfreetype-dev libasound2-dev libpulse-dev libspeechd-dev adb`; `jurplel/install-qt-action@v4` (`version: 6.8.3`, `modules: qthttpserver qtwebsockets qtmultimedia qtspeech`, `cache: true`); polkit-qt-1 0.201.1 (curl, sha check, `cmake -DQT_MAJOR_VERSION=6 -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF`, `sudo cmake --install`); `scripts/build-deb.sh --out dist` (`CMAKE_PREFIX_PATH` from the action); a test build `cmake --preset release`-like with tests ON and `QT_QPA_PLATFORM=offscreen ctest` (whisper ON, no models: the model tests skip); `scripts/test-deb.sh`; `actions/upload-artifact@v4` of `dist/*.deb`. Job `release` (`needs: linux`, `if: startsWith(github.ref, 'refs/tags/v')`): download the artifact, `softprops/action-gh-release@v2` with the `.deb`, `generate_release_notes: true`.
- [ ] Commit `M28: build-deb.sh, test-deb.sh and the GitHub workflow`.

### Task 6: Docs, push, the first run, verification

**Files:** Modify `README.md` (an **Install** section at the top of "Build": the `.deb`, what it needs, the models
download, the helper, `uninstall-helper.sh` before installing over a source install), `CLAUDE.md` (Build section:
`NEXA_PACKAGED`, `scripts/build-deb.sh`, `scripts/test-deb.sh`, the engine lookup order, the workflow),
`ARCHITECTURE.md` (M28 row, Status line).

- [ ] Docs. `cmake --build build-debug` clean; `ctest` green. Commit `M28: docs`.
- [ ] `git push origin m20-hardcore`; `gh workflow run build.yml --ref m20-hardcore`; `gh run watch`; on red, fix and push again (the runner's Ubuntu and Qt differ from this laptop); on green `gh run download` the artifact and `scripts/test-deb.sh` it here.
- [ ] ARCHITECTURE's M28 row with the evidence: [VERIFIED] the CI `.deb` installs and runs in a clean `ubuntu:24.04` here; the sizes of the `.deb` and of the models; what is [REASONED] (a GNOME desktop install of the package was not done on this laptop — it runs the source build and the source-installed helper).
