<#
.SYNOPSIS
Puts the Stoker plugin SDK a project pins into a local cache and prints the SDK's folder.

.DESCRIPTION
Reads the pin - `sdk: { version, sha256 }` from stoker-plugin.yaml, or -Version and -Sha256 - downloads
stoker-plugin-sdk-<version>.zip from the SDK's GitHub release, checks its SHA-256, and unpacks it once into
<cache>/<version>-<sha256[0:12]>. The printed folder holds include/, cmake/, msbuild/, build/ and sdk.json.

STOKER_PLUGIN_SDK, when set, is used instead of a download: a folder (an SDK checkout's native/, or an unpacked zip)
taken as it is, or a zip, which must still match the pinned SHA-256. That is how an SDK change is built against
before it is released.

The cache is %LOCALAPPDATA%\StokerHQ\plugin-sdk on Windows, $XDG_CACHE_HOME/stokerhq/plugin-sdk (else
~/.cache/stokerhq/plugin-sdk) elsewhere, or -Cache.
#>
param(
    [string]$Project = 'stoker-plugin.yaml',
    [string]$Version,
    [string]$Sha256,
    [string]$Cache
)
$ErrorActionPreference = 'Stop'

if (!$Version -or !$Sha256) {
    if (!(Test-Path $Project)) { throw "No $Project, and no -Version/-Sha256 to fetch." }
    $text = Get-Content $Project -Raw
    $block = [regex]::Match($text, '(?ms)^sdk:\s*\{?(.*?)(?:^\S|\z)').Groups[1].Value
    if (!$Version) { $Version = [regex]::Match($block, 'version:\s*"?([0-9A-Za-z.\-]+)"?').Groups[1].Value }
    if (!$Sha256) { $Sha256 = [regex]::Match($block, 'sha256:\s*"?([0-9a-fA-F]{64})"?').Groups[1].Value }
    if (!$Version -or !$Sha256) { throw "$Project pins no sdk { version, sha256 }." }
}
$Sha256 = $Sha256.ToLowerInvariant()

if ($env:STOKER_PLUGIN_SDK -and (Test-Path $env:STOKER_PLUGIN_SDK -PathType Container)) {
    $folder = (Resolve-Path $env:STOKER_PLUGIN_SDK).Path
    if (!(Test-Path (Join-Path $folder 'sdk.json'))) { throw "STOKER_PLUGIN_SDK ($folder) holds no sdk.json." }
    $local = (Get-Content (Join-Path $folder 'sdk.json') -Raw | ConvertFrom-Json).version
    if ($local -ne $Version) { Write-Warning "STOKER_PLUGIN_SDK is SDK $local; the project pins $Version." }
    return $folder
}

if (!$Cache) {
    $Cache = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'StokerHQ/plugin-sdk' }
             elseif ($env:XDG_CACHE_HOME) { Join-Path $env:XDG_CACHE_HOME 'stokerhq/plugin-sdk' }
             else { Join-Path $HOME '.cache/stokerhq/plugin-sdk' }
}
$target = Join-Path $Cache "$Version-$($Sha256.Substring(0, 12))"
if (Test-Path (Join-Path $target 'sdk.json')) { return (Resolve-Path $target).Path }

New-Item -ItemType Directory -Force $Cache | Out-Null
$zip = Join-Path $Cache "stoker-plugin-sdk-$Version.$([Guid]::NewGuid().ToString('N')).zip"
try {
    if ($env:STOKER_PLUGIN_SDK) {
        Copy-Item -LiteralPath $env:STOKER_PLUGIN_SDK $zip
    } else {
        $url = "https://github.com/StokerHQ/plugin-sdk/releases/download/v$Version/stoker-plugin-sdk-$Version.zip"
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "stoker-plugin-sdk-$Version.zip has SHA-256 $actual, not the pinned $Sha256." }
    $staging = "$target.$([Guid]::NewGuid().ToString('N'))"
    Expand-Archive -LiteralPath $zip -DestinationPath $staging
    if (Test-Path $target) { Remove-Item -Recurse -Force $staging } else { Move-Item $staging $target }
} finally {
    Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
}
(Resolve-Path $target).Path
