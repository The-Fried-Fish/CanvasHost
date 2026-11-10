#!/bin/bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 APP CONFIG BASE_ENTITLEMENTS OUTPUT_ENTITLEMENTS" >&2
  exit 2
fi

app=$1
config=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

/usr/bin/security cms -D -i "$config" -o "$work/config.plist"
bundle=$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "$app/Contents/Info.plist")
identifier=$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.application-identifier' "$work/config.plist")
team=$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.developer.team-identifier' "$work/config.plist")
if [ -z "$team" ] || [ "$identifier" != "$team.$bundle" ]; then
  echo "Signing configuration does not match $bundle" >&2
  exit 1
fi
if [ "$(/usr/libexec/PlistBuddy -c 'Print :Entitlements:com.apple.developer.hid.virtual.device' "$work/config.plist")" != true ]; then
  echo "Invalid signing configuration" >&2
  exit 1
fi
cp "$3" "$work/entitlements.plist"
/usr/libexec/PlistBuddy \
  -c "Add :com.apple.application-identifier string $identifier" \
  -c "Add :com.apple.developer.team-identifier string $team" \
  -c 'Add :com.apple.developer.hid.virtual.device bool true' \
  "$work/entitlements.plist"
cp "$work/entitlements.plist" "$4"
cp "$config" "$app/Contents/embedded.provisionprofile"
