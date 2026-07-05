Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:\MSYS -ErrorAction SilentlyContinue
Remove-Item Env:\MINGW -ErrorAction SilentlyContinue

$env:IDF_PATH = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2'
$python = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$idf_py = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py'

# Make sure 'python' resolves to the IDF venv Python (needed by export.ps1)
$env:PATH = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:PATH

& "$env:IDF_PATH\export.ps1"
& $python $idf_py flash monitor -p COM5
