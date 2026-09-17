# Called by rebuild_ffmpeg.bat. Does not compile or replace recorder executables.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build_common.ps1')

$buildDir = Join-Path $repoRoot 'build\ffmpeg-rebuild'
$workDir = Join-Path $buildDir 'work'
$stageDir = Join-Path $buildDir 'package'
$downloads = Join-Path $repoRoot 'build\downloads'
$releaseDir = Join-Path $repoRoot 'release'
$destination = Join-Path $releaseDir 'extern\ffmpeg'
$checksumFile = Join-Path $releaseDir 'SHA256SUMS.txt'

try {
    Assert-WorkspacePath $destination
    Assert-WorkspacePath $downloads
    Assert-WorkspacePath $checksumFile
    Reset-BuildDirectory $buildDir
    & (Join-Path $repoRoot 'extern\ffmpeg\build.ps1') -WorkDirectory $workDir -Destination $stageDir -CacheDirectory $downloads

    # Validate the standalone encoder even when the recorder has not been built.
    $ffmpeg = Join-Path $stageDir 'ffmpeg.exe'
    $raw = Join-Path $buildDir 'smoke-gray16.raw'
    $video = Join-Path $buildDir 'smoke.mkv'
    $decoded = Join-Path $buildDir 'smoke-decoded.raw'
    $pixels = New-Object byte[] (512 * 424 * 2)
    for ($i = 0; $i -lt $pixels.Length; $i++) { $pixels[$i] = $i % 251 }
    [IO.File]::WriteAllBytes($raw, $pixels)
    Invoke-Checked $ffmpeg @('-hide_banner', '-loglevel', 'error', '-nostdin', '-n',
        '-f', 'rawvideo', '-pixel_format', 'gray16le', '-video_size', '512x424', '-framerate', '30',
        '-i', $raw, '-c:v', 'ffv1', '-level', '3', '-pix_fmt', '+gray16le', $video)
    Invoke-Checked $ffmpeg @('-hide_banner', '-loglevel', 'error', '-nostdin', '-n',
        '-i', $video, '-pix_fmt', 'gray16le', '-f', 'rawvideo', $decoded)
    if ((Get-FileHash -LiteralPath $raw).Hash -ne (Get-FileHash -LiteralPath $decoded).Hash) {
        throw 'FFmpeg depth round trip did not preserve the input bytes.'
    }

    $packageFiles = @(Get-PackageFiles $stageDir)
    Assert-PackageDestinations $packageFiles $stageDir $destination
    # Keep checksums for the recorder and other packaged files, never enumerate recordings.
    $checksums = @()
    if (Test-Path -LiteralPath $checksumFile -PathType Leaf) {
        $checksums = @(Get-Content -LiteralPath $checksumFile | Where-Object {
            $_ -notmatch '^[0-9a-fA-F]{64}  extern/ffmpeg/'
        })
    }
    $checksums += @(foreach ($file in $packageFiles) {
        $relative = $file.FullName.Substring($stageDir.Length + 1).Replace('\', '/')
        (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  extern/ffmpeg/' + $relative
    })
    Copy-PackageFiles $packageFiles $stageDir $destination
    $checksums | Sort-Object { ($_ -split '  ', 2)[1] } | Set-Content -LiteralPath $checksumFile -Encoding ASCII
    Write-Host "FFmpeg rebuilt and verified: $destination" -ForegroundColor Green
    Write-Host 'Recorder executables were not rebuilt. No Git staging, commit, or push was performed.'
}
catch {
    [Console]::Error.WriteLine('FFmpeg rebuild failed: ' + $_.Exception.Message)
    exit 1
}
exit 0
