param(
    [string]$AssetSource,
    [ValidateRange(1,16)][int]$Jobs = 6,
    [switch]$Flash,
    [string]$Port
)
$ErrorActionPreference = 'Stop'
$badgeRoot = Split-Path -Parent $PSScriptRoot
$badgeProject = Join-Path $badgeRoot 'firmware\ai_pet_badge'
$badgePack = Join-Path $badgeProject 'main\animations.badge'
if ($AssetSource) {
    $assetPython = Join-Path $badgeRoot '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $assetPython)) { throw 'Create .venv and install tools/requirements-assets.txt first.' }
    & $assetPython (Join-Path $PSScriptRoot 'prepare_badge_assets.py') --source $AssetSource --output $badgePack --preview-dir (Join-Path $badgeRoot 'artifacts\assets\preview') --fps 15
    if ($LASTEXITCODE -ne 0) { throw 'Animation conversion failed.' }
}
if (-not (Test-Path -LiteralPath $badgePack)) { throw 'Animation pack missing. Supply -AssetSource with walk.mp4, jump.mp4 and sit.mp4.' }
. (Join-Path $PSScriptRoot 'Enter-Idf.ps1')
python (Join-Path $PSScriptRoot 'normalize_lock_paths.py')
if ($LASTEXITCODE -ne 0) { throw 'Component-path normalization failed.' }
python (Join-Path $env:IDF_PATH 'tools\idf.py') -C $badgeProject reconfigure
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF configure failed.' }
cmake --build (Join-Path $badgeProject 'build') --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
if ($Flash) {
    if (-not $Port) { throw 'Supply the confirmed board port with -Port.' }
    Push-Location -LiteralPath (Join-Path $badgeProject 'build')
    try {
        python -m esptool --chip esp32s3 --port $Port --baud 921600 write_flash '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw 'Firmware flashing failed.' }
    } finally { Pop-Location }
}
