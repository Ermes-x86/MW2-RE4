# MW2-RE4 uninstaller. Removes only files installed by MW2-RE4. Game saves, the MW2 installation and other
# RE4 mods are never touched.
param(
    [string]$Re4Dir,
    # Unattended: accept every default answer.
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Ask-YesNo([string]$question, [bool]$default = $true) {
    $hint = if ($default) { '[Y/n]' } else { '[y/N]' }
    if ($Yes) { Say "$question $hint -> $(if ($default) { 'yes' } else { 'no' })"; return $default }
    while ($true) {
        $answer = (Read-Host "$question $hint").Trim().ToLowerInvariant()
        if ($answer -eq '') { return $default }
        if ($answer -in 'y', 'yes', 's', 'sim') { return $true }
        if ($answer -in 'n', 'no', 'nao', 'não') { return $false }
    }
}
# The manifest is in RE4\Bin32; the RE4 folder or Bin32 itself may be given.
function Find-Manifest([string]$dir) {
    if (-not $dir) { return $null }
    foreach ($path in (Join-Path $dir 'Bin32\MW2RE4.install.json'), (Join-Path $dir 'MW2RE4.install.json')) {
        if (Test-Path $path) { return $path }
    }
    return $null
}
function Steam-Libraries {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $item = Get-ItemProperty $key -ErrorAction Stop
            foreach ($name in 'SteamPath', 'InstallPath') { if ($item.$name) { $roots += ($item.$name -replace '/', '\') } }
        } catch {}
    }
    $libraries = @()
    foreach ($root in ($roots | Select-Object -Unique)) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($match in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += ($match.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    return $libraries | Select-Object -Unique
}

Say 'MW2-RE4 uninstaller' Cyan
foreach ($name in 'bio4', 'iw4l-re4') {
    while (Get-Process -Name $name -ErrorAction SilentlyContinue) {
        Say "$name.exe is running. Close the game, then press Enter." Yellow
        if ($Yes) { throw "$name.exe is running; close it before uninstalling." }
        Read-Host | Out-Null
    }
}
$manifestPath = Find-Manifest $Re4Dir
if (-not $manifestPath -and -not $Re4Dir) {
    $manifestPath = Steam-Libraries | ForEach-Object { Find-Manifest (Join-Path $_ 'steamapps\common\Resident Evil 4') } |
        Where-Object { $_ } | Select-Object -First 1
    if ($manifestPath) {
        Say "Found MW2-RE4 in: $(Split-Path -Parent (Split-Path -Parent $manifestPath))"
        if (-not (Ask-YesNo 'Uninstall it from there?')) { $manifestPath = $null }
    }
}
while (-not $manifestPath) {
    if ($Yes) { throw 'MW2-RE4 install not found; pass -Re4Dir.' }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select the Resident Evil 4 folder where MW2-RE4 is installed'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'Uninstall cancelled.' }
    $manifestPath = Find-Manifest $dialog.SelectedPath
    if (-not $manifestPath) { Say 'MW2-RE4 is not installed in that folder.' Red }
}
$bin = Split-Path -Parent $manifestPath
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json

foreach ($file in 'MW2RE4.asi', 'MW2RE4.pdb', 'MW2RE4.log', 'MW2RE4.prev.log') {
    $path = Join-Path $bin $file
    if (Test-Path $path) { Remove-Item $path -Force; Say "Removed $file" }
}
$ini = Join-Path $bin 'MW2RE4.ini'
if ((Test-Path $ini) -and (Ask-YesNo 'Remove your MW2-RE4 options (MW2RE4.ini)?')) { Remove-Item $ini -Force; Say 'Removed MW2RE4.ini' }

# The ASI loader goes only if this installer added it, it is unchanged and no other plugin needs it.
$loader = Join-Path $bin 'dinput8.dll'
if ($manifest.asiLoader -and (Test-Path $loader) -and (Get-FileHash $loader).Hash -eq $manifest.asiLoader) {
    $others = @(Get-ChildItem -Path $bin -Filter '*.asi' -Recurse -Depth 2 -File -ErrorAction SilentlyContinue)
    if ($others.Count) {
        Say "Kept the ASI loader (dinput8.dll): other plugins use it ($($others.Name -join ', '))." Yellow
    } elseif (Ask-YesNo 'Remove the ASI loader (dinput8.dll) MW2-RE4 installed?') {
        Remove-Item $loader -Force
        Say 'Removed dinput8.dll'
    }
}

$runtime = $manifest.runtime
if ($runtime -and (Test-Path (Join-Path $runtime 'iw4l-re4.exe'))) {
    Say "The MW2 runtime folder is $runtime" Yellow
    Say '(it holds the runtime, its caches/logs and its own MW2 unlock progress - not your MW2 or RE4 saves).'
    if (Ask-YesNo 'Delete the MW2 runtime folder?') {
        Remove-Item $runtime -Recurse -Force
        Say 'Removed the MW2 runtime folder.'
    }
}
$desktop = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Resident Evil 4 (MW2-RE4).lnk'
if (Test-Path $desktop) { Remove-Item $desktop -Force; Say 'Removed the desktop shortcut.' }
Remove-Item $manifestPath -Force
Say ''
Say 'MW2-RE4 uninstalled. Your RE4 and MW2 saves were not touched.' Green
if ($manifest.backup -and (Test-Path $manifest.backup)) { Say "Earlier backups remain in: $($manifest.backup)" }
