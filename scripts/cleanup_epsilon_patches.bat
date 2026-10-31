@echo off
rem ============================================================================
rem  cleanup_epsilon_patches.bat - enforce the single-source rule in Patches\
rem
rem  The Epsilon addon serves ANY loose file it finds under Patches\ to the
rem  client by remapped FileDataID. When the same tile exists in more than
rem  one folder (e.g. Patches\azeroth\world\maps\azeroth\ AND a flat
rem  Patches\testnewmap\ copy from an old test), the client can be handed the
rem  STALE copy - that is how a tile repaired in Noggit can still crash the
rem  client in-game (2026-10-24, azeroth_32_49).
rem
rem  This script MOVES (never deletes) every known conflicting source into
rem  Patches\_quarantine\<timestamp>\ and leaves ONLY the install target
rem  Patches\azeroth\ standing. Run it with the game closed.
rem
rem  Usage:  cleanup_epsilon_patches.bat [PatchesDir]
rem          default PatchesDir = G:\epsilon\Patches
rem ============================================================================
setlocal EnableExtensions

set "PATCHES=%~1"
if "%PATCHES%"=="" set "PATCHES=G:\epsilon\Patches"

if not exist "%PATCHES%\" (
  echo Patches directory not found: "%PATCHES%"
  echo Pass the correct path as the first argument.
  exit /b 1
)

for /f "tokens=2 delims==" %%T in ('wmic os get localdatetime /value ^| find "="') do set "STAMP=%%T"
set "STAMP=%STAMP:~0,8%_%STAMP:~8,6%"
set "QUARANTINE=%PATCHES%\_quarantine\%STAMP%"

echo Patches directory : %PATCHES%
echo Quarantine target : %QUARANTINE%
echo.

set "MOVED=0"

rem --- known conflicting folders (flat test exports and foreign modpacks) ---
call :quarantine "testnewmap"
call :quarantine "testaz"
call :quarantine "world"

rem --- any Chronicles modpack folder (same map id, foreign files) ---
for /d %%D in ("%PATCHES%\*Chronicles*") do call :quarantine_path "%%~D"
for /d %%D in ("%PATCHES%\!*") do call :quarantine_path "%%~D"

rem --- loose map files sitting directly in Patches\ root ---
for %%E in (adt wdt wdl blp) do (
  for %%F in ("%PATCHES%\*.%%E") do (
    if not exist "%QUARANTINE%\" mkdir "%QUARANTINE%"
    echo Moving loose file %%~nxF
    move /-Y "%%~F" "%QUARANTINE%\" >nul
    set /a MOVED+=1
  )
)

echo.
if "%MOVED%"=="0" (
  echo Nothing to quarantine - Patches\ already follows the single-source rule.
) else (
  echo Moved %MOVED% conflicting item^(s^) into %QUARANTINE%
  echo Nothing was deleted; move items back if one turns out to be needed.
)
echo.
echo Remaining top-level entries in Patches\ :
dir /b "%PATCHES%"
echo.
echo The only map source should now be: %PATCHES%\azeroth\world\maps\azeroth\
pause
exit /b 0

:quarantine
call :quarantine_path "%PATCHES%\%~1"
exit /b 0

:quarantine_path
if exist "%~1\" (
  if not exist "%QUARANTINE%\" mkdir "%QUARANTINE%"
  echo Moving folder %~nx1
  move /-Y "%~1" "%QUARANTINE%\" >nul
  set /a MOVED+=1
) else if exist "%~1" (
  if not exist "%QUARANTINE%\" mkdir "%QUARANTINE%"
  echo Moving file %~nx1
  move /-Y "%~1" "%QUARANTINE%\" >nul
  set /a MOVED+=1
)
exit /b 0
