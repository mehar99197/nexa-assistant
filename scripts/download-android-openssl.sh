#!/usr/bin/env bash
# Fetches OpenSSL 3 built for Android — libcrypto_3.so and libssl_3.so — which the phone app's APK
# has to carry: Qt for Android ships its OpenSSL TLS plugin but not OpenSSL itself, and the plugin
# loads the two by exactly those names. Without them every https:// and wss:// address fails in the
# app with "TLS initialization failed", and only plain http://localhost through adb works.
#
# The libraries are KDAB's prebuilt ones (github.com/KDAB/android_openssl, under the OpenSSL
# licence), taken at a fixed commit — a sparse clone of just the ABI's directory, a few MB rather
# than the whole repository — into ~/Android/android_openssl/ssl_3/<abi>/, and checked: ELF files
# for that chip, and for arm64-v8a the SHA-256 the pinned commit holds. A full clone of the
# repository there is used as it is (NEXA_ANDROID_OPENSSL_DIR for cmake, ANDROID_OPENSSL_DIR for
# build-phone-apk.sh). Usage: download-android-openssl.sh [abi…] — default arm64-v8a; also
# armeabi-v7a, x86, x86_64.
set -euo pipefail

PINNED="b71f1470962019bd89534a2919f5925f93bc5779"      # master on 2026-10-11: OpenSSL 3.x for all four ABIs
COMMIT="${ANDROID_OPENSSL_COMMIT:-$PINNED}"
DEST="${ANDROID_OPENSSL_DIR:-$HOME/Android/android_openssl}"
REPO="https://github.com/KDAB/android_openssl"
ABIS=("$@")
[ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a)

# What the pinned commit's files hash to, where known; a file that differs is refused.
sha256_for() {
    case "$1" in
    arm64-v8a/libcrypto_3.so) echo "1e6c12ae0c2dadfe9d178d7f80f0ab248a1877a234066098bf5594e5205e5740" ;;
    arm64-v8a/libssl_3.so) echo "01d2bd0baac626efd3309f35f99c4b826dd9b885a7e4d14d5b12b3603d3a407f" ;;
    *) echo "" ;;
    esac
}

# ELF machine codes, so a library for another chip (or no library at all) is refused.
machine_for() {
    case "$1" in
    arm64-v8a) echo "b7 00" ;;
    armeabi-v7a) echo "28 00" ;;
    x86) echo "03 00" ;;
    x86_64) echo "3e 00" ;;
    *) echo "" ;;
    esac
}

check() {
    local file="$1" abi="$2" name="$3"
    [ -s "$file" ] || { echo "$REPO has no $name for $abi at commit $COMMIT" >&2; return 1; }
    [ "$(head -c 4 "$file" | od -An -c | tr -d ' ')" = "177ELF" ] || { echo "$file is not an ELF file" >&2; return 1; }
    local want; want="$(machine_for "$abi")"
    if [ -n "$want" ] && [ "$(od -An -tx1 -j18 -N2 "$file" | tr -s ' ' | sed 's/^ //')" != "$want" ]; then
        echo "$file is not built for $abi" >&2
        return 1
    fi
    local sum; sum="$(sha256_for "$abi/$name")"
    if [ "$COMMIT" = "$PINNED" ] && [[ -n "$sum" && "$sum" != @* ]] \
        && [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
        echo "$file does not hash to what commit $COMMIT holds" >&2
        return 1
    fi
}

if [ -d "$DEST/.git" ] && [ "$(git -C "$DEST" config --get core.sparseCheckout || true)" != "true" ]; then
    echo "$DEST is a full clone of $REPO; using it as it is"
else
    if [ ! -d "$DEST/.git" ]; then
        if [ -e "$DEST" ] && [ -n "$(ls -A "$DEST")" ]; then
            echo "$DEST exists and is not a clone of $REPO: move it away, or set ANDROID_OPENSSL_DIR" >&2
            exit 1
        fi
        # Nothing checked out yet, and blobs fetched only for what gets checked out: the libraries
        # asked for, not every ABI and OpenSSL version the repository keeps.
        git clone --no-checkout --depth 1 --filter=blob:none "$REPO" "$DEST"
    fi
    paths=()
    for abi in "${ABIS[@]}"; do paths+=("ssl_3/$abi"); done
    git -C "$DEST" sparse-checkout set "${paths[@]}"
    git -C "$DEST" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$DEST" fetch --depth 1 origin "$COMMIT"
    git -C "$DEST" checkout --quiet --detach "$COMMIT"
fi

for abi in "${ABIS[@]}"; do
    for lib in libcrypto_3.so libssl_3.so; do
        check "$DEST/ssl_3/$abi/$lib" "$abi" "$lib"
    done
done
echo "OpenSSL for Android is in $DEST/ssl_3 (${ABIS[*]}), commit ${COMMIT:0:12}; scripts/build-phone-apk.sh finds it there."
