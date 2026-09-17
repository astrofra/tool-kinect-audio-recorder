# Kinect Audio Recorder

A Windows interview recorder with Kinect v2 depth capture, WASAPI audio and hardware-free audio/depth simulation. The capture core, simulators, file writers, and CLI use **C++11** and CMake. The desktop interface uses **Dear ImGui + GLFW**. Playback, synchronization fitting and the XML editing workflow remain future work.

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

The GUI defaults to simulation. Choose a **Take path prefix** (default `recordings/take`), press **Record**, then **Stop**. Every Record click appends the local start date/time down to milliseconds, for example `recordings/take-2026-09-17_10-02-44-872`. You can restart immediately with the same prefix; previous takes are preserved. The actual path appears under **Current take** and in the session history. The meters display the recorded signal; no microphone, speakers, or headset are required for simulation. Recorded WAVs can be opened in an audio player/editor.

Capture now **continues with warnings by default** after audio discontinuities, invalid timestamps, transient audio/Kinect read failures or a full acquisition queue. Warnings appear in the GUI and `timing/events.jsonl`; the manifest includes their count and the capture policy. Stop drains and finalizes the available data as **Complete with warnings**. Missing samples/images are not invented, so warnings still matter for later synchronization. Enable **Stop on capture errors (strict)** or CLI `--strict` to restore fail-fast acquisition. Invalid startup settings, unavailable devices during initial preparation and disk write failures remain errors.

A large elapsed timecode stays visible at the top: `HH:MM:SS:FF`, at **30 fps non-drop**. It follows the stored audio sample count, keeps the final value after Stop, and resets for each new recording.

The **Depth source** selector defaults to a simulated moving gradient; noise, audio-only and **Kinect v2 (Microsoft SDK 2.0)** modes are also available. The GUI previews the latest stored depth image. Simulation uses 512 x 424 uint16 millimetres at 30 Hz, driven by the audio sample timeline. Physical Kinect capture retains independent native timestamps.

## Kinect v2 capture on Windows

Install the [Microsoft Kinect for Windows SDK 2.0](https://www.microsoft.com/en-us/download/details.aspx?id=44561), including its runtime and drivers. Follow Microsoft's installation instructions, then connect the powered Kinect v2 to USB 3.0. Configure/build for Windows x64 after installation; CMake detects the SDK through `KINECTSDK20_DIR` or its standard installation path. An explicit path can be supplied with `-DKINECTSDK20_DIR="C:/Program Files/Microsoft SDKs/Kinect/v2.0_1409"`. `-DRECORDER_ENABLE_KINECT=OFF` produces a build without sensor support.

```powershell
cmake --preset windows
cmake --build --preset windows --parallel
.\build\windows\Release\recording_tool.exe kinect-info
.\build\windows\Release\recording_tool.exe record --depth kinect --source wasapi --output recordings\kinect-test --duration 10
```

In the GUI, choose **Kinect v2 (Microsoft SDK 2.0)** for depth and **Windows microphone (WASAPI)** for audio. Choose the desired microphone endpoint explicitly. Real-time simulated audio can also accompany physical depth for testing. The Kinect runtime is loaded only when the sensor is selected, so simulation and microphone-only capture still work without it.

Native depth is stored without resampling or clamping, with sensor ID, depth intrinsics, reliable-distance limits, sensor timestamps and host receipt observations. Startup waits up to 15 seconds for a frame; a five-second stream stall produces a warning and retries while the other stream continues. Native depth segments rotate on sensor time and **do not pair by filename with audio segments**. Silent RGB review videos are available for physical capture; synchronized archival audio/depth export still needs a resolved timing solution. See [Kinect implementation and format](documentation/kinect-implementation.md).

## RGB review videos

**Create RGB Matroska preview after Stop** is enabled by default in the GUI for both Kinect and simulated depth. Once the take is finalized, the encoding queue creates **`video/preview-rgb.mkv`**, combining all depth segments. Open it in a player supporting FFV1/Matroska, such as VLC. **Review an existing take** also accepts an earlier take folder and queues the same export. Existing videos are never overwritten.

```powershell
.\release\recording_tool.exe export-preview --input release\recordings\take-2026-09-17_10-14-55-323
# Optionally choose --output VIDEO.mkv and/or --ffmpeg PATH.
.\release\recording_tool.exe record --depth kinect --duration 20 --timestamp-output --output recordings\review --encode-preview
```

The 512 x 424 video uses exactly the GUI palette: blue near, green midway, red far (500..6000 mm), with invalid zero values in black. It is a colorized depth image, not the Kinect color camera. FFV1 stores these RGB colors losslessly (`bgr0`); the original uint16 depth files remain unchanged. Encoding streams images to FFmpeg without an intermediate raw video file.

The preview is **silent**. Kinect review timing uses host receipt timestamps relative to the first depth image, rounded to a 30 fps playback grid; simulation uses its sample timeline. Gaps hold the last image, and the video ends one playback frame after the last stored image. Multiple arrivals in one playback slot retain the latest image. Sensor-clock resets do not collapse pauses. This is a visual review, not a calibrated synchronization export. Encoding runs after Stop on the existing background queue; a new take can begin while it finishes.

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

An explicit CLI `--output` remains an exact directory. Add `--timestamp-output` to use it as a reusable prefix, e.g. `record --output recordings/interview --timestamp-output --depth kinect --source wasapi --duration 10`. Warning-only takes return exit code 0; errors still return nonzero.

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

Depth-enabled takes additionally contain `depth/*.kd16` and `timing/depth-frames.jsonl`. Simulated depth rotates with audio, and its last frame can extend less than one video frame beyond the audio end. Kinect depth rotates independently on the sensor timeline; its journal is authoritative. Audio samples are unchanged.

See [the implemented audio milestone](documentation/audio-implementation.md) for format details, tests, and current limits, and [the design documentation](documentation/README.md) for the complete planned tool.
