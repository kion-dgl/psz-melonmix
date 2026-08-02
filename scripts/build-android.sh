#!/usr/bin/env bash
# Build the Android APK with the PSZ overlay composited into the top screen.
# Needs ANDROID_HOME (SDK + NDK) and a JDK; CI provides both.
#
# Signing. Upstream already reads its keystore out of local.properties, so this
# only has to fill those in when the environment supplies them:
#
#   PSZ_KEYSTORE_B64        base64 of the .keystore file  (CI secret)
#   PSZ_KEYSTORE_PASSWORD   store password
#   PSZ_KEY_ALIAS           key alias      (default: psz-melonmix)
#   PSZ_KEY_PASSWORD        key password   (default: same as the store password)
#
# With those set it builds a SIGNED RELEASE; without them, a debug APK. Debug is
# still installable, it just carries a throwaway signature — so consecutive debug
# builds cannot update each other in place.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${PSZ_WORK:-$ROOT/build}/melonDS-android"
"$ROOT/scripts/bootstrap.sh" ensure-android
: "${ANDROID_HOME:?set ANDROID_HOME to your Android SDK}"

PROPS="$DIR/local.properties"
echo "sdk.dir=$ANDROID_HOME" > "$PROPS"

TASK=":app:assembleGitHubProdDebug"
if [ -n "${PSZ_KEYSTORE_B64:-}" ]; then
    echo ">> signing configured — building release"
    KS="$DIR/psz-release.keystore"
    printf '%s' "$PSZ_KEYSTORE_B64" | base64 -d > "$KS"
    chmod 600 "$KS"
    {
        echo "MELONDS_KEYSTORE=$KS"
        echo "MELONDS_KEYSTORE_PASSWORD=${PSZ_KEYSTORE_PASSWORD:?keystore password required}"
        echo "MELONDS_KEY_ALIAS=${PSZ_KEY_ALIAS:-psz-melonmix}"
        echo "MELONDS_KEY_PASSWORD=${PSZ_KEY_PASSWORD:-$PSZ_KEYSTORE_PASSWORD}"
    } >> "$PROPS"
    TASK=":app:assembleGitHubProdRelease"
else
    echo ">> no keystore in the environment — building an unsigned debug APK"
fi

chmod +x "$DIR/gradlew"
( cd "$DIR" && ./gradlew "$TASK" --no-daemon )

# local.properties carries the store password in plain text and the keystore is
# a decoded secret; both have served their purpose and neither should be left
# lying in the work tree.
rm -f "$PROPS" "$DIR/psz-release.keystore"

find "$DIR/app/build/outputs" -name "*.apk" -print
