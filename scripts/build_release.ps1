# Called by build_release.bat. Requires Windows PowerShell 5.1 or newer.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = Join-Path $repoRoot 'build\release'
$releaseDir = Join-Path $repoRoot 'release'
$stageDir = Join-Path $buildDir 'package'

# Check both containment and intermediate junctions before deleting or writing.
function Assert-WorkspacePath([string] $Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $repoRoot.TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the workspace: $fullPath"
    }
    $cursor = $fullPath
    while ($cursor -ne $repoRoot) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to use a junction or symbolic link: $cursor"
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
}

function Invoke-Checked([string] $Program, [string[]] $Arguments) {
    Write-Host ("> " + $Program + ' ' + ($Arguments -join ' '))
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

try {
    Push-Location -LiteralPath $repoRoot
    try {
        # Fail before deleting anything if a required command is missing.
        $cmake = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop).Source
        $ctest = (Get-Command ctest.exe -CommandType Application -ErrorAction Stop).Source
        $null = Get-Command git.exe -CommandType Application -ErrorAction Stop
        Assert-WorkspacePath $buildDir
        Assert-WorkspacePath $releaseDir
        if (Test-Path -LiteralPath $buildDir) {
            # Windows PowerShell versions differ in recursive link handling.
            # Refuse linked entries instead of risking deletion outside this tree.
            $linkedEntry = Get-ChildItem -LiteralPath $buildDir -Force -Recurse |
                Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
                Select-Object -First 1
            if ($linkedEntry) { throw "Refusing to clean a build containing a link: $($linkedEntry.FullName)" }
            Write-Host "Cleaning $buildDir"
            Remove-Item -LiteralPath $buildDir -Recurse -Force
        }

        Invoke-Checked $cmake @('--preset', 'windows-release')
        Invoke-Checked $cmake @('--build', '--preset', 'windows-release', '--parallel')
        Invoke-Checked $cmake @('--install', $buildDir, '--config', 'Release', '--prefix', $stageDir)
        Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\README.md') -Destination $stageDir

        $ffmpegWork = Join-Path $buildDir 'ffmpeg'
        $ffmpegPackage = Join-Path $stageDir 'extern\ffmpeg'
        $downloads = Join-Path $repoRoot 'build\downloads'
        Assert-WorkspacePath $ffmpegWork
        Assert-WorkspacePath $ffmpegPackage
        Assert-WorkspacePath $downloads
        & (Join-Path $repoRoot 'extern\ffmpeg\build.ps1') -WorkDirectory $ffmpegWork -Destination $ffmpegPackage -CacheDirectory $downloads
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

        $packageFiles = @(Get-ChildItem -LiteralPath $stageDir -File -Recurse | Sort-Object FullName)
        $checksums = foreach ($file in $packageFiles) {
            $relative = $file.FullName.Substring($stageDir.Length + 1).Replace('\', '/')
            (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative
        }
        $checksums | Set-Content -LiteralPath (Join-Path $stageDir 'SHA256SUMS.txt') -Encoding ASCII
        $packageFiles = @(Get-ChildItem -LiteralPath $stageDir -File -Recurse)

        # Publish only generated package files; keep recordings and other user files.
        # Preflight destinations so a symlink cannot redirect an overwrite.
        foreach ($file in $packageFiles) {
            $relative = $file.FullName.Substring($stageDir.Length + 1)
            $destination = Join-Path $releaseDir $relative
            Assert-WorkspacePath $destination
            if (Test-Path -LiteralPath $destination -PathType Container) {
                throw "Expected a file at the release destination: $destination"
            }
        }
        foreach ($file in $packageFiles) {
            $relative = $file.FullName.Substring($stageDir.Length + 1)
            $destination = Join-Path $releaseDir $relative
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
        }
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
