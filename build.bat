@echo off
setlocal
pushd "%~dp0"

where cl.exe >nul 2>&1
if not errorlevel 1 goto compiler_ready
set "KEYFLOW_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%KEYFLOW_VSWHERE%" goto no_compiler
for /f "usebackq tokens=*" %%i in (`"%KEYFLOW_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "KEYFLOW_VS=%%i"
if not defined KEYFLOW_VS goto no_compiler
call "%KEYFLOW_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto failed

:compiler_ready
if not exist build mkdir build
if /i "%~1"=="test" goto tests
if /i "%~1"=="analyze" set "KEYFLOW_ANALYZE=/analyze"

echo [BUILD] Compiling resources ...
rc /nologo /fo build\macos-keyflow.res macos-keyflow.rc
if errorlevel 1 goto failed
echo [BUILD] Compiling macos-keyflow.exe ...
cl /nologo /O1 /Os /GS /W4 /WX /utf-8 /Zi %KEYFLOW_ANALYZE% /Fo:build\macos-keyflow.obj /Fd:build\compiler.pdb ^
   macos-keyflow.c build\macos-keyflow.res ^
   /link /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG /PDB:build\macos-keyflow.pdb ^
   /OUT:macos-keyflow.exe
if errorlevel 1 goto failed
for %%f in (macos-keyflow.exe) do echo [OK] macos-keyflow.exe: %%~zf bytes
goto success

:tests
cl /nologo /O1 /GS /W4 /WX /utf-8 /Fo:build\regression.obj tests\regression.c ^
   /link /SUBSYSTEM:CONSOLE /OUT:build\regression.exe
if errorlevel 1 goto failed
build\regression.exe
if errorlevel 1 goto failed
goto success

:no_compiler
echo [FAILED] Install Visual Studio C++ Build Tools or run in a Developer Command Prompt.
goto failed

:failed
popd
exit /b 1

:success
popd
exit /b 0
