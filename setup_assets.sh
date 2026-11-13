#!/bin/sh
# Copies the game data and the original engine library out of YOUR Tamagotchi Classic APK into this project.
# Usage: ./setup_assets.sh path/to/TamagotchiClassic.apk   (a .zip copy of the APK works too)
set -e
APK="$1"
if [ -z "$APK" ] || [ ! -f "$APK" ]; then echo "usage: $0 path/to/tamagotchi_classic.apk"; exit 1; fi
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/app/src/main/assets"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Info-ZIP unzip exits with 1 for harmless warnings and 2+ for format quirks that real APKs sometimes have;
# in that case fall back to Python's more tolerant zip reader.
unzip -q -o "$APK" -d "$TMP" 2>/dev/null && RC=0 || RC=$?
if [ "$RC" -gt 1 ]; then
  echo "unzip reported code $RC, retrying with python3..."
  command -v python3 >/dev/null 2>&1 || { echo "error: cannot read the APK (unzip code $RC) and python3 is not installed"; exit 1; }
  rm -rf "$TMP" && mkdir -p "$TMP"
  python3 - "$APK" "$TMP" <<'PY'
import sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as z:
    for info in z.infolist():
        try:
            z.extract(info, dst)
        except Exception as e:
            print("skipped", info.filename, e)
PY
fi

[ -d "$TMP/assets" ] || { echo "error: no assets/ folder in the APK"; exit 1; }
SO="$(find "$TMP/lib" -name libAndroidGame001JNI.so 2>/dev/null | head -n 1)"
[ -n "$SO" ] || { echo "error: libAndroidGame001JNI.so not found in the APK"; exit 1; }

rm -rf "$DEST"; mkdir -p "$DEST/engine"
cp -R "$TMP/assets/." "$DEST/"
cp "$SO" "$DEST/engine/engine.bin"      # loaded by the emulator, never by Android itself
echo "game data : $(find "$DEST" -type f | wc -l) files"
echo "engine    : $(wc -c < "$DEST/engine/engine.bin") bytes (from $(basename "$(dirname "$SO")"))"

# Optional: use the original launcher icon
ICON="$(find "$TMP/res" -name 'icon.png' 2>/dev/null | head -n 1)"
if [ -n "$ICON" ]; then
  mkdir -p "$HERE/app/src/main/res/drawable-nodpi"
  cp "$ICON" "$HERE/app/src/main/res/drawable-nodpi/ic_launcher_art.png"
  cat > "$HERE/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@android:color/white"/>
    <foreground><inset android:drawable="@drawable/ic_launcher_art" android:inset="20%"/></foreground>
</adaptive-icon>
XML
  echo "icon      : installed"
fi
echo "done. Open the project in Android Studio (or run ./gradlew assembleDebug)."
