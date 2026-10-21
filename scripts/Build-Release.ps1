# Packages the release zip from the built plugin and runtime:
#   dist\MW2-RE4-<version>.zip
#     Install-MW2RE4.bat / Uninstall-MW2RE4.bat
#     installer\  (PowerShell installer scripts)
#     files\      (MW2RE4.asi, default MW2RE4.ini, iw4l-re4.exe, default re4-bridge-settings.cfg,
#                  dinput8.dll = Ultimate ASI Loader)
#     licenses\   (IW4L's NOTICE, LICENSE and third-party licences)
#     README.md, LICENSE, NOTICE
# Build first: host\Build-ASI.cmd and the runtime (see README).
param(
    [Parameter(Mandatory)][string]$Version,
    # Built runtime (iw4l.exe); shipped as iw4l-re4.exe.
    [string]$RuntimeExe,
    # IW4L source tree the runtime was built from (for its licence files).
    [string]$RuntimeSource,
    # Ultimate ASI Loader's x86 dinput8.dll.
    [string]$AsiLoader,
    # Package the binaries even if they name a local user folder (e.g. Rust's C:\Users\<name>\.cargo paths).
    [switch]$KeepLocalPaths
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeExe) { $RuntimeExe = Join-Path $root 'runtime-build\debug\iw4l.exe' }
if (-not $RuntimeSource) { $RuntimeSource = Join-Path $root 'iw4l-source' }
if (-not $AsiLoader) { $AsiLoader = Join-Path $root 'third_party\ual-v9.7.4\extracted\dinput8.dll' }
$asi = Join-Path $root 'host\build\MW2RE4.asi'
foreach ($path in $asi, $RuntimeExe, $AsiLoader, (Join-Path $RuntimeSource 'NOTICE')) {
    if (-not (Test-Path $path)) { throw "Missing $path" }
}

$stage = Join-Path $root "dist\MW2-RE4-$Version"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$filesDir = Join-Path $stage 'files'
New-Item -ItemType Directory -Force $filesDir, (Join-Path $stage 'installer'), (Join-Path $stage 'licenses') | Out-Null
Copy-Item $asi $filesDir
Copy-Item $RuntimeExe (Join-Path $filesDir 'iw4l-re4.exe')
Copy-Item $AsiLoader (Join-Path $filesDir 'dinput8.dll')
Copy-Item (Join-Path $root 'installer\MW2RE4.ini') $filesDir
Copy-Item (Join-Path $root 'installer\re4-bridge-settings.cfg') $filesDir
Copy-Item (Join-Path $root 'installer\*.ps1') (Join-Path $stage 'installer')
Copy-Item (Join-Path $root 'installer\*.bat') $stage
foreach ($doc in 'README.md', 'LICENSE', 'NOTICE') { Copy-Item (Join-Path $root $doc) $stage }
foreach ($doc in 'NOTICE', 'LICENSE', 'THIRD-PARTY-LICENSES.txt') {
    Copy-Item (Join-Path $RuntimeSource $doc) (Join-Path $stage "licenses\IW4L-$doc")
}

# Never package anything personal or any game data.
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    if ($file.Name -match '^account|\.lnk$|\.log$|\.ff$|\.iwd$|\.pdb$') { throw "Refusing to package $($file.Name)" }
}
$found = @()
foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $raw = [IO.File]::ReadAllBytes($file.FullName)
    foreach ($encoding in [Text.Encoding]::ASCII, [Text.Encoding]::Unicode) {
        foreach ($shift in 0, 1) {
            $text = $encoding.GetString($raw, $shift, $raw.Length - $shift)
            if ($text -match '[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+\\') { $found += "$($file.Name): $($Matches[0])" }
        }
    }
}
if ($found) {
    $message = "Local user paths in the package:`n  " + (($found | Select-Object -Unique) -join "`n  ")
    if (-not $KeepLocalPaths) { throw "$message`nRebuild with --remap-path-prefix, or pass -KeepLocalPaths." }
    Write-Warning $message
}

$zip = Join-Path $root "dist\MW2-RE4-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Zip entries use forward slashes, which every unzip tool understands.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
        $name = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name, 'Optimal')
    }
} finally { $archive.Dispose() }
Get-FileHash $zip | ForEach-Object { "SHA-256 $($_.Hash)  $zip" }
