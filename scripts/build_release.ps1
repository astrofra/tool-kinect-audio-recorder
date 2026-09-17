# Called by build_release.bat. Requires Windows PowerShell 5.1 or newer.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'build_common.ps1')
$buildDir = Join-Path $repoRoot 'build\release'
$releaseDir = Join-Path $repoRoot 'release'
$stageDir = Join-Path $buildDir 'package'
$ffmpegBundle = Join-Path $releaseDir 'extern\ffmpeg'

try {
    Push-Location -LiteralPath $repoRoot
    try {
        # Fail before deleting anything if the independent FFmpeg package is missing.
        Assert-WorkspacePath $releaseDir
        Assert-WorkspacePath $ffmpegBundle
        if (-not (Test-Path -LiteralPath (Join-Path $ffmpegBundle 'ffmpeg.exe') -PathType Leaf)) {
            throw 'Bundled FFmpeg is missing. Run rebuild_ffmpeg.bat first, then build_release.bat.'
        }
        $ffmpegFiles = @(Get-PackageFiles $ffmpegBundle)
        foreach ($required in @('sources.json', 'build.ps1', 'build.sh', 'README.md', 'version.txt', 'config.h', 'config_components.h', 'licenses\COPYING.LGPLv2.1')) {
            if (-not (Test-Path -LiteralPath (Join-Path $ffmpegBundle $required) -PathType Leaf)) {
                throw "Incomplete FFmpeg package ($required missing). Run rebuild_ffmpeg.bat first."
            }
        }
        $ffmpegSources = Get-Content -LiteralPath (Join-Path $ffmpegBundle 'sources.json') -Raw | ConvertFrom-Json
        $ffmpegArchive = Join-Path $ffmpegBundle ('sources\' + $ffmpegSources.source.file)
        Assert-WorkspacePath $ffmpegArchive
        if (-not (Test-Path -LiteralPath $ffmpegArchive -PathType Leaf)) {
            throw 'FFmpeg source archive is missing. Run rebuild_ffmpeg.bat first.'
        }
        if ((Get-FileHash -LiteralPath $ffmpegArchive -Algorithm SHA256).Hash -ne $ffmpegSources.source.sha256) {
            throw 'FFmpeg source archive hash mismatch. Run rebuild_ffmpeg.bat first.'
        }
        Write-Host "Reusing FFmpeg: $ffmpegBundle (run rebuild_ffmpeg.bat to rebuild it)"
        $cmake = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop).Source
        $ctest = (Get-Command ctest.exe -CommandType Application -ErrorAction Stop).Source
        $null = Get-Command git.exe -CommandType Application -ErrorAction Stop
        Reset-BuildDirectory $buildDir

        Invoke-Checked $cmake @('--preset', 'windows-release')
        Invoke-Checked $cmake @('--build', '--preset', 'windows-release', '--parallel')
        Invoke-Checked $cmake @('--install', $buildDir, '--config', 'Release', '--prefix', $stageDir)
        Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\README.md') -Destination $stageDir

        $ffmpegPackage = Join-Path $stageDir 'extern\ffmpeg'
        Assert-WorkspacePath $ffmpegPackage
        Assert-PackageDestinations $ffmpegFiles $ffmpegBundle $ffmpegPackage
        Copy-PackageFiles $ffmpegFiles $ffmpegBundle $ffmpegPackage
        $env:RECORDER_TEST_FFMPEG = Join-Path $ffmpegPackage 'ffmpeg.exe'
        Invoke-Checked $ctest @('--preset', 'windows-release', '--no-tests=error')

        # Exercise the packaged programs before touching the existing release.
        Invoke-Checked (Join-Path $stageDir 'recording_tool.exe') @(
            'record', '--source', 'simulate', '--output', 'build/release/cli-smoke',
            '--duration', '0.25', '--channels', '2', '--depth', 'gradient', '--encode-depth', '--fast')
        Invoke-Checked (Join-Path $stageDir 'audio_recorder.exe') @(
            '--smoke-test', 'build/release/gui-smoke')
        Invoke-Checked (Join-Path $stageDir 'recording_tool.exe') @(
            'export-depth', '--input', 'build/release/cli-smoke/depth/000000.kd16',
            '--audio', 'build/release/cli-smoke/audio/000000.wav', '--output', 'build/release/cli-smoke.mkv')

        $packageFiles = @(Get-PackageFiles $stageDir)
        $checksums = foreach ($file in $packageFiles) {
            $relative = $file.FullName.Substring($stageDir.Length + 1).Replace('\', '/')
            (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative
        }
        $checksums | Set-Content -LiteralPath (Join-Path $stageDir 'SHA256SUMS.txt') -Encoding ASCII
        # The reused vendor bundle is checked and tested, but only the dedicated
        # rebuild script publishes it. Preserve its files and modification times.
        $packageFiles = @(Get-PackageFiles $stageDir | Where-Object {
            -not $_.FullName.StartsWith($ffmpegPackage + '\', [StringComparison]::OrdinalIgnoreCase)
        })

        # Publish only generated package files; keep recordings and other user files.
        # Preflight destinations so a symlink cannot redirect an overwrite.
        Assert-PackageDestinations $packageFiles $stageDir $releaseDir
        Copy-PackageFiles $packageFiles $stageDir $releaseDir
        Write-Host "Ready to commit: $releaseDir" -ForegroundColor Green
        Write-Host 'No Git staging, commit, or push was performed.'
    }
    finally {
        Pop-Location
    }
}
catch {
    [Console]::Error.WriteLine("Build/release failed: " + $_.Exception.Message)
    exit 1
}
exit 0
