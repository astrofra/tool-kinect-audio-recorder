# Kinect Audio Recorder: Windows x64 release

This folder is intended to be committed alongside the source. At the repository
root, `build_release.bat` rebuilds and tests the recorder while reusing the bundled
FFmpeg. `rebuild_ffmpeg.bat` separately rebuilds only FFmpeg and updates its package
and checksums. If FFmpeg is missing, run that script first. Do not edit generated
files; edit `packaging/README.md` or the source, then rebuild.

## Run

Double-click `audio_recorder.exe` for the Dear ImGui/GLFW interface. Simulation
is selected on first launch: choose the audio/video inputs and press **Enregistrer**, then
**Arrêter**. **Paramètres** contains the output prefix. Each take appends local date/time, e.g.
`take-2026-09-17_10-02-44-872`; you can reuse the prefix for successive takes.
No audio hardware is needed for simulation. A graphics driver supporting
OpenGL 3.3 is required for the interface.

The large counter at the top displays elapsed recorded audio as `HH:MM:SS:FF`
at 30 fps non-drop. It retains the final duration after Stop and resets for each
new recording. It remains visible while opening settings or take history.

**Entrée vidéo** defaults to a moving gradient, with animated noise or audio-only
as alternatives. Record captures the simulated 512 x 424, 30 Hz, 16-bit depth map
alongside audio and displays a false-color preview. No Kinect driver is needed.

**Paramètres > Encoder les segments simulés en Matroska** is enabled by default.
After each segment closes (every 60 seconds, or at Stop), its depth and audio are
queued into `video/000000.mkv`, etc. Only one encoding runs at a time, with reduced
CPU priority and two codec threads. The queue appears below the depth and audio panels.
You can start a new take while earlier segments encode. Closing the window waits
for the queue with the interface still responsive; **Garder ouvert** cancels
closing. Original depth, WAV and timing files are always kept. The progress bar
counts completed jobs in the queue; it does not estimate progress within a job.

**Gain** adjusts recorded audio from **-24 to +36 dB**, including during capture.
Drag the knob, use the wheel, or edit its value; double-click resets it to 0 dB.
All channels receive the same software gain before WAV storage and metering.
Red indicators warn at 0 dBFS. Float32 storage keeps headroom without hard clipping;
reduce gain if the indicators light up. Windows microphone settings are unchanged.

**Pause / Reprendre** keeps one take and leaves the devices open, discarding audio
and images received during the pause. The timecode stops after queued writes drain.
Pause boundaries are journaled; native Kinect timing remains intact, so RGB review
holds the last image during a pause. **Lecture** opens the last encoded RGB preview
in the associated external player. The RGB preview is still silent.

## Automatic settings

The GUI creates **`recorder.ini` beside the executable**, saves edits automatically,
and restores them on the next launch. It remembers audio device ID/name, video source,
gain, session, output prefix, duration, segment length, strict mode, simulation and
encoding options, last take, and window size/maximized state. A missing remembered
microphone stays selected and reports an error instead of switching to another input.

Use **Paramètres** for secondary options. `recorder.example.ini` documents the defaults;
edit the personal INI while the application is closed. It is UTF-8, and relative paths
are relative to the INI folder. `audio_recorder.exe --config PATH.ini` selects a different
configuration. Rebuilds preserve the personal INI; only the example is packaged.
CLI captures use their own command-line options, including `--gain-db 30`.

From PowerShell in this folder:

```powershell
# Create a ten-second simulated take without a microphone or graphics context.
.\recording_tool.exe record --output recordings\test --duration 10

# List available Windows audio inputs.
.\recording_tool.exe devices

# Record the default microphone until Ctrl+C.
.\recording_tool.exe record --source wasapi --output recordings\microphone --duration 0
```

The GUI chooses a new timestamped folder for each take. In the CLI, add
`--timestamp-output` to reuse an `--output` prefix; an exact existing output
directory is still protected. Each take contains segmented float32
WAV audio, a JSON manifest, and timing journals, plus raw `.kd16` segments when
depth is enabled. Simulation generates samples but does not play them through
speakers. In-app playback and DaVinci Resolve XML conform are not implemented yet.

For a real microphone, choose its name under **Entrée audio** (WASAPI). Inputs are listed
automatically; **Actualiser les entrées** refreshes the list and preserves the selection.
The recorder keeps the Windows shared-mode sample rate (8 to 192 kHz) and all
**1 to 32 channels**, with a meter per channel and the actual input name shown
after starting. PCM 8/16/24/32-bit and float32 inputs are stored as float32 WAV
without resampling or downmixing. Multichannel files preserve channel order and
the source channel mask using WAVEFORMATEXTENSIBLE. Simulation remains mono/stereo.
The default rotation is 60 seconds; longer configured segments are shortened if
needed to keep each WAV below the RIFF size limit, with a warning in the journal.

## Watch an RGB preview

**Paramètres > Créer l’aperçu RGB après Arrêter** is enabled by default. After finalization,
the background queue creates `video/preview-rgb.mkv` in the take folder, for Kinect
as well as simulation. Open this file in VLC or another FFV1/Matroska player.
**Prises > Exporter l’aperçu RGB** accepts an earlier take folder and exports the same
video. The CLI equivalent is:

```powershell
.\recording_tool.exe export-preview --input recordings\YOUR-TAKE
# Optional: --output another-preview.mkv, --ffmpeg PATH
# For automatic export during CLI recording, add --encode-preview.
```

The preview combines all depth segments with exactly the screen's blue/green/red
palette (500..6000 mm; zero is black), at 512 x 424, using lossless RGB FFV1.
It shows colorized depth, not the Kinect color camera. It is **without sound**;
Kinect/audio synchronization remains separate work. Kinect playback follows host
receipt timestamps, rounded to 30 fps; pauses hold the previous image instead of
speeding up time. Frames arriving in the same playback slot use the latest image.
The video spans the first through last stored depth image plus one playback frame.
Simulation follows its stored sample timeline. All raw files and journals stay intact.
No large intermediate raw video is written, and an existing preview is never replaced.

For physical capture, install Microsoft Kinect for Windows SDK 2.0 and its drivers,
connect a powered Kinect v2 to USB 3.0, and use a package built with SDK support.
Choose **Kinect v2 — Capteur** under **Entrée vidéo** and select the
desired WASAPI microphone. The SDK/runtime is not included in this package.

```powershell
.\recording_tool.exe kinect-info
.\recording_tool.exe record --depth kinect --source wasapi --output recordings\kinect-test --duration 10
```

Physical depth retains native sensor timestamps, calibration and uint16 millimetres.
Its segments are independent of audio segments. Silent RGB review is available;
synchronized archival audio/depth export remains simulation-only until a calibrated
timing solution is implemented. Simulation and audio-only recording work without
the Kinect runtime.

Capture continues through acquisition warnings by default, including audio
discontinuities and temporary Kinect/audio read failures. The GUI reports warnings in **Journal**; `timing/events.jsonl` stores the details. Missing data is not
filled with fake images or silence. Enable **Paramètres > Arrêter sur erreur de capture (strict)** or
CLI `--strict` to stop on acquisition errors. Startup configuration/device failures
and disk write failures remain blocking. A finalized take with warnings is shown
as **TERMINÉ AVEC ALERTES** and returns CLI exit code 0.

To record depth from the CLI and optionally export one segment as lossless video:

```powershell
.\recording_tool.exe record --output recordings\depth-test --duration 10 --depth gradient
.\recording_tool.exe record --output recordings\auto-test --duration 65 --depth gradient --encode-depth
.\recording_tool.exe export-depth --input recordings\depth-test\depth\000000.kd16 --audio recordings\depth-test\audio\000000.wav --output recordings\depth-test\depth.mkv
```

Export uses FFV1 `gray16le` and optional PCM audio in Matroska. The included
`extern/ffmpeg/ffmpeg.exe` is found relative to the recorder executable, including
when this folder is moved or launched from another working directory. No PATH
configuration is needed. `--ffmpeg PATH` selects an explicit alternative; PATH is
used as a fallback only when the bundled binary is absent. Choose the WAV with the same segment
number and take as the depth file. The command exports one finalized segment at a
time, rejects existing output files and reports encoder errors. Original timing
journals remain the authority for the complete take.

The CLI's `--encode-depth` and `--encode-preview` modes wait for all queued jobs before exiting and
returns an error if any encoding failed, even when capture itself succeeded.
Background jobs write `.mkv.part` first, then publish `.mkv` only after success.
Each attempted job has a `.mkv.json` status and `.mkv.log` encoder diagnostics
when those files can be written. Failed jobs do not stop capture or later jobs.
The queue is in memory; it is not automatically resumed after a crash or forced
exit. Re-export missing videos from the retained originals using `export-depth`
or `export-preview` (use a new output name if a failed attempt left a `.part`/`.log`).
CPU priority does not eliminate disk contention: test sustained recordings on
the intended machine. Uncheck automatic encoding before the next take if needed.

## Troubleshooting

### Unsupported mono/stereo audio format in an older build

Use the updated recorder: audio inputs with **1 to 32 channels** are supported.
With enhancements disabled, the Kinect microphone on the development PC exposes
**4 channels at 16 kHz, float32**. Keep **Audio enhancements > Off** and select
the Kinect microphone under **Entrée audio**. A new recording
should show four meters and save all four channels in one WAV. If a player cannot
play this layout, open it in an audio editor with multichannel support.

### Kinect v2 repeatedly disconnects on Windows 11

**Disable audio enhancements on the Kinect microphone.** On the development PC,
Windows Voice Clarity was associated with that input. The user confirmed that
turning audio enhancements off resolved the repeated disconnections on
17 September 2026.

The Kinect light cycled off and on while the adapter's power light stayed on.
Windows logged USB removals as event **1010** in
`Microsoft-Windows-Kernel-PnP/Device Management`. Disconnects were about
**16.26 seconds apart** on average across 12 measured intervals (an observed
cycle, not a fixed timeout). Depth had long gaps, and microphone audio stopped
after about six seconds with `0x88890004` (`AUDCLNT_E_DEVICE_INVALIDATED`).
A second Kinect on the same adapter, cable and USB port showed the same problem.

1. Stop the current recording.
2. Open **Windows Settings > System > Sound > Input**.
3. Select **Microphone Array (Xbox NUI Sensor)**, or
   **Microphone Array (2- Xbox NUI Sensor)** for a replacement sensor.
4. Set **Audio enhancements** to **Off** (French: **Améliorations audio > Désactivé**).
5. Start a new recording of at least **45 seconds** and check that the sensor stays
   on and both audio and depth continue throughout the take.

This setting belongs to the selected audio endpoint: check it again after changing
Kinect sensors. The recorder does not change this Windows setting automatically.
Continuing with warnings keeps a take running but does not prevent USB disconnects
or restore missing data from an earlier take.

Related guidance: [Kinect disconnect loop and Windows audio enhancements](https://ar-sandbox.eu/docs/kinectsandbox-software/troubleshooting/).

## Package contents

- `audio_recorder.exe`: desktop interface.
- `recording_tool.exe`: command-line recorder and device enumeration.
- `extern/ffmpeg/`: minimal FFmpeg 9.0.1, its licenses, exact source archive and rebuild scripts.
- `LICENSE`: project license (GPL v3).
- `licenses/`: GLFW and Dear ImGui license notices.
- `SHA256SUMS.txt`: SHA-256 hashes of the generated package files except itself.

The executables include the C/C++ runtime and GUI libraries statically. No Python,
CMake, Git, Kinect SDK, or separate Visual C++ runtime installation is needed to
run them. They still use Windows system libraries and the installed graphics/audio
drivers. Python is used only by optional tests during the build.

Bundled FFmpeg supports FFV1/rawvideo/PCM exports and decoding. H.264/H.265,
network access, GPU encoding, ffplay and ffprobe are not included in this build.
See [FFmpeg's package notice](extern/ffmpeg/README.md) for the LGPL 2.1-or-later
license, source provenance, build configuration and reproduction instructions.
Keep the complete `extern/ffmpeg/` directory with the release when distributing it.

Rebuilding refreshes generated files here after successful compilation and tests.
Other files, including `recordings/`, are preserved. Recordings in this folder are
ignored by Git. Build products and test recordings stay in `build/release/` at the
repository root and are not part of this package.

See `../documentation/audio-implementation.md` in the source repository for the
file format and limitations. Build instructions are in `../README.md`.
