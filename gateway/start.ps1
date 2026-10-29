$ErrorActionPreference = 'Stop'
$env:ICEBREAKER_MOCK = '0'
$env:SILICONFLOW_ASR_MODEL = 'FunAudioLLM/SenseVoiceSmall'
$env:SILICONFLOW_MODEL = 'deepseek-ai/DeepSeek-R1-0528-Qwen3-8B'

python -u (Join-Path $PSScriptRoot 'server.py') --host 0.0.0.0 --port 8765
exit $LASTEXITCODE
