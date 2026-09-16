# Kinect Audio Recorder

A Windows interview recorder, starting with audio capture and hardware-free audio simulation. The capture core, simulator, WAV writer, and CLI use **C++11** and CMake. The desktop interface uses **Dear ImGui + GLFW**. Kinect capture and the XML editing workflow are specified but not implemented yet.

## Build on Windows

Requirements: Visual Studio 2022 with the C++ desktop workload/Windows SDK, CMake 3.21+ for presets, and Git. The GUI build fetches pinned Dear ImGui and GLFW revisions on its first configuration.

For a complete clean build and a ready-to-commit Windows x64 package:

```powershell
.\build_release.bat
.\release\audio_recorder.exe
```

The batch file works from any working directory. It invokes the checked-in PowerShell helper, deletes only `build/release/`, fetches the pinned GUI sources again, compiles both executables in Release mode with a static C/C++ runtime, runs CTest and CLI/hidden-window GUI simulation checks, and copies the package into `release/`. Internet access and an OpenGL 3.3-capable graphics driver are required for this complete build. Python 3 is optional; CMake enables the additional WAV/JSON validation suite when it finds an interpreter.

`release/` contains the two executables, a usage guide, license notices, and SHA-256 checksums. It is tracked normally by Git; the script does not stage files, commit, or push. Existing release files are only updated after compilation and all enabled tests succeed. Other files in `release/`, including recordings, are preserved; `release/recordings/` is ignored. Close running release executables before rebuilding so Windows permits replacement. Edit `packaging/README.md` to change the generated usage guide. Intermediate builds, downloaded dependencies, and test recordings stay under ignored `build/`.

For incremental development builds:

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
ctest --preset windows
.\build\windows\Release\audio_recorder.exe
```

The GUI defaults to simulation. Choose a new take folder, press **Record**, then **Stop**. The meters display the recorded signal; no microphone, speakers, or headset are required for simulation. Recorded WAVs can be opened in an audio player/editor. This first UI lists takes created during the current application session.

## Command line

```powershell
# Real-time simulated audio, with a tone and an audible marker every second.
.\build\windows\Release\recording_tool.exe record --output recordings\test --duration 10

# Deterministic stereo sine test, generated as fast as the writer can accept it.
.\build\windows\Release\recording_tool.exe record --output recordings\stereo --duration 65 --channels 2 --signal sine --fast

# Discover Windows inputs, then capture the default one until Ctrl+C.
.\build\windows\Release\recording_tool.exe devices
.\build\windows\Release\recording_tool.exe record --source wasapi --output recordings\microphone --duration 0
```

Use `--device "<endpoint ID>"` to select a specific microphone. WASAPI uses the endpoint's native shared-mode sample rate and mono/stereo layout, displayed in the GUI and saved in the manifest. It does not silently switch devices. `--help` lists simulation and file-rotation options. Existing take directories are never overwritten.

## Portable build without GUI dependencies

```sh
cmake -S . -B build/cli -DRECORDER_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/cli --config Release --parallel
ctest --test-dir build/cli -C Release --output-on-failure
```

The core build requires CMake 3.16+ and a C++11 compiler, with no downloaded libraries. On non-Windows systems this milestone provides simulation, the CLI, and file writing; hardware audio capture is currently Windows-only. Python 3 is optional for the independent file-validation tests and is never an application runtime dependency. Portable code paths still need qualification on Linux/macOS; local validation was performed on Windows.

## Recording files

Each take contains `manifest.json`, `checkpoint.json`, segmented `audio/*.wav` files, and `timing/*.jsonl` journals. Audio is uncompressed IEEE float32 PCM, rotated every 60 seconds by default. Packet metadata preserves sample positions, native timing, receipt-clock observations, and error flags for future Kinect synchronization.

See [the implemented audio milestone](documentation/audio-implementation.md) for format details, tests, and current limits, and [the design documentation](documentation/README.md) for the complete planned tool.
