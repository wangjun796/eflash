@echo off
setlocal enabledelayedexpansion

echo ========================================
echo eFlash FTL Tests with Timeout
echo ========================================
echo.

set TEST_EXE=build_vs2022\Release\eflash_ftl_tests.exe
set TIMEOUT_SEC=10

if not exist %TEST_EXE% (
    echo ERROR: Test executable not found!
    echo Please run build_vs2022.bat first.
    exit /b 1
)

echo Starting test with %TIMEOUT_SEC%s timeout...
echo.

:: Run test with timeout using PowerShell
powershell -Command "& { 
    $process = Start-Process -FilePath '%TEST_EXE%' -PassThru -NoNewWindow;
    $timeout = %TIMEOUT_SEC%;
    $watch = [System.Diagnostics.Stopwatch]::StartNew();
    
    while (!$process.HasExited -and $watch.Elapsed.TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 100;
    }
    
    if (!$process.HasExited) {
        Write-Host 'TIMEOUT: Test exceeded ${timeout}s, killing process...' -ForegroundColor Red;
        Stop-Process -Id $process.Id -Force;
        exit 1;
    } else {
        Write-Host 'Test completed within timeout' -ForegroundColor Green;
        exit $process.ExitCode;
    }
}"

echo.
echo Exit code: %ERRORLEVEL%
endlocal