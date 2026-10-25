# MW2-RE4 installer. Installs the MW2RE4.asi plugin into Resident Evil 4 (2005) Ultimate HD Edition and the
# MW2 runtime next to it, pointing the runtime at an existing MW2 installation.
#
# It never deletes or modifies save games:
#   - RE4's saves live in Steam's userdata folder (never touched).
#   - The MW2 folder is only read; a shortcut to it is created in the runtime.
# Files this installer would replace are backed up first.
param(
    [string]$Re4Dir,
    [string]$Mw2Dir,
    [string]$RuntimeDir,
    # Unattended: accept every default answer (folders must be found or given).
    [switch]$Yes,
    [switch]$NoDesktopShortcut,
    # Only show which RE4 and MW2 folders would be used; install nothing.
    [switch]$DetectOnly
)
$ErrorActionPreference = 'Stop'
$package = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$files = Join-Path $package 'files'
$re4Exe = 9139840 # bio4.exe of the current Steam release (1.1.0), the one MW2-RE4 is made and tested with

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
function Pick-Folder([string]$description, [string]$start) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = $description
    $dialog.ShowNewFolderButton = $false
    if ($start -and (Test-Path $start)) { $dialog.SelectedPath = $start }
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    return $null
}
# The RE4 folder is the one with Bin32\bio4.exe; picking Bin32 itself is accepted too.
function Resolve-Re4([string]$dir) {
    if (-not $dir) { return $null }
    if (Test-Path (Join-Path $dir 'Bin32\bio4.exe')) { return (Resolve-Path $dir).Path.TrimEnd('\') }
    if ((Split-Path -Leaf $dir) -eq 'Bin32' -and (Test-Path (Join-Path $dir 'bio4.exe'))) {
        return (Resolve-Path (Split-Path -Parent $dir)).Path.TrimEnd('\')
    }
    return $null
}
# MW2 only: MW3, Black Ops and Black Ops II also have zone\<language>\common_mp.ff, so the fastfile's version
# must be MW2's (IW4, 0x114), as the runtime itself checks. Other Call of Duty folders are never used.
function Test-Mw2([string]$dir) {
    if (-not $dir) { return $false }
    $zone = Join-Path $dir 'zone'
    if (-not (Test-Path $zone)) { return $false }
    foreach ($ff in @(Get-ChildItem -Path $zone -Filter 'common_mp.ff' -Recurse -Depth 1 -File -ErrorAction SilentlyContinue)) {
        try {
            $stream = [IO.File]::OpenRead($ff.FullName)
            try {
                $header = New-Object byte[] 12
                if ($stream.Read($header, 0, 12) -eq 12 -and [BitConverter]::ToUInt32($header, 8) -eq 0x114) { return $true }
            } finally { $stream.Dispose() }
        } catch {}
    }
    return $false
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
function Backup-File([string]$path, [string]$backupDir, [string]$replacement) {
    if (Test-Path $path) {
        # Nothing to preserve when the installed file is already identical.
        if ($replacement -and (Get-FileHash $path).Hash -eq (Get-FileHash $replacement).Hash) { return }
        New-Item -ItemType Directory -Force $backupDir | Out-Null
        Copy-Item $path (Join-Path $backupDir (Split-Path -Leaf $path)) -Force
    }
}
function Set-IniValue([string[]]$lines, [string]$key, [string]$value) {
    $found = $false
    $result = foreach ($line in $lines) {
        if ($line -match "^\s*$([regex]::Escape($key))\s*=") { $found = $true; "$key=$value" } else { $line }
    }
    if (-not $found) {
        # new keys go at the end of the [MW2RE4] section, before the next section ([Balancing], [Guns])
        $start = [array]::FindIndex([string[]]$result, [Predicate[string]] { param($l) $l -match '^\s*\[MW2RE4\]' })
        $at = [array]::FindIndex([string[]]$result, [int]($start + 1), [Predicate[string]] { param($l) $l -match '^\s*\[' })
        if ($at -lt 0) { $result = @($result) + "$key=$value" }
        else { $result = @($result[0..($at - 1)]) + "$key=$value" + @($result[$at..($result.Count - 1)]) }
    }
    return , @($result)
}

Clear-Host
Say '=============================================================' Cyan
Say '  MW2-RE4 (Beta)  -  MW2 gameplay inside Resident Evil 4' Cyan
Say '=============================================================' Cyan
Say ''
Say 'This installer will:'
Say '  1. Find your Resident Evil 4 (Ultimate HD Edition) folder.'
Say '  2. Find your Call of Duty: Modern Warfare 2 (2009) folder.'
Say '  3. Copy MW2RE4.asi (and an ASI loader if needed) into RE4\Bin32'
Say '     and the MW2 runtime next to it.'
Say ''
Say 'Your RE4 and MW2 save games are never deleted or changed.' Green
Say '(But a backup is always recommended when installing mods.)' Yellow
Say ''

if (-not $DetectOnly) {
    foreach ($name in 'bio4', 'iw4l-re4') {
        while (Get-Process -Name $name -ErrorAction SilentlyContinue) {
            Say "$name.exe is running. Close the game, then press Enter." Yellow
            if ($Yes) { throw "$name.exe is running; close it before installing." }
            Read-Host | Out-Null
        }
    }
    foreach ($required in 'MW2RE4.asi', 'MW2RE4.ini', 'iw4l-re4.exe', 're4-bridge-settings.cfg', 'dinput8.dll') {
        if (-not (Test-Path (Join-Path $files $required))) {
            throw "The package is incomplete: files\$required is missing. Download the full release zip again."
        }
    }
}

# --- Resident Evil 4 -------------------------------------------------------
$given = $Re4Dir
$Re4Dir = Resolve-Re4 $Re4Dir
if ($given -and -not $Re4Dir) {
    # a folder given on purpose is never swapped for one found automatically
    if ($Yes -or $DetectOnly) { throw "$given does not contain Bin32\bio4.exe." }
    Say "$given does not contain Bin32\bio4.exe." Red
}
if (-not $Re4Dir -and -not $given) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Steam\steamapps\common\Resident Evil 4",
        "$env:ProgramFiles\Steam\steamapps\common\Resident Evil 4"
    ) + (Steam-Libraries | ForEach-Object { Join-Path $_ 'steamapps\common\Resident Evil 4' })
    $Re4Dir = $candidates | ForEach-Object { Resolve-Re4 $_ } | Where-Object { $_ } | Select-Object -First 1
    if ($Re4Dir) {
        Say "Found Resident Evil 4: $Re4Dir"
        if (-not $DetectOnly -and -not (Ask-YesNo 'Use this folder?')) { $Re4Dir = $null }
    }
}
if (-not $Re4Dir) {
    while (-not $Re4Dir) {
        if ($Yes -or $DetectOnly) { throw 'Resident Evil 4 folder not found; pass -Re4Dir.' }
        Say 'Select your Resident Evil 4 folder (the one containing Bin32\bio4.exe).' Yellow
        $picked = Pick-Folder 'Select the Resident Evil 4 folder (contains Bin32\bio4.exe)' $null
        if (-not $picked) { throw 'Installation cancelled: no Resident Evil 4 folder selected.' }
        $Re4Dir = Resolve-Re4 $picked
        if (-not $Re4Dir) { Say 'That folder does not contain Bin32\bio4.exe.' Red }
    }
}
$bin = Join-Path $Re4Dir 'Bin32'
$exe = Get-Item (Join-Path $bin 'bio4.exe')
if ($exe.Length -ne $re4Exe) {
    Say ''
    Say 'WARNING: bio4.exe is not the current Steam release of RE4 Ultimate HD Edition.' Yellow
    Say 'MW2-RE4 is made for the unmodified Steam bio4.exe (1.1.0). Patched or older executables' Yellow
    Say 'may not work. If you replaced bio4.exe, Steam > Properties > Installed Files > Verify restores it.' Yellow
    if (-not $DetectOnly -and -not (Ask-YesNo 'Continue anyway?' $false)) { throw 'Installation cancelled: Steam bio4.exe required.' }
}

# --- Modern Warfare 2 ------------------------------------------------------
if (-not (Test-Mw2 $Mw2Dir)) {
    if ($Mw2Dir) {
        if ($Yes -or $DetectOnly) { throw "$Mw2Dir is not Modern Warfare 2 (2009): no MW2 zone\<language>\common_mp.ff (MW3, Black Ops and Black Ops II are not used)." }
        Say "$Mw2Dir is not Modern Warfare 2 (2009): no MW2 zone\<language>\common_mp.ff (MW3, Black Ops and Black Ops II are not used)." Red
    } else {
        $Mw2Dir = Steam-Libraries | ForEach-Object { Join-Path $_ 'steamapps\common\Call of Duty Modern Warfare 2' } |
            Where-Object { Test-Mw2 $_ } | Select-Object -First 1
        if ($Mw2Dir) {
            Say "Found Modern Warfare 2: $Mw2Dir"
            if (-not $DetectOnly -and -not (Ask-YesNo 'Use this folder?')) { $Mw2Dir = $null }
        }
    }
    while (-not (Test-Mw2 $Mw2Dir)) {
        if ($Yes -or $DetectOnly) { throw 'Modern Warfare 2 folder not found; pass -Mw2Dir.' }
        Say 'Select your Call of Duty: Modern Warfare 2 (2009) folder (contains the "zone" folder).' Yellow
        $Mw2Dir = Pick-Folder 'Select the Modern Warfare 2 (2009) folder (contains zone\...\common_mp.ff)' $Mw2Dir
        if (-not $Mw2Dir) { throw 'Installation cancelled: no Modern Warfare 2 folder selected.' }
        if (-not (Test-Mw2 $Mw2Dir)) { Say 'That folder is not Modern Warfare 2 (2009): no MW2 zone\<language>\common_mp.ff (MW3, Black Ops and Black Ops II are not used).' Red }
    }
}

# --- Runtime location ------------------------------------------------------
if (-not $RuntimeDir) {
    $RuntimeDir = Join-Path $Re4Dir 'MW2RE4'
    # The runtime writes caches and logs; Program Files is not writable for it.
    if ($Re4Dir -like "$env:ProgramFiles*" -or $Re4Dir -like "${env:ProgramFiles(x86)}*") {
        $RuntimeDir = Join-Path $env:LOCALAPPDATA 'MW2RE4'
    }
}

# --- ASI loader ------------------------------------------------------------
# RE4 loads .asi plugins through Ultimate ASI Loader in Bin32. bio4.exe loads dinput8.dll, winmm.dll and
# xinput1_3.dll from there, so the loader can take any of these names. A dinput8.dll that is not an ASI
# loader (re4_tweaks', a trainer's) is kept, and the loader goes in under the next free name instead.
function Test-AsiLoader([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $text = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($path))
    return $text.Contains('Ultimate ASI Loader') -or $text.Contains('ThirteenAG')
}
$loader = $null
$installLoader = $false
foreach ($name in 'dinput8.dll', 'winmm.dll', 'xinput1_3.dll') {
    if (Test-AsiLoader (Join-Path $bin $name)) { $loader = Join-Path $bin $name; break }
}
if ($loader) { Say "ASI loader found (Bin32\$(Split-Path -Leaf $loader)); it is kept." Green }
else {
    foreach ($name in 'dinput8.dll', 'winmm.dll', 'xinput1_3.dll') {
        if (-not (Test-Path (Join-Path $bin $name))) { $loader = Join-Path $bin $name; break }
    }
    if (-not $loader) { throw 'Bin32 has dinput8.dll, winmm.dll and xinput1_3.dll from other mods and none of them loads .asi plugins. Move one of them out of Bin32 and run the installer again.' }
    $installLoader = $true
    $name = Split-Path -Leaf $loader
    if ($name -eq 'dinput8.dll') { Say 'No ASI loader in RE4\Bin32: the bundled Ultimate ASI Loader (dinput8.dll) will be installed.' }
    else { Say "Bin32\dinput8.dll belongs to another mod (re4_tweaks or a trainer) and does not load .asi plugins: it is kept, and the bundled Ultimate ASI Loader is installed as $name." Yellow }
}
if ((Test-Path (Join-Path $bin 're4_tweaks.asi')) -or (Test-Path (Join-Path $bin 're4_tweaks'))) {
    Say 're4_tweaks is installed. MW2-RE4 is not tested with it; if MW2 mode misbehaves, try without it.' Yellow
}

Say ''
Say 'Ready to install:' Cyan
Say "  Resident Evil 4 : $Re4Dir"
Say "  Modern Warfare 2: $Mw2Dir"
Say "  MW2 runtime     : $RuntimeDir"
if ($DetectOnly) { Say ''; Say 'Detection only: nothing was installed.' Green; return }
if (-not (Ask-YesNo 'Install now?')) { throw 'Installation cancelled.' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $Re4Dir "MW2RE4-backup\$stamp"
New-Item -ItemType Directory -Force $RuntimeDir | Out-Null

# ASI loader and plugin.
if ($installLoader) { Copy-Item (Join-Path $files 'dinput8.dll') $loader }  # the same file under any of its names
Backup-File (Join-Path $bin 'MW2RE4.asi') $backup (Join-Path $files 'MW2RE4.asi')
Copy-Item (Join-Path $files 'MW2RE4.asi') (Join-Path $bin 'MW2RE4.asi') -Force

# Runtime. Player settings and the runtime account (progression) are kept.
Backup-File (Join-Path $RuntimeDir 'iw4l-re4.exe') $backup (Join-Path $files 'iw4l-re4.exe')
Copy-Item (Join-Path $files 'iw4l-re4.exe') (Join-Path $RuntimeDir 'iw4l-re4.exe') -Force
$settings = Join-Path $RuntimeDir 're4-bridge-settings.cfg'
if (-not (Test-Path $settings)) { Copy-Item (Join-Path $files 're4-bridge-settings.cfg') $settings }

# Shortcut the runtime uses to read the MW2 installation (read-only).
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut((Join-Path $RuntimeDir 'Modern Warfare 2.lnk'))
$link.TargetPath = $Mw2Dir
$link.Save()

# Plugin settings: keep the player's options, update only the runtime location.
$ini = Join-Path $bin 'MW2RE4.ini'
if (Test-Path $ini) {
    Backup-File $ini $backup
    $lines = @(Get-Content $ini)
} else {
    $lines = @(Get-Content (Join-Path $files 'MW2RE4.ini'))
}
$lines = Set-IniValue $lines 'RuntimeDir' $RuntimeDir
$lines = Set-IniValue $lines 'RuntimeExecutable' 'iw4l-re4.exe'
$lines = Set-IniValue $lines 'RuntimeSettings' 're4-bridge-settings.cfg'
$lines = Set-IniValue $lines 'MappingName' 'MW2RE4_v1'
Set-Content -Path $ini -Value $lines -Encoding Unicode

# Record what was installed so the uninstaller removes only these files.
$manifestPath = Join-Path $bin 'MW2RE4.install.json'
$loaderHash = if ($installLoader) { (Get-FileHash $loader).Hash } else { $null }
if (-not $loaderHash -and (Test-Path $manifestPath)) {
    # an update: the ASI loader an earlier install added is still this mod's
    $previous = Get-Content $manifestPath -Raw | ConvertFrom-Json
    $previousName = if ($previous.asiLoaderName) { $previous.asiLoaderName } else { 'dinput8.dll' }
    if ($previous.asiLoader -and $previousName -eq (Split-Path -Leaf $loader) -and (Test-Path $loader) -and
        (Get-FileHash $loader).Hash -eq $previous.asiLoader) {
        $loaderHash = $previous.asiLoader
    }
}
$manifest = [ordered]@{
    installed = (Get-Date).ToString('s')
    re4 = $Re4Dir
    mw2 = $Mw2Dir
    runtime = $RuntimeDir
    asiLoader = $loaderHash
    asiLoaderName = $(if ($loaderHash) { Split-Path -Leaf $loader } else { $null })
    backup = $(if (Test-Path $backup) { $backup } else { $null })
}
$manifest | ConvertTo-Json | Set-Content -Path $manifestPath -Encoding UTF8

if (-not $NoDesktopShortcut -and (Ask-YesNo 'Create a desktop shortcut "Resident Evil 4 (MW2-RE4)"?')) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $play = $shell.CreateShortcut((Join-Path $desktop 'Resident Evil 4 (MW2-RE4).lnk'))
    $play.TargetPath = $exe.FullName
    $play.WorkingDirectory = $bin
    $play.Save()
}

Say ''
Say 'MW2-RE4 installed.' Green
if (Test-Path $backup) { Say "Replaced files were backed up to: $backup" }
Say ''
Say 'How to play:' Cyan
Say '  Start Resident Evil 4, load your game, then press F6. The first start'
Say '  prepares MW2 assets and can take about a minute.'
Say '  F6 on/off  F7 controls  F10 settings  F11 gun damage  F5 gun list  1 next gun  E use'
