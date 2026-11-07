# File-only cleanup of completed rooted package runs. Originals, keys, reports,
# native build trees, and pinned/current installer and module outputs are kept.
[CmdletBinding()]
param([switch]$Apply, [ValidateRange(3,100)][int]$KeepLatest=3,
      [ValidateRange(30,525600)][int]$MinAgeMinutes=30)
$ErrorActionPreference='Stop'
Set-StrictMode -Version 2
$project=[IO.Path]::GetFullPath($PSScriptRoot)
$root=Join-Path $project 'build/rooted'
if (!(Test-Path -LiteralPath $root)) { return }
function Checked([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($project+'\',[StringComparison]::OrdinalIgnoreCase)) { throw "Outside project: $full" }
    $cursor=$full
    while ($cursor) {
        $item=Get-Item -LiteralPath $cursor -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path: $cursor" }
        $cursor=[IO.Path]::GetDirectoryName($cursor)
    }
    if ((Resolve-Path -LiteralPath $full).ProviderPath -ne $full) { throw "Resolved path changed: $full" }
    return $full
}
$null=Checked $root
$pins=@{}
$retention=Join-Path $project 'build-retention.json'
$retentionHash=$null
if (Test-Path -LiteralPath $retention) {
    $null=Checked $retention
    $retentionHash=(Get-FileHash -LiteralPath $retention).Hash
    $config=Get-Content -LiteralPath $retention -Raw | ConvertFrom-Json
    if ($config.PSObject.Properties['pinnedRootedRuns']) {
        foreach ($pin in $config.pinnedRootedRuns) {
            if ($pin.run -notmatch '^\d{8}-\d{6}-[0-9a-f]{8}$' -or !$pin.reason) { throw 'Invalid rooted retention pin' }
            $pins[$pin.run]=$true
        }
    }
}
$runs=@(Get-ChildItem -LiteralPath $root -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}-[0-9a-f]{8}$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'report.json')) } | Sort-Object Name -Descending)
$latest=@($runs | Select-Object -First $KeepLatest -ExpandProperty Name)
$cutoff=[DateTime]::UtcNow.AddMinutes(-$MinAgeMinutes)
$entries=@()
foreach ($run in $runs) {
    if ($run.Name -notmatch '^\d{8}-\d{6}-[0-9a-f]{8}$') { continue }
    $reportPath=Join-Path (Checked $run.FullName) 'report.json'
    if (!(Test-Path -LiteralPath $reportPath)) { continue }
    $null=Checked $reportPath
    $reportHash=(Get-FileHash -LiteralPath $reportPath).Hash
    $report=Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if (!$report.PSObject.Properties['module'] -or !$report.PSObject.Properties['module_sha256']) { continue }
    $module=[IO.Path]::GetFullPath($report.module)
    if ([IO.Path]::GetDirectoryName($module) -ne $run.FullName -or !(Test-Path -LiteralPath $module)) { continue }
    $null=Checked $module
    if ((Get-FileHash -LiteralPath $module).Hash -ne $report.module_sha256) { throw "Module checksum mismatch: $module" }
    $protected=$pins.ContainsKey($run.Name) -or $latest -contains $run.Name
    foreach ($file in Get-ChildItem -LiteralPath $run.FullName -File) {
        # Input APKs are packaging copies: every complete module is self-contained.
        $inputCopy=$file.Name -match '^input-[0-9]+\.apk$'
        $obsoleteOutput=!$protected -and $file.LastWriteTimeUtc -lt $cutoff -and
            $file.Name -match '^(PGRazor-Root(-module-[A-Za-z0-9.-]+\.zip|(-[A-Za-z0-9.-]+)?\.apk)|installer-(unsigned|aligned)\.apk|payload\.zip|loader-(arm64-v8a|x86_64)\.so)$'
        if (!$inputCopy -and !$obsoleteOutput) { continue }
        $entries+= [pscustomobject]@{path=(Checked $file.FullName); bytes=$file.Length;
            sha256=(Get-FileHash -LiteralPath $file.FullName).Hash; report=$reportPath;
            reportHash=$reportHash; status='planned'}
    }
}
$receiptDir=Join-Path $project 'build/cleanup'
$null=New-Item -ItemType Directory -Force -Path $receiptDir
$null=Checked $receiptDir
$receiptPath=Join-Path $receiptDir ('rooted-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')+'.json')
$receipt=[ordered]@{schema=1; apply=[bool]$Apply; root=$root; files=$entries; deletedBytes=0; status='planned'}
function Save { $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding utf8 }
Save
try {
    if ($Apply) {
        foreach ($entry in $entries) {
            if ($retentionHash -and (Get-FileHash -LiteralPath (Checked $retention)).Hash -ne $retentionHash) { throw 'Retention changed' }
            if ((Get-FileHash -LiteralPath (Checked $entry.report)).Hash -ne $entry.reportHash) { throw 'Report changed' }
            $path=Checked $entry.path
            if ((Get-FileHash -LiteralPath $path).Hash -ne $entry.sha256) { throw 'Output changed' }
            Remove-Item -LiteralPath $path
            $entry.status='deleted'; $receipt.deletedBytes+=$entry.bytes; Save
        }
    }
    $receipt.status=$(if ($Apply) {'complete'} else {'dry-run'})
} catch { $receipt.status='failed'; throw } finally { Save }
Write-Output ("Rooted cleanup: {0} files selected; {1:N3} GiB reclaimed. Receipt: {2}" -f $entries.Count,($receipt.deletedBytes/1GB),$receiptPath)
