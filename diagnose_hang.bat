@echo off
setlocal enabledelayedexpansion

echo ========================================
echo eFlash FTL Tests - 分阶段超时诊断
echo ========================================
echo.

set TEST_EXE=build_vs2022\Release\eflash_ftl_tests.exe
set LOG_DIR=test_logs

if not exist %TEST_EXE% (
    echo ERROR: Test executable not found!
    echo Please run build_vs2022.bat first.
    exit /b 1
)

if not exist %LOG_DIR% mkdir %LOG_DIR%

:: 阶段1: 5秒超时 - 检查是否卡在初始化
echo [Phase 1] Testing with 5s timeout (checking init phase)...
powershell -Command "& { 
    $process = Start-Process -FilePath '%TEST_EXE%' -PassThru -NoNewWindow -RedirectStandardOutput '%LOG_DIR%\test_5s.log' -RedirectStandardError '%LOG_DIR%\test_5s_err.log';
    $timeout = 5;
    $watch = [System.Diagnostics.Stopwatch]::StartNew();
    Write-Host 'Waiting up to ${timeout}s...';
    
    while (!$process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 100;
    }
    
    if (!$process.HasExited) {
        Write-Host 'TIMEOUT: Killing process after ${timeout}s' -ForegroundColor Red;
        Stop-Process -Id $process.Id -Force;
        Write-Host 'Check log: %LOG_DIR%\test_5s.log' -ForegroundColor Yellow;
    } else {
        Write-Host 'Completed within ${timeout}s' -ForegroundColor Green;
    }
}"

echo.
type %LOG_DIR%\test_5s.log | findstr /C:"[TEST]" /C:"[PASS]" /C:"[FAIL]" /C:"Starting"
echo.

:: 阶段2: 15秒超时 - 检查是否卡在某个具体测试
echo [Phase 2] Testing with 15s timeout...
powershell -Command "& { 
    $process = Start-Process -FilePath '%TEST_EXE%' -PassThru -NoNewWindow -RedirectStandardOutput '%LOG_DIR%\test_15s.log' -RedirectStandardError '%LOG_DIR%\test_15s_err.log';
    $timeout = 15;
    $watch = [System.Diagnostics.Stopwatch]::StartNew();
    Write-Host 'Waiting up to ${timeout}s...';
    
    while (!$process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 100;
    }
    
    if (!$process.HasExited) {
        Write-Host 'TIMEOUT: Killing process after ${timeout}s' -ForegroundColor Red;
        Stop-Process -Id $process.Id -Force;
        Write-Host 'Check log: %LOG_DIR%\test_15s.log' -ForegroundColor Yellow;
    } else {
        Write-Host 'Completed within ${timeout}s' -ForegroundColor Green;
    }
}"

echo.
echo === Last 20 lines of 15s log ===
powershell -Command "Get-Content '%LOG_DIR%\test_15s.log' -Tail 20"
echo.

:: 阶段3: 30秒超时 - 完整运行或捕获长时间卡死
echo [Phase 3] Testing with 30s timeout (full run or long hang detection)...
powershell -Command "& { 
    $process = Start-Process -FilePath '%TEST_EXE%' -PassThru -NoNewWindow -RedirectStandardOutput '%LOG_DIR%\test_30s.log' -RedirectStandardError '%LOG_DIR%\test_30s_err.log';
    $timeout = 30;
    $watch = [System.Diagnostics.Stopwatch]::StartNew();
    Write-Host 'Waiting up to ${timeout}s...';
    
    while (!$process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 100;
    }
    
    if (!$process.HasExited) {
        Write-Host 'TIMEOUT: Killing process after ${timeout}s' -ForegroundColor Red;
        Stop-Process -Id $process.Id -Force;
        Write-Host 'Test is hanging! Check logs for details.' -ForegroundColor Red;
    } else {
        Write-Host 'Test completed successfully!' -ForegroundColor Green;
    }
}"

echo.
echo === Final Results Summary ===
type %LOG_DIR%\test_30s.log | findstr /C:"Test Results" /C:"passed" /C:"failed"
echo.

echo Logs saved to: %LOG_DIR%\
dir %LOG_DIR%\*.log

endlocal
pause