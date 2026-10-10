#!/usr/bin/env bash
# Builds Nexa's phone app (android/) as an APK with the Qt for Android kit, and prints where it is.
#
# Needs: the Qt Android kit (aqt install-qt all_os android <version> android_arm64_v8a …), the Android
# SDK with platform-tools, a platform, build-tools and an NDK (sdkmanager), a JDK 17 or later, and the
# system's own Qt of the same version for the host tools (Debian ships androiddeployqt in
# qt6-base-dev-tools). Gradle and the Android Gradle plugin are fetched by the first build. Every path
# can be overridden through the variables below.
set -euo pipefail
cd "$(dirname "$0")/.."

QT_VERSION="${QT_VERSION:-6.10.2}"
QT_ANDROID="${QT_ANDROID:-$HOME/Qt/$QT_VERSION/android_arm64_v8a}"
QT_HOST_PATH="${QT_HOST_PATH:-/usr}"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$(ls -d "$ANDROID_SDK_ROOT"/ndk/* 2>/dev/null | sort -V | tail -n 1)}"
# Gradle 8.14 (Qt's) runs on Java 17 to 24: the newest JDK in that range, not the newest there is —
# and a JDK, with javac, not the JRE Debian installs by default (`apt install openjdk-21-jdk-headless`,
# or a Temurin JDK unpacked under ~/Android).
if [ -z "${JAVA_HOME:-}" ]; then
    for version in 24 23 22 21 20 19 18 17; do
        for candidate in /usr/lib/jvm/java-$version-openjdk-* "$HOME"/Android/jdk-$version*; do
            if [ -x "$candidate/bin/javac" ]; then
                JAVA_HOME="$candidate"
                break 2
            fi
        done
    done
fi
JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/a-jdk-with-javac}"
BUILD_DIR="${BUILD_DIR:-build-android}"

# Debian keeps Qt's CMake files under the multiarch directory; the kit expects to be told.
if [ -d "$QT_HOST_PATH/lib/x86_64-linux-gnu/cmake/Qt6HostInfo" ]; then
    QT_HOST_PATH_CMAKE_DIR="$QT_HOST_PATH/lib/x86_64-linux-gnu/cmake"
else
    QT_HOST_PATH_CMAKE_DIR="$QT_HOST_PATH/lib/cmake"
fi

for required in "$QT_ANDROID/lib/cmake/Qt6/qt.toolchain.cmake" "$QT_HOST_PATH_CMAKE_DIR/Qt6HostInfo" \
                "$ANDROID_SDK_ROOT/platform-tools" "${ANDROID_NDK_ROOT:-/nonexistent}/ndk-build" "$JAVA_HOME/bin/javac"; do
    if [ ! -e "$required" ]; then
        echo "missing: $required" >&2
        echo "(README → \"The phone app\" lists what to install)" >&2
        exit 1
    fi
done
export JAVA_HOME ANDROID_SDK_ROOT ANDROID_NDK_ROOT

cmake -S android -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$QT_ANDROID/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DQT_HOST_PATH="$QT_HOST_PATH" -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_PATH_CMAKE_DIR" \
    -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build "$BUILD_DIR" --target nexa-phone_make_apk

apk="$(find "$BUILD_DIR" -name '*.apk' -newer "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
echo "APK: $apk"
echo "install with: adb install -r \"$apk\""
