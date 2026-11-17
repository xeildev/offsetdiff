$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$stageRoot = $null
try {
    & cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DOFFSETDIFF_BUILD_GUI=ON -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & cmake --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    & ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    $projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
    $releaseName = 'xeil-offsetdiff-0.1.0'
    $releaseDir = Join-Path $projectRoot "dist\$releaseName"
    $archivePath = Join-Path $projectRoot "dist\$releaseName-win64.zip"
    $stageRoot = Join-Path $projectRoot ('build\package-' + [guid]::NewGuid().ToString('N'))
    $stageRelease = Join-Path $stageRoot $releaseName
    $stageArchive = Join-Path $stageRoot "$releaseName-win64.zip"

    & cmake --install build --config Release --prefix $stageRelease
    if ($LASTEXITCODE -ne 0) { throw 'Release installation failed.' }
    Compress-Archive -LiteralPath $stageRelease -DestinationPath $stageArchive

    $previousDir = Join-Path $projectRoot 'dist\previous'
    if (Test-Path -LiteralPath $archivePath) {
        New-Item -ItemType Directory -Path $previousDir -Force | Out-Null
        $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
        $backup = Join-Path $previousDir "$releaseName-$hash-win64.zip"
        if (-not (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $archivePath -Destination $backup
        }
    } elseif (Test-Path -LiteralPath $releaseDir) {
        New-Item -ItemType Directory -Path $previousDir -Force | Out-Null
        $backup = Join-Path $previousDir ("$releaseName-" + [guid]::NewGuid().ToString('N') + '-win64.zip')
        Compress-Archive -LiteralPath $releaseDir -DestinationPath $backup
    }

    if (Test-Path -LiteralPath $releaseDir) {
        $resolvedRelease = (Resolve-Path -LiteralPath $releaseDir).Path
        if (-not $resolvedRelease.StartsWith($projectRoot + '\dist\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Release directory is outside the project dist directory.'
        }
        Remove-Item -LiteralPath $resolvedRelease -Recurse -Force
    }
    New-Item -ItemType Directory -Path (Join-Path $projectRoot 'dist') -Force | Out-Null
    Move-Item -LiteralPath $stageRelease -Destination $releaseDir
    Move-Item -LiteralPath $stageArchive -Destination $archivePath -Force
    Write-Host 'Ready: dist\xeil-offsetdiff-0.1.0\offsetdiff.exe'
    Write-Host 'ZIP:   dist\xeil-offsetdiff-0.1.0-win64.zip'
} finally {
    if ($stageRoot -and (Test-Path -LiteralPath $stageRoot)) {
        $resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
        $buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build')) + '\'
        if ($resolvedStage.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force
        }
    }
    Pop-Location
}
