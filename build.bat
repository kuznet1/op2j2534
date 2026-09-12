@echo off
rem Build op2j2534.dll for the Tactrix OpenPort 2.0 (32-bit and 64-bit).
setlocal enabledelayedexpansion

set ROOT=%~dp0
rem %ProgramFiles(x86)% contains parentheses, which terminate a parenthesised
rem IF/FOR block early. Capture it once at top level, then use !PF86! inside
rem blocks so delayed expansion hides the parentheses from the parser.
set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="

if exist "!VSWHERE!" for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH for %%e in (Community Professional Enterprise BuildTools) do (
    for %%v in (18 2022 2019) do (
        if not defined VSPATH if exist "!PF64!\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvarsall.bat" set "VSPATH=!PF64!\Microsoft Visual Studio\%%v\%%e"
        if not defined VSPATH if exist "!PF86!\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvarsall.bat" set "VSPATH=!PF86!\Microsoft Visual Studio\%%v\%%e"
    )
)

if not defined VSPATH (
    echo ERROR: Visual Studio not found.
    exit /b 1
)
set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvarsall.bat"
echo Using !VSPATH!

set SRC=%ROOT%src\op2_port.c %ROOT%src\op2_proto.c %ROOT%src\j2534_api.c
set CFLAGS=/nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /I"%ROOT%include"
set LIBS=setupapi.lib advapi32.lib
set DEF=/DEF:"%ROOT%src\op2j2534.def"

if not exist "%ROOT%build" mkdir "%ROOT%build"

echo === 32-bit ===
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1
pushd "%ROOT%build"
if not exist x86 mkdir x86
cl %CFLAGS% /LD /Fe:x86\op2j2534.dll /Fo:x86\ %SRC% /link %DEF% %LIBS%
if errorlevel 1 (popd & echo 32-bit build FAILED & exit /b 1)
popd

echo === 64-bit ===
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 1
pushd "%ROOT%build"
if not exist x64 mkdir x64
cl %CFLAGS% /LD /Fe:x64\op2j2534_64.dll /Fo:x64\ %SRC% /link %DEF% %LIBS%
if errorlevel 1 (popd & echo 64-bit build FAILED & exit /b 1)
popd

echo.
echo Built:
echo   %ROOT%build\x86\op2j2534.dll
echo   %ROOT%build\x64\op2j2534_64.dll
endlocal
