<#
.SYNOPSIS
Prune obsolete generated APK replicas, retaining evidence and usable rollbacks.
.DESCRIPTION
Dry-run by default. -Apply deletes only planned top-level APK files under
build/nonroot; reports, logs, screenshots, keys and nested directories remain.
Update build-retention.json pinnedRuns before replacing a current/rollback run.
Use complete run directory names, not paths; keep old rollback pins until the
replacement has passed device checks. Run again without -Apply to review changes.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$RootedOnly,
    [ValidateRange(3, 100)][int]$KeepLatest = 3,
    [ValidateRange(30, 525600)][int]$MinAgeMinutes = 30
)

if ($RootedOnly) {
    & (Join-Path $PSScriptRoot 'cleanup-rooted.ps1') -Apply:$Apply -KeepLatest $KeepLatest -MinAgeMinutes $MinAgeMinutes
    return
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$runPattern = '^\d{8}-\d{6}-[0-9a-f]{8}$'
$apkPattern = '^(base(-aligned|-unsigned)?|split-[0-9]+(-aligned|-unsigned)?|input-[0-9]+)\.apk$'
$signedPattern = '^(base|split-[0-9]+)\.apk$'

function Assert-PlainPath([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path)
    $rootFull = [IO.Path]::GetFullPath($Root)
    if ($rootFull.Length -gt ([IO.Path]::GetPathRoot($rootFull)).Length) { $rootFull = $rootFull.TrimEnd('\') }
    if (-not ($full.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($rootFull.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "Path is outside the allowed root: $full"
    }
    # Check each existing path component, including the boundary directory.
    $cursor = $full
    while ($true) {
        $item = Get-Item -LiteralPath $cursor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not allowed: $cursor"
        }
        if ($cursor.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase)) { break }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    $resolved = (Resolve-Path -LiteralPath $full).ProviderPath
    if (-not $resolved.Equals($full, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Resolved path changed: $full -> $resolved"
    }
    return $resolved
}

function Get-Property($Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

$project = [IO.Path]::GetFullPath($PSScriptRoot)
# Also reject links in ancestors of the project root.
$null = Assert-PlainPath $project ([IO.Path]::GetPathRoot($project))
$build = Assert-PlainPath (Join-Path $project 'build') $project
$nonroot = Assert-PlainPath (Join-Path $build 'nonroot') $project
$retentionPath = Assert-PlainPath (Join-Path $project 'build-retention.json') $project
$retentionHash = (Get-FileHash -LiteralPath $retentionPath -Algorithm SHA256).Hash.ToLowerInvariant()
$retention = Get-Content -LiteralPath $retentionPath -Raw | ConvertFrom-Json
if ((Get-Property $retention 'schema') -ne 1 -or $null -eq (Get-Property $retention 'pinnedRuns')) {
    throw 'Invalid retention configuration; expected schema 1 and pinnedRuns.'
}
$pins = @{}
foreach ($pin in $retention.pinnedRuns) {
    $name = [string](Get-Property $pin 'run')
    $reason = [string](Get-Property $pin 'reason')
    if ($name -notmatch $runPattern -or [string]::IsNullOrWhiteSpace($reason) -or $pins.ContainsKey($name)) {
        throw "Invalid or duplicate retention pin: $name"
    }
    $null = Assert-PlainPath (Join-Path $nonroot $name) $project
    $pins[$name] = $reason
}
if ($pins.Count -eq 0) { throw 'At least one current/rollback pin is required.' }

$started = [DateTime]::UtcNow
$cutoff = $started.AddMinutes(-$MinAgeMinutes)
$runs = @()
foreach ($directory in Get-ChildItem -LiteralPath $nonroot -Directory) {
    $null = Assert-PlainPath $directory.FullName $project
    $row = [pscustomobject]@{
        run = $directory.Name; path = $directory.FullName; group = $null
        completed = $false; restorable = $false; retained = $false
        reason = ''; receiptSha256 = $null; newestWriteUtc = $directory.LastWriteTimeUtc
    }
    $runs += $row
    if ($directory.Name -notmatch $runPattern) { $row.reason = 'Unrecognized run name'; continue }
    $reportPath = Join-Path $directory.FullName 'report.json'
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { $row.reason = 'No completed report'; continue }
    $null = Assert-PlainPath $reportPath $project
    try {
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $package = [string](Get-Property $report 'package')
        $abi = [string](Get-Property $report 'runtime_abi')
        if (-not $abi) {
            $runtime = @(Get-Property $report 'runtime')
            if ($runtime.Count -eq 1 -and $null -ne $runtime[0]) { $abi = [string](Get-Property $runtime[0] 'abi') }
            elseif ((Get-Property $report 'mode') -eq 'panel-only') { $abi = 'panel-only' }
        }
        $outputs = @(Get-Property $report 'outputs')
        if ($package -notin @('dev.pgrazor.preview', 'com.nianticlabs.pokemongo') -or
            $abi -notin @('arm64-v8a', 'x86_64', 'auto', 'panel-only') -or $outputs.Count -eq 0) {
            throw 'Missing/unsupported package, runtime ABI or outputs'
        }
        if ($abi -eq 'auto') {
            $bundledAbis = @(Get-Property $report 'runtime_abis')
            if ($bundledAbis.Count -ne 2 -or 'arm64-v8a' -notin $bundledAbis -or 'x86_64' -notin $bundledAbis) { throw 'Incomplete unified runtime receipt' }
        }
        $restorable = $true
        foreach ($output in $outputs) {
            $outputPath = [IO.Path]::GetFullPath([string](Get-Property $output 'file'))
            if (-not ([IO.Path]::GetDirectoryName($outputPath)).Equals($directory.FullName, [StringComparison]::OrdinalIgnoreCase) -or
                [IO.Path]::GetFileName($outputPath) -notmatch $signedPattern -or
                [string](Get-Property $output 'sha256') -notmatch '^[0-9a-fA-F]{64}$') {
                throw 'Output receipt does not describe direct signed APKs in this run'
            }
            if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) { $restorable = $false }
            else { $null = Assert-PlainPath $outputPath $project }
        }
        $row.completed = $true; $row.restorable = $restorable; $row.group = "$package/$abi"
        $row.receiptSha256 = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash.ToLowerInvariant()
    } catch { $row.reason = 'Invalid report: ' + $_.Exception.Message; continue }
    # A recent report/APK makes the entire run ineligible, even if its directory
    # timestamp is old. Small logs may continue growing and are never deleted.
    foreach ($file in Get-ChildItem -LiteralPath $directory.FullName -File) {
        if ($file.Name -eq 'report.json' -or $file.Name -match $apkPattern) {
            $null = Assert-PlainPath $file.FullName $project
            if ($file.LastWriteTimeUtc -gt $row.newestWriteUtc) { $row.newestWriteUtc = $file.LastWriteTimeUtc }
        }
    }
}

foreach ($group in @($runs | Where-Object { $_.completed -and $_.restorable } | Group-Object group)) {
    foreach ($run in @($group.Group | Sort-Object run -Descending | Select-Object -First $KeepLatest)) {
        $run.retained = $true; $run.reason = "Latest $KeepLatest complete APK sets for $($run.group)"
    }
}
foreach ($run in $runs) {
    if ($pins.ContainsKey($run.run)) { $run.retained = $true; $run.reason = 'Pinned: ' + $pins[$run.run] }
    elseif (-not $run.completed) { $run.retained = $true }
    elseif ($run.newestWriteUtc -gt $cutoff) { $run.retained = $true; $run.reason = "Modified within $MinAgeMinutes minutes" }
}

$entries = @()
foreach ($run in @($runs | Where-Object { -not $_.retained })) {
    foreach ($file in Get-ChildItem -LiteralPath $run.path -File) {
        if ($file.Name -notmatch $apkPattern) { continue }
        $checked = Assert-PlainPath $file.FullName $project
        $entries += [pscustomobject]@{
            path = $checked; run = $run.run; bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $checked -Algorithm SHA256).Hash.ToLowerInvariant()
            lastWriteUtc = $file.LastWriteTimeUtc.ToString('o')
            status = $(if ($Apply) { 'planned' } else { 'dry-run' }); error = $null
        }
    }
}
$receiptDirectory = Join-Path $build 'cleanup'
if (-not (Test-Path -LiteralPath $receiptDirectory)) { $null = New-Item -ItemType Directory -Path $receiptDirectory }
$null = Assert-PlainPath $receiptDirectory $project
$receiptPath = Join-Path $receiptDirectory ($started.ToString('yyyyMMdd-HHmmss-fffffff') + '.json')
$receipt = [ordered]@{
    schema = 1; startedUtc = $started.ToString('o'); apply = [bool]$Apply
    root = $nonroot; retentionFile = $retentionPath; retentionSha256 = $retentionHash
    keepLatest = $KeepLatest; minAgeMinutes = $MinAgeMinutes
    status = $(if ($Apply) { 'applying' } else { 'dry-run' })
    runs = $runs; files = $entries; deletedBytes = 0
}
function Save-Receipt { $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8 }
Save-Receipt
if ($Apply) {
    try {
        foreach ($entry in $entries) {
            if ((Get-FileHash -LiteralPath $retentionPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $retentionHash) {
                throw 'Retention pins changed during cleanup; rerun to review the new plan.'
            }
            $run = @($runs | Where-Object { $_.run -eq $entry.run })[0]
            $reportPath = Assert-PlainPath (Join-Path $run.path 'report.json') $project
            if ((Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $run.receiptSha256) {
                throw "Build report changed during cleanup: $reportPath"
            }
            $checked = Assert-PlainPath $entry.path $project
            $current = Get-Item -LiteralPath $checked
            if ($current.Name -notmatch $apkPattern -or $current.PSIsContainer -or
                -not $current.DirectoryName.Equals($run.path, [StringComparison]::OrdinalIgnoreCase) -or
                $current.Length -ne $entry.bytes -or $current.LastWriteTimeUtc.ToString('o') -ne $entry.lastWriteUtc -or
                $current.LastWriteTimeUtc -gt $cutoff -or
                (Get-FileHash -LiteralPath $checked -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
                $entry.status = 'skipped-changed'; Save-Receipt; continue
            }
            # Deliberately file-only, literal, and non-recursive.
            Remove-Item -LiteralPath $checked -ErrorAction Stop
            $entry.status = 'deleted'; $receipt.deletedBytes += $entry.bytes
            Save-Receipt
        }
        $receipt.status = 'complete'
    } catch {
        $receipt.status = 'stopped-on-error'
        if ($null -ne $entry) { $entry.status = 'error'; $entry.error = $_.Exception.Message }
        throw
    } finally { Save-Receipt }
}
[long]$total = 0
foreach ($entry in $entries) { $total += $entry.bytes }
Write-Output ("{0}: {1} APK files; {2:N3} GiB selected; {3:N3} GiB deleted." -f $(if ($Apply) {'Applied'} else {'Dry run'}), $entries.Count, ($total/1GB), ($receipt.deletedBytes/1GB))
Write-Output ("Protected {0} of {1} run directories. Receipt: {2}" -f @($runs | Where-Object retained).Count, $runs.Count, $receiptPath)
