param([switch]$Upgrade)

$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
$requirements = Join-Path $PSScriptRoot 'requirements.txt'

if (-not (Test-Path $python)) {
    $uv = Get-Command uv -ErrorAction SilentlyContinue
    if ($uv) {
        & $uv.Source venv --python 3.12.12 (Join-Path $PSScriptRoot '.venv')
    } else {
        $launcher = Get-Command py -ErrorAction SilentlyContinue
        if (-not $launcher) { throw 'Install uv or Python 3.12 to create the Gateway environment.' }
        & $launcher.Source -3.12 -m venv (Join-Path $PSScriptRoot '.venv')
    }
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create Gateway .venv.' }
}

$uv = Get-Command uv -ErrorAction SilentlyContinue
if ($uv) {
    $uvArgs = @('pip', 'install', '--python', $python, '--requirement', $requirements)
    if ($Upgrade) { $uvArgs += '--upgrade' }
    & $uv.Source @uvArgs
} else {
    & $python -m ensurepip --upgrade
    if ($LASTEXITCODE -ne 0) { throw 'pip bootstrap failed; install uv and rerun bootstrap.ps1.' }
    if ($Upgrade) { & $python -m pip install --upgrade --requirement $requirements }
    else { & $python -m pip install --requirement $requirements }
}
if ($LASTEXITCODE -ne 0) { throw 'Failed to install Gateway requirements.' }
& $python (Join-Path $PSScriptRoot 'check_env.py')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "Gateway environment ready: $python"
