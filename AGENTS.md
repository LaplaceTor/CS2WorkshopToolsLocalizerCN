# AGENTS.md

## Build & Checks

- **Prerequisites**: Windows x64, MSVC (VS 2022), CMake ≥ 3.20, Ninja, Qt 6 (`Core`, `Gui`, `Widgets`, `Network`, `Concurrent`), and git submodule `third_party/minhook` (`git submodule update --init --recursive`).
- **Configure & Build** (see [CMakeLists.txt](CMakeLists.txt) and [.github/workflows/pr-ci.yml](.github/workflows/pr-ci.yml)):
  ```powershell
  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:Qt6_DIR"
  cmake --build build --config Release
  ```
- **Two build targets**:
  - `CS2WorkshopToolsLocalizerCN.exe`: Qt6 Widgets launcher (`WIN32`).
  - `qtcore_qm.dll`: Injected hook DLL compiled with MinHook and static MSVC runtime (`MultiThreaded`, no Qt link dependency).
- **No unit test target**: `CMakeLists.txt` intentionally omits unit tests; hook/injection behavior is verified against live Hammer, and dictionary edits are gated by:
  ```powershell
  python scripts/check_duplicates.py
  ```
- **Source list is explicit**: Adding or removing `.cpp` files requires updating `CMakeLists.txt` directly.

## Architecture & Layering

Strict dependency direction: **`UI (src/ui/)` → `Service (src/service/)` → `Core (src/core/)`**.

- `Core` must never depend on `UI`; `Service` orchestrates workflows without depending on `QWidget` controls.
- `src/core/path_constants.h` is the single source of truth for CS2 directory/file literals and path builders (shared by both the launcher and `qtcore_qm.dll`).
- `src/core/launcher_config.h` / `src/core/launcher_config.cpp` own `config.ini` persistence (`Launcher/Cs2Root`, `Launcher/SelectedAddon`, `Launcher/LaunchArgs`, `Launcher/UseMachineTrans`). When saving in `MainWindow::saveSettings()`, populate all fields so partial updates do not clear `Cs2Root`.
- `MainWindow` member initializer list order in `src/ui/mainwindow.cpp` must match declaration order in `src/ui/mainwindow.h` (MSVC `-Wreorder` / `C5038`).
- All source files and comments are UTF-8 (`add_compile_options(/utf-8)` on MSVC).

## CS2 Path Detection & Persistence

- On startup (`src/main.cpp`), load `Launcher/Cs2Root` from `config.ini` first and validate via `Cs2Detector::IsValidCs2Root()` before falling back to `Cs2Detector::DetectCs2()`.
- `Cs2Detector::IsValidCs2Root()` requires both `game/bin/win64/cs2.exe` and `game/bin/win64/Qt5Core.dll` to exist as regular files (empty or leftover `win64` directories must fail validation).
- `Cs2Detector::CheckRegistrySteam()` parses `libraryfolders.vdf` block structure and prioritizes libraries whose `"apps"` block contains `"730"` before falling back to other libraries.

## Translations (`translations/*.jsonc`)

- `qt_translations.jsonc` uses per-DLL top-level sections (`"hammer"`, `"modeldoc_editor"`, etc.) with `"common"` as fallback.
- `fgd_translations.jsonc` and `fgd_override.jsonc` cover FGD entity/property/I/O descriptions.
- `qt_fallback.jsonc` and `fgd_fallback.jsonc` are script-generated; edit only the non-fallback dictionaries.
- Runtime edits from the built-in editor go through `DictionaryTextUpdater` (`src/core/dictionary_text_updater.*`) to preserve JSONC `//` comments and formatting in-place.
