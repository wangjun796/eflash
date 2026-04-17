# eFlash FTL Tests - 分阶段超时诊断脚本

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "eFlash FTL Tests - 分阶段超时诊断" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

$TEST_EXE = "build_vs2022\Release\eflash_ftl_tests.exe"
$LOG_DIR = "test_logs"

if (-not (Test-Path $TEST_EXE)) {
    Write-Host "ERROR: Test executable not found!" -ForegroundColor Red
    Write-Host "Please run build_vs2022.bat first." -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $LOG_DIR)) {
    New-Item -ItemType Directory -Path $LOG_DIR | Out-Null
}

# 阶段1: 5秒超时 - 检查是否卡在初始化
Write-Host "[Phase 1] Testing with 5s timeout (checking init phase)..." -ForegroundColor Yellow
$process = Start-Process -FilePath $TEST_EXE -PassThru -NoNewWindow `
    -RedirectStandardOutput "$LOG_DIR\test_5s.log" `
    -RedirectStandardError "$LOG_DIR\test_5s_err.log"

$timeout = 5
$watch = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "Waiting up to ${timeout}s..."

while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
    Start-Sleep -Milliseconds 100
}

if (-not $process.HasExited) {
    Write-Host "TIMEOUT: Killing process after ${timeout}s" -ForegroundColor Red
    Stop-Process -Id $process.Id -Force
    Write-Host "Check log: $LOG_DIR\test_5s.log" -ForegroundColor Yellow
} else {
    Write-Host "Completed within ${timeout}s" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Phase 1 Output (key lines) ===" -ForegroundColor Cyan
Get-Content "$LOG_DIR\test_5s.log" | Select-String "\[TEST\]|\[PASS\]|\[FAIL\]|Starting" | Select-Object -Last 10
Write-Host ""

# 阶段2: 15秒超时 - 检查是否卡在某个具体测试
Write-Host "[Phase 2] Testing with 15s timeout..." -ForegroundColor Yellow
$process = Start-Process -FilePath $TEST_EXE -PassThru -NoNewWindow `
    -RedirectStandardOutput "$LOG_DIR\test_15s.log" `
    -RedirectStandardError "$LOG_DIR\test_15s_err.log"

$timeout = 15
$watch = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "Waiting up to ${timeout}s..."

while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
    Start-Sleep -Milliseconds 100
}

if (-not $process.HasExited) {
    Write-Host "TIMEOUT: Killing process after ${timeout}s" -ForegroundColor Red
    Stop-Process -Id $process.Id -Force
    Write-Host "Check log: $LOG_DIR\test_15s.log" -ForegroundColor Yellow
} else {
    Write-Host "Completed within ${timeout}s" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Last 20 lines of 15s log ===" -ForegroundColor Cyan
Get-Content "$LOG_DIR\test_15s.log" -Tail 20
Write-Host ""

# 阶段3: 30秒超时 - 完整运行或捕获长时间卡死
Write-Host "[Phase 3] Testing with 30s timeout (full run or long hang detection)..." -ForegroundColor Yellow
$process = Start-Process -FilePath $TEST_EXE -PassThru -NoNewWindow `
    -RedirectStandardOutput "$LOG_DIR\test_30s.log" `
    -RedirectStandardError "$LOG_DIR\test_30s_err.log"

$timeout = 30
$watch = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "Waiting up to ${timeout}s..."

while (-not $process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
    Start-Sleep -Milliseconds 100
}

if (-not $process.HasExited) {
    Write-Host "TIMEOUT: Killing process after ${timeout}s" -ForegroundColor Red
    Stop-Process -Id $process.Id -Force
    Write-Host "Test is hanging! Check logs for details." -ForegroundColor Red
} else {
    Write-Host "Test completed successfully!" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Final Results Summary ===" -ForegroundColor Cyan
Get-Content "$LOG_DIR\test_30s.log" | Select-String "Test Results|passed|failed" | Select-Object -Last 5
Write-Host ""

Write-Host "Logs saved to: $LOG_DIR\" -ForegroundColor Green
Get-ChildItem "$LOG_DIR\*.log" | Select-Object Name, Length, LastWriteTime

Write-Host ""
Write-Host "Press Enter to exit..."
Read-Host
