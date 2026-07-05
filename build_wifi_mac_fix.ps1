Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:\MSYS -ErrorAction SilentlyContinue
Remove-Item Env:\MINGW -ErrorAction SilentlyContinue

$env:IDF_PATH = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2'
$venv_python = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'

# Make sure 'python' resolves to the IDF venv Python
$env:PATH = 'E:\Tools\ESP-IDF\Espressif\python_env\idf5.5_py3.11_env\Scripts;' + $env:PATH

# Source ESP-IDF environment to get toolchain and other paths
& "$env:IDF_PATH\export.ps1"

$idf_py = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py'
& $venv_python $idf_py build
