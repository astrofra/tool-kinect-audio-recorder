# Shared by the recorder and FFmpeg build entry points (Windows PowerShell 5.1+).
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

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

function Reset-BuildDirectory([string] $Path) {
    Assert-WorkspacePath $Path
    if (Test-Path -LiteralPath $Path) {
        # Windows PowerShell versions differ in recursive link handling.
        $linkedEntry = Get-ChildItem -LiteralPath $Path -Force -Recurse |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
            Select-Object -First 1
        if ($linkedEntry) { throw "Refusing to clean a build containing a link: $($linkedEntry.FullName)" }
        Write-Host "Cleaning $Path"
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Invoke-Checked([string] $Program, [string[]] $Arguments) {
    Write-Host ("> " + $Program + ' ' + ($Arguments -join ' '))
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

function Get-PackageFiles([string] $Directory) {
    Assert-WorkspacePath $Directory
    $entries = @(Get-ChildItem -LiteralPath $Directory -Recurse -Force)
    foreach ($entry in $entries) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing a linked package entry: $($entry.FullName)"
        }
    }
    $entries | Where-Object { -not $_.PSIsContainer } | Sort-Object FullName
}

function Assert-PackageDestinations($Files, [string] $Source, [string] $Destination) {
    foreach ($file in $Files) {
        $target = Join-Path $Destination $file.FullName.Substring($Source.Length + 1)
        Assert-WorkspacePath $target
        if (Test-Path -LiteralPath $target -PathType Container) {
            throw "Expected a file at the release destination: $target"
        }
    }
}

function Copy-PackageFiles($Files, [string] $Source, [string] $Destination) {
    foreach ($file in $Files) {
        $target = Join-Path $Destination $file.FullName.Substring($Source.Length + 1)
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}
