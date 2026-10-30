$ErrorActionPreference = 'Stop'
$python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
if (-not (Test-Path $python)) { throw 'Run .\gateway\bootstrap.ps1 first.' }
& $python (Join-Path $PSScriptRoot 'check_env.py')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $python -m pytest -q
exit $LASTEXITCODE
