@echo off
rem Build script for Visual Studio 2022.
rem Run this from a "x64 Native Tools Command Prompt for VS 2022".

@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

setlocal enabledelayedexpansion

where cl >nul 2>&1
if errorlevel 1 (
    echo Error: cl.exe not found on PATH.
    echo Please run this script from a "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

set CFLAGS=/nologo /W3 /MD /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /Iinclude /Isrc
set LIBS=ws2_32.lib

if not exist build mkdir build

set SRCS=src\rpc.c src\ws.c src\event_loop.c src\protocol.c src\compat.c
set OBJS=

for %%f in (%SRCS%) do (
    echo Compiling %%f
    cl %CFLAGS% /c %%f /Fobuild\%%~nf.obj
    if errorlevel 1 exit /b 1
    set OBJS=!OBJS! build\%%~nf.obj
)

echo Linking server.exe
cl %CFLAGS% examples\server.c !OBJS! /Feserver.exe /link %LIBS%
if errorlevel 1 exit /b 1

echo Linking client.exe
cl %CFLAGS% examples\client.c !OBJS! /Feclient.exe /link %LIBS%
if errorlevel 1 exit /b 1

rem Clean up linker artifacts left in cwd
del /Q server.obj client.obj 2>nul

echo.
echo Build complete: server.exe client.exe
endlocal
