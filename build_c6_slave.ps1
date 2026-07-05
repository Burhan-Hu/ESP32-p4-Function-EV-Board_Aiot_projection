Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:\MSYS -ErrorAction SilentlyContinue
Remove-Item Env:\MINGW -ErrorAction SilentlyContinue

$env:IDF_PATH = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2'
$python = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$idf_py = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py'

# Make sure 'python' resolves to the IDF venv Python
$env:PATH = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:PATH

& "$env:IDF_PATH\export.ps1"

# Ensure toolchain is in PATH
$toolchain = 'E:\Tools\ESP-IDF\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin'
$env:PATH = $toolchain + ';' + $env:PATH

$slave_dir = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\esp_workspace\aiot_agent\managed_components\espressif__esp_hosted\slave'
Set-Location $slave_dir

& $python $idf_py -B E:\c6build set-target esp32c6 build
