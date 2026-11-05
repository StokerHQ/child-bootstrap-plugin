#!/usr/bin/env sh
# fetch-sdk.sh [stoker-plugin.yaml] - puts the Stoker plugin SDK a project pins into a local cache and prints the
# SDK's folder (include/, cmake/, msbuild/, build/, sdk.json). The pin is `sdk: { version, sha256 }`, or
# STOKER_PLUGIN_SDK_VERSION and STOKER_PLUGIN_SDK_SHA256. STOKER_PLUGIN_SDK, when set, is a local folder used as it
# is, or a local zip that must still match the pinned SHA-256 - fetch-sdk.ps1 does the same on Windows.
set -eu

project="${1:-stoker-plugin.yaml}"
version="${STOKER_PLUGIN_SDK_VERSION:-}"
sha256="${STOKER_PLUGIN_SDK_SHA256:-}"
if [ -z "$version" ] || [ -z "$sha256" ]; then
    [ -f "$project" ] || { echo "fetch-sdk: no $project, and no STOKER_PLUGIN_SDK_VERSION/_SHA256" >&2; exit 1; }
    block="$(sed -n '/^sdk:/,/^[^ ]/p' "$project")"
    [ -n "$version" ] || version="$(printf '%s\n' "$block" | sed -n 's/.*version:[[:space:]]*"\{0,1\}\([0-9A-Za-z.-]*\)"\{0,1\}.*/\1/p' | head -n 1)"
    [ -n "$sha256" ] || sha256="$(printf '%s\n' "$block" | sed -n 's/.*sha256:[[:space:]]*"\{0,1\}\([0-9a-fA-F]\{64\}\)"\{0,1\}.*/\1/p' | head -n 1)"
fi
[ -n "$version" ] && [ -n "$sha256" ] || { echo "fetch-sdk: $project pins no sdk { version, sha256 }" >&2; exit 1; }
sha256="$(printf '%s' "$sha256" | tr 'A-F' 'a-f')"

if [ -n "${STOKER_PLUGIN_SDK:-}" ] && [ -d "$STOKER_PLUGIN_SDK" ]; then
    [ -f "$STOKER_PLUGIN_SDK/sdk.json" ] || { echo "fetch-sdk: STOKER_PLUGIN_SDK holds no sdk.json" >&2; exit 1; }
    (cd "$STOKER_PLUGIN_SDK" && pwd)
    exit 0
fi

cache="${STOKER_PLUGIN_SDK_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/stokerhq/plugin-sdk}"
target="$cache/$version-$(printf '%s' "$sha256" | cut -c1-12)"
if [ -f "$target/sdk.json" ]; then echo "$target"; exit 0; fi

mkdir -p "$cache"
zip="$(mktemp "$cache/stoker-plugin-sdk-$version.XXXXXX")"
staging="$(mktemp -d "$cache/.staging.XXXXXX")"
trap 'rm -f "$zip"; rm -rf "$staging"' EXIT
if [ -n "${STOKER_PLUGIN_SDK:-}" ]; then
    cp "$STOKER_PLUGIN_SDK" "$zip"
else
    curl -fsSL "https://github.com/StokerHQ/plugin-sdk/releases/download/v$version/stoker-plugin-sdk-$version.zip" -o "$zip"
fi
actual="$(sha256sum "$zip" | cut -d' ' -f1)"
[ "$actual" = "$sha256" ] || { echo "fetch-sdk: stoker-plugin-sdk-$version.zip has SHA-256 $actual, not the pinned $sha256" >&2; exit 1; }
unzip -q "$zip" -d "$staging"
if [ -d "$target" ]; then rm -rf "$staging"; else mv "$staging" "$target"; fi
echo "$target"
