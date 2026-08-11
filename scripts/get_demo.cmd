@echo off
REM get_demo.cmd -- Windows entry point for get_demo.ps1.
REM
REM Exists because a bare .ps1 is close to unrunnable for an ordinary user:
REM double-clicking one opens it in Notepad, and Windows client defaults to
REM the Restricted execution policy, which refuses to run it at all.  A .ps1
REM downloaded inside a release zip also carries the Mark of the Web, so even
REM RemoteSigned rejects it as unsigned-from-the-internet.  Bypass applies to
REM this one invocation only; it changes no machine setting.
REM
REM Usage: get_demo.cmd [destination]
REM   Defaults to the folder this script sits in -- in a release that is the
REM   folder holding hexenwail.exe, which is where the data belongs.

setlocal
set "DEST=%~1"
if "%DEST%"=="" set "DEST=%~dp0."

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get_demo.ps1" "%DEST%"
set "RC=%ERRORLEVEL%"

REM A double-clicked .cmd gets a console that closes the moment it exits,
REM taking the result message with it.  cmd.exe is launched with /c to run a
REM file and quit, which is exactly that case; an interactive prompt has no
REM /c on its own command line, so this does not pause when run by hand.
echo %CMDCMDLINE% | findstr /i /c:"/c" >nul && pause
exit /b %RC%
