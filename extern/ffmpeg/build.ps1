param(
    [Parameter(Mandatory = $true)][string] $WorkDirectory,
    [Parameter(Mandatory = $true)][string] $Destination,
    [Parameter(Mandatory = $true)][string] $CacheDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# No deletion: the caller owns cleaning its build tree. Refuse to overwrite a package.
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$Destination = [IO.Path]::GetFullPath($Destination)
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
if ((Test-Path -LiteralPath $WorkDirectory) -or (Test-Path -LiteralPath $Destination)) {
    throw 'FFmpeg work and destination directories must be new.'
}
$sources = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sources.json') -Raw | ConvertFrom-Json
$git = (Get-Command git.exe -CommandType Application -ErrorAction Stop).Source
$gitRoot = Split-Path -Parent (Split-Path -Parent $git)
$bash = Join-Path $gitRoot 'usr\bin\bash.exe'
if ($env:RECORDER_GIT_BASH) { $bash = $env:RECORDER_GIT_BASH }
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw 'Git for Windows Bash was not found. Set RECORDER_GIT_BASH to its bash.exe path.'
}
$curl = (Get-Command curl.exe -CommandType Application -ErrorAction Stop).Source
$tar = (Get-Command tar.exe -CommandType Application -ErrorAction Stop).Source
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsInstall) { throw 'Visual Studio 2022 C++ tools were not found.' }
Import-Module (Join-Path $vsInstall 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:RECORDER_FFMPEG_CC_DIR = Split-Path -Parent (Get-Command cl.exe).Source
$null = New-Item -ItemType Directory -Path $WorkDirectory,$CacheDirectory -Force

function Get-VerifiedArchive($Entry) {
    $path = Join-Path $CacheDirectory $Entry.file
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "Downloading $($Entry.file)"
        & $curl --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 180 --output "$path.partial" $Entry.url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($Entry.url)" }
        if ((Get-FileHash -LiteralPath "$path.partial" -Algorithm SHA256).Hash -ne $Entry.sha256) {
            throw "Downloaded archive hash mismatch: $($Entry.file)"
        }
        Move-Item -LiteralPath "$path.partial" -Destination $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Entry.sha256) {
        throw "Cached archive hash mismatch: $path"
    }
    return $path
}

$archive = Get-VerifiedArchive $sources.source
$makeArchive = Get-VerifiedArchive $sources.make
& $tar -xf $archive -C $WorkDirectory
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract FFmpeg sources.' }
& $tar -xf $makeArchive -C $WorkDirectory
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract GNU Make.' }
$sourceDirectory = Join-Path $WorkDirectory ('ffmpeg-' + $sources.version)
$make = Join-Path $WorkDirectory 'usr\bin\make.exe'
$log = Join-Path $WorkDirectory 'build.log'
Write-Host "Building minimal FFmpeg $($sources.version) with MSVC; log: $log"
& $bash --noprofile --norc (Join-Path $PSScriptRoot 'build.sh') $sourceDirectory $make $log
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ }
    throw "FFmpeg compilation failed; see $log"
}

$null = New-Item -ItemType Directory -Path $Destination,(Join-Path $Destination 'licenses'),(Join-Path $Destination 'sources') -Force
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'ffmpeg.exe') -Destination $Destination
Copy-Item -LiteralPath $archive -Destination (Join-Path $Destination 'sources')
foreach ($name in @('COPYING.LGPLv2.1','COPYING.LGPLv3','COPYING.GPLv2','COPYING.GPLv3','LICENSE.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory $name) -Destination (Join-Path $Destination 'licenses')
}
foreach ($name in @('build.ps1','build.sh','sources.json','README.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $Destination
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'config.h'),(Join-Path $sourceDirectory 'config_components.h') -Destination $Destination
$version = & (Join-Path $Destination 'ffmpeg.exe') -version
if ($LASTEXITCODE -ne 0) { throw 'The bundled FFmpeg executable did not start.' }
$version | Set-Content -LiteralPath (Join-Path $Destination 'version.txt') -Encoding UTF8
Write-Host "FFmpeg ready: $Destination"
