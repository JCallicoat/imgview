#!/usr/bin/env bash
# Turn a built imgview.app into a self-contained, ad-hoc signed .dmg.
#
#   make-dmg.sh <path/to/imgview.app> <output.dmg> [expected-archs]
#
# expected-archs defaults to "arm64 x86_64": every Mach-O file in the bundle
# must contain all of them, so a non-universal Qt or Exiv2 is caught here
# rather than by a user on the other kind of Mac.
#
# Needs macdeployqt on PATH (from the Qt install used for the build).
set -euo pipefail

app=${1:?usage: make-dmg.sh <imgview.app> <output.dmg> [expected-archs]}
dmg=${2:?usage: make-dmg.sh <imgview.app> <output.dmg> [expected-archs]}
expected_archs=${3:-"arm64 x86_64"}

# 1. Copy Qt's frameworks and plugins (incl. image formats) into the bundle.
macdeployqt "$app" -always-overwrite -verbose=1

# 2. Check every binary in the bundle has every expected architecture.
missing=0
while IFS= read -r -d '' f; do
  if file "$f" | grep -q 'Mach-O'; then
    archs=$(lipo -archs "$f")
    for a in $expected_archs; do
      if [[ " $archs " != *" $a "* ]]; then
        echo "error: $f lacks $a (has: $archs)" >&2
        missing=1
      fi
    done
  fi
done < <(find "$app" -type f -print0)
if (( missing )); then
  exit 1
fi
echo "All binaries in $app contain: $expected_archs"

# 3. Ad-hoc sign (no Apple Developer ID). macdeployqt rewrote library paths,
#    which invalidates existing signatures, and Apple Silicon refuses to run
#    unsigned code.
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

# 4. Disk image with the app and an Applications shortcut to drag it onto.
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
cp -R "$app" "$staging/"
ln -s /Applications "$staging/Applications"
rm -f "$dmg"
hdiutil create -volname "imgview" -srcfolder "$staging" -fs HFS+ \
  -format UDZO -ov "$dmg"
echo "Created $dmg"
