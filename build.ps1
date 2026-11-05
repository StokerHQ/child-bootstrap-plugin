<#
.SYNOPSIS
Builds every target stoker-plugin.yaml lists into build\<architecture>.

.DESCRIPTION
Fetches the Stoker plugin SDK release stoker-plugin.yaml pins (scripts\fetch-sdk.ps1: downloaded once, checked by
SHA-256, cached), then builds each Windows target with the SDK's helper. Needs Visual Studio with the C++ workload,
found through vswhere. STOKER_PLUGIN_SDK names a local SDK folder or zip instead of the release.
#>
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$sdk = & (Join-Path $root 'scripts\fetch-sdk.ps1') -Project (Join-Path $root 'stoker-plugin.yaml')
$targets = @(Get-Content (Join-Path $root 'stoker-plugin.yaml') |
    Where-Object { $_ -match '^\s*-\s*\{\s*os:\s*windows' } |
    ForEach-Object { if ($_ -match 'architecture:\s*(\w+)') { $Matches[1] } })
foreach ($architecture in $targets) {
    $platform = if ($architecture -eq 'x86') { 'Win32' } else { 'x64' }
    $dll = & (Join-Path $sdk 'build\Build-HookPlugin.ps1') `
        -Source (Join-Path $root 'src\child_bootstrap.cpp') `
        -Name 'child-bootstrap' -Platform $platform -Out (Join-Path $root "build\$architecture")
    Write-Host "Built $dll"
}