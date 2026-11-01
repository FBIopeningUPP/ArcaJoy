@echo off
cd /d "%~dp0"
setlocal

rem Several Pythons can be installed; use the first one that already has pyserial,
rem otherwise install pyserial into the first working one.
set "PY="
for %%C in ("py -3" "python" "python3") do (
  if not defined PY (
    %%~C -c "import serial" >nul 2>&1 && set "PY=%%~C"
  )
)
if not defined PY (
  for %%C in ("py -3" "python" "python3") do (
    if not defined PY (
      %%~C -c "import sys" >nul 2>&1 && set "PY=%%~C"
    )
  )
  if not defined PY (
    echo Python 3 was not found. Install it from python.org ^(64-bit^) and tick "Add to PATH".
    pause
    exit /b 1
  )
  echo Installing pyserial...
  %PY% -m pip install pyserial
)

%PY% megajoy_bridge.py %*
pause
