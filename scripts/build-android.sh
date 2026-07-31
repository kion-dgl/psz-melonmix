#!/usr/bin/env bash
# Build the Android APK with the PSZ overlay composited into the top screen.
# Needs ANDROID_HOME (SDK + NDK) and a JDK; CI provides both.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${PSZ_WORK:-$ROOT/build}/melonDS-android"
[ -d "$DIR" ] || "$ROOT/scripts/bootstrap.sh" android
: "${ANDROID_HOME:?set ANDROID_HOME to your Android SDK}"
echo "sdk.dir=$ANDROID_HOME" > "$DIR/local.properties"
chmod +x "$DIR/gradlew"
( cd "$DIR" && ./gradlew :app:assembleGitHubProdDebug --no-daemon )
find "$DIR/app/build/outputs" -name "*.apk" -print
