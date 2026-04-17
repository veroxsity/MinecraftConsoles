@echo off
setlocal

set "DEST=E:\Minecraft.Client"
set "DEBUG_DIR=%~dp0build\windows64\Minecraft.Client\Debug"
set "RELEASE_EXE=%~dp0build\windows64\Minecraft.Client\Release\Minecraft.Client.exe"

echo [1/3] Building Minecraft.Client (Release)...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul 2>&1
cmake --build --preset windows64-release --target Minecraft.Client
if %errorlevel% neq 0 (
    echo.
    echo BUILD FAILED.
    pause
    exit /b 1
)

echo.
echo [2/3] Syncing game data to %DEST%...
if not exist "%DEST%" mkdir "%DEST%"

xcopy "%DEBUG_DIR%\*" "%DEST%\" /E /Y /I /EXCLUDE:%~dp0build-release-exclude.txt >nul
if %errorlevel% neq 0 (
    echo.
    echo XCOPY FAILED.
    pause
    exit /b 1
)

echo.
echo [3/3] Copying Release exe over Debug exe...
copy /y "%RELEASE_EXE%" "%DEST%\Minecraft.Client.exe"
if %errorlevel% neq 0 (
    echo.
    echo COPY FAILED.
    pause
    exit /b 1
)

echo.
echo Done. Game folder ready at %DEST%
pause
