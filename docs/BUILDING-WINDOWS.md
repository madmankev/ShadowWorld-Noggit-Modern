# Building on Windows

This repository builds a single `noggit.exe` that supports the WotLK,
Shadowlands, Retail and Forever project targets. The supported toolchain is
**MSVC + Qt 5.15** — see the notes at the end about other toolchains.

## Toolchain used by CI

`.github/workflows/windows-build.yml` builds the editor on a `windows-2022`
runner and uploads the deployed output as the `noggit-windows-x64` artifact.

| Component | Version |
| --- | --- |
| Runner | `windows-2022` |
| Compiler | MSVC x64 (`ilammy/msvc-dev-cmd`) |
| CMake | 3.31.6 (pinned — the FetchContent dependencies predate CMake 4 policies) |
| Generator | Ninja |
| Qt | 5.15.2 `win64_msvc2019_64` (installed by `jurplel/install-qt-action`) |
| Std | C++20 |

Trigger it with **Actions → Windows Build → Run workflow**, by pushing to the
branch configured in the workflow, or with `gh workflow run windows-build.yml`.
The **unity build** input toggles `FAST_BUILD_NOGGIT_JUMBO`.

`FAST_BUILD_LIB_JUMBO` is deliberately **off**: the unity TUs break on
Qt 5.15.2 headers because `stdext::checked_array_iterator` is used before
`<iterator>` has been pulled in, which newer MSVC STLs reject with
`error C3861: 'stdext': identifier not found`.

## Building locally

Prerequisites:

1. Visual Studio 2022 (Desktop C++ workload), x64.
2. CMake 3.31 (3.x; avoid CMake 4 with these dependencies).
3. Qt 5.15.2 MSVC 2019 64-bit.
4. Git, and network access for the FetchContent dependencies.

```bat
git submodule update --init --recursive
cmake -S . -B build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DNOGGIT_OPENGL_ERROR_CHECK=OFF ^
      -DFAST_BUILD_NOGGIT_PCH=ON
cmake --build build --target noggit --parallel
```

The executable lands in `build\bin` together with the Qt libraries that the
`windeployqt` post-build step deploys, plus `themes/`, `definitions/` and
`noggit-definitions/` copied from `dist/`.

Configure-time dependencies that are fetched or probed:

* `cmake/FindLua.cmake`, `cmake/FindStormLib.cmake` — prebuilt binaries from
  the `prophecy-rp/dependencies` GitLab repository (fallback: system Lua 5.4 /
  StormLib).
* `cmake-overrides/FindCascLib.cmake` — upstream CascLib pinned by commit.
* `cmake/FindJson.cmake`, `Findlodepng.cmake`, `FindSol2.cmake`,
  `FindFastNoise2.cmake` — GitHub sources.
* The qsqlmysql Qt SQL driver, downloaded from `thecodemonkey86/qt_mysql_driver`
  for the detected Qt version.

### Submodules

`cmake`, `src/external/blizzard-archive-library`,
`src/external/blizzard-database-library` and `dist/definitions` are required.
`dist/listfile` and `dist/themes` are hosted on `gitlab.com`; they only feed the
post-build copy steps, so a clone of the tree without them still compiles.

## Known issues fixed here

* **Configure aborted before Qt/OpenGL detection.**
  `cmake-overrides/FindCascLib.cmake` ran
  `src/external/blizzard-archive-library/cmake-overrides/PreferLocalHighResCasc.cmake`
  through `execute_process(... COMMAND_ERROR_IS_FATAL ANY)`, but that script is
  not present in the pinned submodule (nor anywhere in its history), so every
  fresh clone failed with `CMake Error: Not a file`. The step is now executed
  only when the file exists, and a status line is printed otherwise. When the
  script is restored upstream, the override picks it up automatically.

## Other toolchains

A Linux/macOS build is not supported by this fork yet. The code relies on
Windows-only pieces (`include/win/StackWalker.cpp`, `framelesshelper`), and the
README notes that FetchContent support for those hosts is "coming in the
future". Building on Linux additionally needs Qt5 with OpenGL, StormLib and Lua
development packages, none of which this project ships.
