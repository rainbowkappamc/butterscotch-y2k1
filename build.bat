@echo off
setlocal

echo ================================
echo  Select a build target:
echo ================================
echo  1. Nintendo 64 (32mb Cart)
echo  2. Nintendo 64 Disk Drive (64mb Disk)
echo  3. Windows (32 Bit)
echo  4. Windows (64 Bit)
echo ================================
set /p CHOICE="Enter target: "

if "%CHOICE%"=="1" goto BUILD_N64
if "%CHOICE%"=="2" goto BUILD_N64DD
if "%CHOICE%"=="3" goto BUILD_WIN32
if "%CHOICE%"=="4" goto BUILD_WIN64
echo ERROR: Invalid option "%CHOICE%".
pause
exit /b 1

:BUILD_N64
echo [N64] Building Nintendo 64 ROM...
make n64
if errorlevel 1 goto FAIL
goto DONE

:BUILD_N64DD
echo [N64DD] Building Nintendo 64 Disk Drive ROM...
make n64dd
if errorlevel 1 goto FAIL
goto DONE

:BUILD_WIN32
echo [WIN32] Configuring with CMake (32-bit)...
set PATH=C:\msys64\mingw32\bin;%PATH%
rmdir /s /q build\win32 2>nul
"C:\Program Files\CMake\bin\cmake.exe" -B build\win32 -G "MinGW Makefiles" -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:\msys64\mingw32\bin\gcc.exe -DCMAKE_C_FLAGS="-m32"
if errorlevel 1 goto FAIL
echo [WIN32] Building...
"C:\Program Files\CMake\bin\cmake.exe" --build build\win32
if errorlevel 1 goto FAIL
goto DONE

:BUILD_WIN64
echo [WIN64] Configuring with CMake (64-bit)...
set PATH=C:\msys64\ucrt64\bin;%PATH%
"C:\Program Files\CMake\bin\cmake.exe" -B build\win64 -G "MinGW Makefiles" -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto FAIL
echo [WIN64] Building...
"C:\Program Files\CMake\bin\cmake.exe" --build build\win64
if errorlevel 1 goto FAIL
goto DONE

:FAIL
echo.
echo ERROR: Build failed.
pause
exit /b 1

:DONE
echo.
echo Build complete.
pause
endlocal
