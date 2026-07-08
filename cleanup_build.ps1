$proj = 'E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\esp_workspace\aiot_agent'
Write-Host "Cleaning up build locks for $proj"

$targets = @('python', 'ninja', 'cmake', 'powershell')
$killed = 0

Get-Process | Where-Object { $_.ProcessName -in $targets } | ForEach-Object {
    try {
        $cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction Stop).CommandLine
        if ($cmd -like "*$proj*") {
            Write-Host "Stopping locked process: $($_.ProcessName) (PID $($_.Id))"
            Stop-Process -Id $_.Id -Force -ErrorAction Stop
            $killed++
        }
    } catch {
        Write-Host "Could not inspect/stop PID $($_.Id): $_"
    }
}

if ($killed -eq 0) {
    Write-Host "No locking processes found with project path in command line."
}

Start-Sleep -Seconds 2

$buildDir = Join-Path $proj 'build'
if (Test-Path $buildDir) {
    try {
        Remove-Item -Recurse -Force $buildDir -ErrorAction Stop
        Write-Host "Deleted build directory."
    } catch {
        Write-Error "Failed to delete build directory: $_"
        exit 1
    }
} else {
    Write-Host "build directory already absent."
}

Write-Host "Cleanup complete. You can now run: idf.py fullclean build flash monitor"
