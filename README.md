# Kinect Audio Recorder

A Windows interview recorder with audio capture and hardware-free audio/depth simulation. The capture core, simulators, file writers, and CLI use **C++11** and CMake. The desktop interface uses **Dear ImGui + GLFW**. Physical Kinect capture and the XML editing workflow are specified but not implemented yet.

## Build on Windows

Requirements: Visual Studio 2022 with the C++ desktop workload/Windows SDK, CMake 3.21+ for presets, and Git for Windows. The full release build also uses Git Bash and Windows `curl.exe`/`tar.exe` (with xz/zstd support). The GUI build fetches pinned Dear ImGui and GLFW revisions on its first configuration.

For a complete clean build and a ready-to-commit Windows x64 package:

```powershell
.\build_release.bat
.\release\audio_recorder.exe
```

The batch file works from any working directory. It invokes the checked-in PowerShell helper, deletes only `build/release/`, fetches the pinned GUI sources again, and compiles both recorder executables and a minimal FFmpeg from scratch with static runtimes. It then runs CTest, CLI/hidden-window GUI simulation and bundled-video export checks before copying the package into `release/`. Internet access and an OpenGL 3.3-capable graphics driver are required for this complete build. FFmpeg and GNU Make source/tool downloads are pinned by SHA-256 and cached under `build/downloads/`; compiled objects are rebuilt. Python 3 is optional for the additional file, codec and package-relocation tests.

`release/` contains the two recorder executables, a usage guide, license notices, SHA-256 checksums, and `extern/ffmpeg/` with the encoder, licenses, exact source archive and build recipe. It is tracked normally by Git; the script does not stage files, commit, or push. Existing release files are only updated after compilation and all enabled tests succeed. Other files in `release/`, including recordings, are preserved; `release/recordings/` is ignored. Close running release executables before rebuilding so Windows permits replacement. Edit `packaging/README.md` to change the generated usage guide. Intermediate builds, downloaded dependencies, and test recordings stay under ignored `build/`.

For incremental development builds:

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
ctest --preset windows
.\build\windows\Release\audio_recorder.exe
```

The GUI defaults to simulation. Choose a new take folder, press **Record**, then **Stop**. The meters display the recorded signal; no microphone, speakers, or headset are required for simulation. Recorded WAVs can be opened in an audio player/editor. This first UI lists takes created during the current application session.

A large elapsed timecode stays visible at the top: `HH:MM:SS:FF`, at **30 fps non-drop**. It follows the stored audio sample count, keeps the final value after Stop, and resets for each new recording.

The **Depth source** selector defaults to a simulated moving gradient; noise and audio-only modes are also available. The GUI previews the latest stored depth image. Capture uses 512 x 424 uint16 millimetres at 30 Hz, driven by the audio sample timeline.

**Encode finished segments to Matroska in background** is enabled by default in the GUI. Each closed depth/audio pair is queued as `video/000000.mkv`, etc. One worker runs one FFmpeg process at a time, at reduced CPU priority with two codec threads. The queue panel shows pending, completed and failed jobs across takes. Stop requests capture finalization without waiting for encoding; you can start another take as soon as capture finishes. Closing the window drains the queue with the interface still responsive. The raw recordings are always retained.

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

## Simulated depth and lossless video

```powershell
.\release\recording_tool.exe record --output recordings\depth-test --duration 10 --depth gradient

# Capture and automatically encode each finished segment in the background.
.\release\recording_tool.exe record --output recordings\auto-test --duration 65 --depth gradient --encode-depth

# Uses the bundled FFmpeg: export one depth segment with its matching audio.
.\release\recording_tool.exe export-depth --input recordings\depth-test\depth\000000.kd16 --audio recordings\depth-test\audio\000000.wav --output recordings\depth-test\depth-video.mkv
```

Use `--depth noise` for animated noise, or `--depth off` for audio only (the CLI default). Native recording uses raw 16-bit segments and timing journals; **FFV1/gray16le in Matroska** preserves depth values exactly. The CLI opts into background encoding with `--encode-depth` and waits for the queue before exiting; `--ffmpeg PATH` selects an encoder for that queue or for manual export. Export prefers `extern/ffmpeg/ffmpeg.exe` relative to the recorder executable, then PATH. The Windows release includes a minimal FFmpeg 9.0.1 for FFV1/rawvideo/PCM, with its LGPL notices and complete corresponding FFmpeg source. See [its build recipe and provenance](extern/ffmpeg/README.md). Export validates one finalized segment at a time and never overwrites an existing output. See [the depth implementation](documentation/depth-implementation.md) for timing, format, measurements and limitations.

## Portable build without GUI dependencies

```sh
cmake -S . -B build/cli -DRECORDER_BUILD_GUI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/cli --config Release --parallel
ctest --test-dir build/cli -C Release --output-on-failure
```

The core build requires CMake 3.16+ and a C++11 compiler, with no downloaded libraries. On non-Windows systems this milestone provides simulation, the CLI, and file writing; hardware audio capture is currently Windows-only. Python 3 is optional for the independent file-validation tests and is never an application runtime dependency. Portable code paths still need qualification on Linux/macOS; local validation was performed on Windows.

## Recording files

Each take contains `manifest.json`, `checkpoint.json`, segmented `audio/*.wav` files, and `timing/*.jsonl` journals. Audio is uncompressed IEEE float32 PCM, rotated every 60 seconds by default. Packet metadata preserves sample positions, native timing, receipt-clock observations, and error flags for future Kinect synchronization.

Depth-enabled takes additionally contain `depth/*.kd16` and `timing/depth-frames.jsonl`, rotating at the same interval. The last depth frame can extend less than one video frame beyond the audio end; audio samples are unchanged.

See [the implemented audio milestone](documentation/audio-implementation.md) for format details, tests, and current limits, and [the design documentation](documentation/README.md) for the complete planned tool.
