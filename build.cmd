@echo off
rem ============================================================================
rem  ShadowWorld Noggit Modern - automatic local build (MSVC + Qt 5.15)
rem
rem  Usage:
rem    build.cmd [release|debug|relwithdebinfo] [clean] [run] [help]
rem
rem      release / debug / relwithdebinfo   build type (default: release)
rem      clean                              delete the build directory first
rem      run                                start noggit.exe after a good build
rem      help                               show this text
rem
rem  Environment overrides:
rem    QT_DIR              Qt 5.15 kit folder (auto-detected when omitted)
rem    BUILD_DIR           build output folder (default: build)
rem    BUILD_GENERATOR     CMake generator (default: Ninja when available)
rem    JOBS                parallel jobs (default: number of CPUs)
rem    NOGGIT_PCH          set to OFF to disable the noggit precompiled header
rem    CMAKE_EXTRA_ARGS    extra arguments forwarded to the configure step
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || goto :err_cd
if not defined TEMP set "TEMP=%ROOT%"
set "TMPFILE=%TEMP%\noggit_build_tmp.txt"

if not defined BUILD_DIR set "BUILD_DIR=build"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined TARGET set "TARGET=noggit"
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined NOGGIT_PCH set "NOGGIT_PCH=ON"
set "DO_CLEAN=0"
set "DO_RUN=0"

rem ------------------------------- arguments ---------------------------------
:parse_args
if "%~1"=="" goto args_done
set "ARG=%~1"
if /i "!ARG!"=="help" goto :usage
if /i "!ARG!"=="--help" goto :usage
if /i "!ARG!"=="/?" goto :usage
if /i "!ARG!"=="clean" (set "DO_CLEAN=1" & shift & goto :parse_args)
if /i "!ARG!"=="run" (set "DO_RUN=1" & shift & goto :parse_args)
if /i "!ARG!"=="debug" (set "BUILD_TYPE=Debug" & shift & goto :parse_args)
if /i "!ARG!"=="release" (set "BUILD_TYPE=Release" & shift & goto :parse_args)
if /i "!ARG!"=="relwithdebinfo" (set "BUILD_TYPE=RelWithDebInfo" & shift & goto :parse_args)
echo [build] ERROR: unknown argument "!ARG!"
call :usage
endlocal & exit /b 1
:args_done

echo.
echo ==============================================================
echo  ShadowWorld Noggit Modern - build
echo   root       : !ROOT!
echo   build dir  : !BUILD_DIR!
echo   build type : !BUILD_TYPE!
echo   target     : !TARGET!
echo ==============================================================
echo.

rem ------------------------------- toolchain --------------------------------
call :find_msvc
if errorlevel 1 goto :err_msvc

call :find_cmake
if errorlevel 1 goto :err_cmake

call :find_ninja

call :find_qt
if errorlevel 1 goto :err_qt

if "!DO_CLEAN!"=="1" (
  echo [build] Cleaning "!BUILD_DIR!" ...
  if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
)

call :fetch_submodules
if errorlevel 1 goto :err_submodules

rem --------------------------- generator choice -----------------------------
if defined BUILD_GENERATOR (
  set "GEN=!BUILD_GENERATOR!"
) else if defined NINJA_PATH (
  set "GEN=Ninja"
) else (
  set "GEN=Visual Studio 17 2022"
)
set "PLATFORM_ARGS="
if /i not "!GEN!"=="Ninja" set "PLATFORM_ARGS=-A x64"
echo [build] Generator: !GEN!

rem ------------------------------- configure --------------------------------
set "QT_ARGS="
if defined QT_PREFIX (
  set QT_ARGS=-DQt5_DIR="!QT_PREFIX!\lib\cmake\Qt5" -DCMAKE_PREFIX_PATH="!QT_PREFIX!"
) else (
  echo [build] Qt will be located through the ambient CMAKE_PREFIX_PATH.
)

echo [build] Configuring ...
"%CMAKE_EXE%" -S "!ROOT!" -B "!BUILD_DIR!" -G "!GEN!" !PLATFORM_ARGS! ^
  -DCMAKE_BUILD_TYPE=!BUILD_TYPE! ^
  -DNOGGIT_OPENGL_ERROR_CHECK=OFF ^
  -DFAST_BUILD_NOGGIT_PCH=!NOGGIT_PCH! ^
  !QT_ARGS! !CMAKE_EXTRA_ARGS!
if errorlevel 1 goto :err_configure

rem --------------------------------- build ----------------------------------
echo.
echo [build] Building target "!TARGET!" with !JOBS! jobs ...
"%CMAKE_EXE%" --build "!BUILD_DIR!" --target !TARGET! --config !BUILD_TYPE! --parallel !JOBS!
if errorlevel 1 goto :err_build

rem -------------------------------- report ----------------------------------
set "EXE_PATH="
if exist "!BUILD_DIR!\bin\!TARGET!.exe" set "EXE_PATH=!BUILD_DIR!\bin\!TARGET!.exe"
if not defined EXE_PATH if exist "!BUILD_DIR!\bin\!BUILD_TYPE!\!TARGET!.exe" set "EXE_PATH=!BUILD_DIR!\bin\!BUILD_TYPE!\!TARGET!.exe"
if not defined EXE_PATH if exist "!BUILD_DIR!\bin\Debug\!TARGET!.exe" set "EXE_PATH=!BUILD_DIR!\bin\Debug\!TARGET!.exe"

echo.
echo ==============================================================
if defined EXE_PATH (
  for %%f in ("!EXE_PATH!") do echo  Build succeeded: %%~ff  [%%~zf bytes]
) else (
  echo  Build succeeded, but no freshly built !TARGET!.exe was found.
  echo  Look in "!BUILD_DIR!\bin".
)
echo ==============================================================
echo.

if "!DO_RUN!"=="1" (
  if defined EXE_PATH (
    echo [build] Starting "!EXE_PATH!" ...
    start "" "!EXE_PATH!"
  ) else (
    echo [build] Cannot run: executable was not found.
  )
)

endlocal & exit /b 0

rem ============================================================================
rem  Subroutines
rem ============================================================================

:find_msvc
rem Initialises the MSVC x64 environment unless it is already set up.
if defined VSCMD_ARG_TGT_ARCH (
  echo [build] MSVC environment already initialised ^(target !VSCMD_ARG_TGT_ARCH!^).
  exit /b 0
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" call :vswhere_lookup
if not defined VSPATH call :vs_path_scan
if not defined VSPATH exit /b 1
if not exist "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" exit /b 1
echo [build] MSVC environment: !VSPATH!
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
exit /b 0

:vswhere_lookup
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
  if not defined VSPATH set "VSPATH=%%i"
)
exit /b 0

:vs_path_scan
for %%d in (
  "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\18\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\18\Community"
  "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
) do (
  if not defined VSPATH if exist "%%~d\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~d"
)
exit /b 0

:find_cmake
set "CMAKE_EXE="
for /f "delims=" %%c in ('where cmake 2^>nul') do (
  if not defined CMAKE_EXE set "CMAKE_EXE=%%c"
)
if not defined CMAKE_EXE if defined VSPATH (
  if exist "!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
)
if not defined CMAKE_EXE exit /b 1
set "CMAKE_VER="
"!CMAKE_EXE!" --version > "%TMPFILE%" 2>nul
if exist "%TMPFILE%" for /f "usebackq tokens=3" %%v in ("%TMPFILE%") do if not defined CMAKE_VER set "CMAKE_VER=%%v"
if exist "%TMPFILE%" del /q "%TMPFILE%" >nul 2>&1
echo [build] CMake: !CMAKE_EXE!  ^(version !CMAKE_VER!^)
if "!CMAKE_VER:~0,2!"=="4." (
  echo [build] WARNING: CMake 4.x rejects some of this project's dependencies.
  echo [build]          Use CMake 3.31.x if the configure step below fails.
)
exit /b 0

:find_ninja
set "NINJA_PATH="
for /f "delims=" %%n in ('where ninja 2^>nul') do (
  if not defined NINJA_PATH set "NINJA_PATH=%%n"
)
if not defined NINJA_PATH if defined VSPATH (
  if exist "!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set "NINJA_PATH=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    set "PATH=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;!PATH!"
  )
)
if defined NINJA_PATH echo [build] Ninja: !NINJA_PATH!
exit /b 0

:find_qt
set "QT_PREFIX="
if defined QT_DIR call :try_qt_dir "%QT_DIR%"
if not defined QT_PREFIX if defined QT_ROOT_DIR call :try_qt_dir "%QT_ROOT_DIR%"
if not defined QT_PREFIX if defined QTDIR call :try_qt_dir "%QTDIR%"
if not defined QT_PREFIX call :qt_from_qmake
if not defined QT_PREFIX call :qt_root_scan
if not defined QT_PREFIX (
  if defined CMAKE_PREFIX_PATH (
    echo [build] No Qt kit auto-detected; relying on CMAKE_PREFIX_PATH.
    exit /b 0
  )
  exit /b 1
)
echo [build] Qt: !QT_PREFIX!
call :qt_version_check
if errorlevel 1 exit /b 1
exit /b 0

:try_qt_dir
if not defined QT_PREFIX if exist "%~1\lib\cmake\Qt5\Qt5Config.cmake" set "QT_PREFIX=%~1"
exit /b 0

:qt_from_qmake
set "QMAKE_EXE="
for /f "delims=" %%q in ('where qmake 2^>nul') do (
  if not defined QMAKE_EXE set "QMAKE_EXE=%%q"
)
if not defined QMAKE_EXE exit /b 0
"!QMAKE_EXE!" -query QT_INSTALL_PREFIX > "%TMPFILE%" 2>nul
if not exist "%TMPFILE%" exit /b 0
set "PREFIX_FROM_QMAKE="
set /p PREFIX_FROM_QMAKE=<"%TMPFILE%" 2>nul
del /q "%TMPFILE%" >nul 2>&1
if defined PREFIX_FROM_QMAKE call :try_qt_dir "!PREFIX_FROM_QMAKE!"
exit /b 0

:qt_root_scan
for %%r in ("C:\Qt" "D:\Qt" "%USERPROFILE%\Qt") do call :scan_qt_root "%%~r"
exit /b 0

:scan_qt_root
if defined QT_PREFIX exit /b 0
if not exist "%~1" exit /b 0
for /d %%k in ("%~1\5.15*") do (
  if not defined QT_PREFIX call :try_qt_dir "%%~k\msvc2019_64"
  if not defined QT_PREFIX call :try_qt_dir "%%~k\msvc2019"
)
exit /b 0

:qt_version_check
set "QT_VER="
"!QT_PREFIX!\bin\qmake.exe" -query QT_VERSION > "%TMPFILE%" 2>nul
if exist "%TMPFILE%" set /p QT_VER=<"%TMPFILE%" 2>nul
if exist "%TMPFILE%" del /q "%TMPFILE%" >nul 2>&1
if not defined QT_VER exit /b 0
echo [build] Qt version: !QT_VER!
if not "!QT_VER:~0,2!"=="5." (
  echo [build] ERROR: this project requires Qt 5.15 but found !QT_VER!.
  exit /b 1
)
exit /b 0

:fetch_submodules
if not exist "cmake\cmake_function.cmake" (
  echo [build] Initialising required submodules ...
  git submodule update --init cmake src/external/blizzard-archive-library src/external/blizzard-database-library dist/definitions
  if errorlevel 1 exit /b 1
)
if not exist "dist\definitions\." mkdir "dist\definitions" >nul 2>&1
if not exist "dist\themes\." (
  echo [build] NOTE: dist\themes is missing; creating an empty folder so the post-build copy step succeeds.
  mkdir "dist\themes" >nul 2>&1
)
exit /b 0

rem ============================================================================
rem  Errors and usage
rem ============================================================================

:err_cd
echo [build] ERROR: cannot change directory to the repository root.
endlocal & exit /b 1

:err_msvc
echo.
echo [build] ERROR: no Visual Studio installation with the C++ x64 toolchain was found.
echo [build]        Install "Desktop development with C++" ^(VS 2022 or newer^) and try again.
endlocal & exit /b 1

:err_cmake
echo.
echo [build] ERROR: cmake was not found on PATH and Visual Studio does not ship one.
echo [build]        Install CMake 3.31.x and make sure "cmake" is on PATH.
endlocal & exit /b 1

:err_qt
echo.
echo [build] ERROR: Qt 5.15 was not found.
echo [build]        Point QT_DIR at a Qt 5.15 MSVC 2019 64-bit kit, for example:
echo [build]          set QT_DIR=C:\Qt\5.15.2\msvc2019_64
endlocal & exit /b 1

:err_submodules
echo.
echo [build] ERROR: fetching the required submodules failed.
echo [build]        Run: git submodule update --init --recursive
endlocal & exit /b 1

:err_configure
echo.
echo [build] ERROR: the CMake configure step failed; see the messages above.
endlocal & exit /b 1

:err_build
echo.
echo [build] ERROR: the build failed; see the compiler output above.
endlocal & exit /b 1

:usage
echo.
echo ShadowWorld Noggit Modern - automatic build
echo.
echo   build.cmd [release^|debug^|relwithdebinfo] [clean] [run] [help]
echo.
echo   release         build the Release configuration ^(default^)
echo   debug           build the Debug configuration
echo   relwithdebinfo  optimised build with debug symbols
echo   clean           remove the build directory before configuring
echo   run             start noggit.exe after a successful build
echo.
echo Environment overrides:
echo   QT_DIR  BUILD_DIR  BUILD_GENERATOR  JOBS  NOGGIT_PCH  CMAKE_EXTRA_ARGS
exit /b 0
