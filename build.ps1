$ErrorActionPreference = 'Continue'
$env:MSYSTEM = $null

$idfPath = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2'
$env:IDF_PATH = $idfPath
$python = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$activate_script = "$idfPath\tools\activate.py"

$activateOut = & $python $activate_script -s cmd.exe --export --quiet 2>&1
$activateOut | Set-Content -Path "$PSScriptRoot\build_export.log"

$batFile = $null
foreach ($line in $activateOut) {
    if ($line -match '^call\s+(.+\.bat)\s*$') {
        $batFile = $matches[1]
        break
    }
}

if (-not $batFile) {
    Write-Error "Failed to find activate batch file in output"
    exit 1
}

if (-not (Test-Path $batFile)) {
    Write-Error "Activate batch file not found: $batFile"
    exit 1
}

$cmdLine = '"' + $batFile + '" && python.exe "' + $idfPath + '\tools\idf.py" fullclean build'
Write-Host "Running: cmd /c $cmdLine"
cmd /c $cmdLine
exit $LASTEXITCODE
