@echo off
if "%~1"=="" (
    echo Drag a .h file onto this batch file to open it in the viewer.
    pause
    exit /b 1
)

python "%~dp0h_viewer.py" "%~1"
