#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export GRADLE_USER_HOME="$project_root/.local/gradle"
cd "$project_root/android"
./gradlew --no-daemon --console=plain :app:testDebugUnitTest :app:assembleDebug
mkdir -p "$project_root/build/apk"
cp app/build/outputs/apk/debug/app-debug.apk "$project_root/build/apk/go3s-bridge-debug.apk"
printf '%s\n' "APK: $project_root/build/apk/go3s-bridge-debug.apk"
