# Audio recorder implementation: first milestone

This document describes implemented behavior. The [product specification](specification.md) remains the broader target. The implementation language is **C++11**, superseding the original C++20 recommendation.

## Available components

- `recording_tool`: record audio or enumerate Windows capture endpoints.
- `audio_recorder`: Dear ImGui/GLFW interface following the supplied mockup, with English transport controls, RGB depth view, vertical meters, live gain and persistent INI preferences.
- `recorder_core`: portable source interface, deterministic simulator, bounded packet queue, background file writer, float32 WAV serialization, and recording state/control.
- `WasapiSource`: Windows shared-mode, event-driven capture using native Windows SDK APIs. COM objects are created, used, and released on the capture thread.

There are no runtime dependencies for the CLI beyond the platform's C/C++ runtime. The optional GUI statically compiles pinned Dear ImGui v1.92.9b and GLFW 3.5.1 sources. The GUI needs an OpenGL 3.3 driver; simulation through the CLI does not need graphics or audio hardware. The application does not install or replace drivers.

`build_release.bat` builds a clean Windows x64 recorder package with the MSVC runtime statically linked into both executables. It reuses the existing `release/extern/ffmpeg/` package and runs the available CTest suites and simulation checks against the installed CLI and GUI before updating `release/`. `rebuild_ffmpeg.bat` rebuilds FFmpeg separately. The package includes license notices and checksums, is intended for Git, and needs no separately installed C/C++ runtime. Incremental builds remain available through the `windows` preset. See the repository [build instructions](../README.md).

The recorder accepts another `AudioSource` implementation through the same interface; tests use this to inject timestamp errors, sequence gaps, and source failures. This does not couple future Kinect capture to the audio device API.

## Elapsed timecode display

The GUI has a large, fixed header showing `HH:MM:SS:FF` at **30/1 fps, non-drop**, matching the default planned proxy rate. The display is derived from stored audio sample frames at the actual sample rate, rounded down to the current timecode frame using integer arithmetic. It does not advance when no audio is written, retains the final duration after Stop/finalization, and resets to zero when a new recording starts. It stays visible while settings or take history are open. Hours continue past 24 for long takes.

This is an elapsed-duration display in SMPTE-style notation, not embedded source timecode or an external clock synchronization feature. It does not change recording timestamps or the archive format; interrupted takes still require inspection of their timing journals.

## GUI controls and configuration

`Gain` applies -24..+36 dB to every audio channel before queueing to disk. Live changes ramp linearly over the beginning of the next packet, up to 10 ms. The float32 writer does not hard-clip values above full scale: the meter lights red and `audio_overload` is a warning, including in strict mode. `--gain-db` selects the initial CLI gain. The manifest records the initial gain, and every audio journal fragment records the target dB, start/end linear multipliers and ramp length. Packet offsets identify the correct portion of a ramp across file rotation.

Pause keeps acquisition running and discards packets/images until resume. Both queues drain normally, the stored-sample timecode freezes, and Stop works during pause. `pause`/`resume` events retain host ticks and captured-audio frame positions. The first resumed audio/depth item carries `pause_boundary`; intentional gaps do not count as device discontinuities. Native clock resets still do. Kinect timestamps remain native, and its RGB preview retains the elapsed pause as a held image. Simulated depth continues on the stored-audio clock.

The GUI loads `recorder.ini` beside its executable, or an explicit `--config` file. UTF-8 quoted strings support backslashes, quotes and line escapes; unknown keys survive a rewrite, while comments are regenerated. Invalid individual values are reported and retain defaults. Writes use the existing atomic metadata writer, are debounced by 600 ms, and flush on close. Relative paths resolve against the INI directory. Audio selection stores an endpoint ID, never an enumeration index; a missing endpoint is not silently replaced. Rebuilds install `recorder.example.ini` only, preserving the personal INI.

The view preserves the 512x424 image aspect ratio and palette. Fonts come from Windows Segoe UI with an embedded ImGui fallback. Meter channels scroll horizontally above four channels. Encoding progress counts finished jobs rather than inventing an active-job estimate; free disk space is refreshed every five seconds. Play opens the last completed RGB preview through the Windows file association, without an embedded player.

## Simulation signal

Default: 48 kHz, mono, amplitude 0.25, 440 Hz base tone, plus an 80 ms 1 kHz marker each sample-clock second. Markers have 5 ms ramps. `--signal sine` produces only the base tone; the second stereo channel uses 1.5 times the base frequency. `--amplitude 0` generates silence. Simulation writes samples to the recorder and meters; it does not play sound through the speakers.

Real-time mode schedules packets using the monotonic host clock. `--fast` produces the same samples without real-time waiting and explicitly marks timing as `synthetic-unpaced`. Its generated sample timestamps can run ahead of the real receipt clock; those observations must never be used as measurements of device latency. Fast mode waits for writer capacity. Real-time queue overflow logs a warning and skips the incoming packet by default; `--strict` stops capture instead.

Duration is converted once to a whole number of sample frames at the actual sample rate. The final packet is trimmed precisely. A sample frame contains one sample per channel. A zero duration records until Stop/Ctrl+C; unbounded fast simulation is rejected.

## Native Windows capture

`devices` returns active input names and opaque endpoint IDs. Capture opens the chosen endpoint or the current default once. The actual shared-mode mix rate (8000..192000 Hz) and all 1..32 channels are retained. Float32 and integer PCM input with 8, 16, 24, or 32-bit containers are accepted, converting samples to float32 storage. No resampling, channel reordering or downmixing occurs. Per-channel peak/RMS meters follow the actual channel count, and the GUI displays the resolved input name. Selecting a subset of channels is deferred; simulation still offers one or two channels.

WASAPI provides a device sample position and a correlated QPC value in 100 ns units for each packet. Store the original values and flags, including invalid timestamps and silent packets. Integer conversion does not turn a device into a higher-resolution ADC. The native API behavior follows Microsoft's [capture buffer contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) and [event-driven initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize).

The initial WASAPI discontinuity flag is retained without treating startup as mid-take loss. Subsequent discontinuities or valid device-position gaps now log warnings and retain the received PCM and native positions without stopping. Timestamp-error packets retain their samples and are excluded from continuity comparisons. No packets for five seconds raises a read error; the default policy journals it, pauses briefly and retries the same source while Kinect acquisition continues. It never switches microphones. Invalid-size/non-finite packets are skipped with a warning. `--strict` restores interruption on source failures, discontinuities and queue overload.

Warning events contain a code, message, occurrence count and first/last host ticks. Pending events are coalesced by code, keeping the diagnostic queue bounded. The writer owns log I/O. The UI reports warnings in **Log** and displays **COMPLETE WITH WARNINGS** after finalization; the manifest retains `state: complete` with `warnings`, `last_warning` and `capture_policy`. The CLI returns zero for warning-only takes. If microphone reads keep failing, a finite take stops after its requested wall duration is reached and the current read/retry finishes; an indefinite take remains stoppable. Initial device-open/configuration and file-write errors remain fatal. Retrying an invalidated device handle is not a guarantee of automatic reconnection.

## File contract

Optional simulated depth capture adds a separate raster stream, schema and journal, documented in [the depth implementation](depth-implementation.md). The audio-only contract below remains supported.

The schema is `kinect-audio-prototype/1`, intentionally distinguishable from the future complete Kinect archive. A typical take contains:

```text
take/
  manifest.json
  checkpoint.json
  audio/
    000000.wav
    000001.wav
  timing/
    audio-packets.jsonl
    events.jsonl
```

WAV files contain little-endian IEEE float32 samples, a format chunk, a sample-count `fact` chunk, and a data chunk. Standard mono/stereo files retain the 18-byte IEEE-float format chunk (58-byte file header). More than two channels, or a nonstandard explicit mono/stereo channel mask, use a 40-byte WAVEFORMATEXTENSIBLE chunk (80-byte header), with 32 valid bits, the IEEE-float subtype GUID and the source channel mask. A zero mask retains unspecified positions, as used by the Kinect microphone array; export disables FFmpeg's channel-layout guessing.

Files rotate at a whole-second sample boundary; rotation may split a source packet. Default rotation is 60 seconds. At high channel counts/rates, a longer requested segment is capped to fit RIFF's 32-bit length field, with an `audio_segment_limit` warning and effective `segment_seconds` in the manifest. The audio queue targets five seconds but reduces its slot count to retain the 64 MiB payload budget.

The manifest identifies source kind, selected device, actual sample format/channel mask, clock origin/frequency, simulation recipe, file list, total stored frames, and state. UTC is descriptive metadata. Successful completion follows queue draining, WAV header finalization, file flushing, and manifest replacement. An output directory must be new; the recorder never erases an existing take.

Each JSONL audio entry records the file and sample offset, stored frame count, original packet device position, packet offset when split across files, source timestamp in 100 ns units, raw receipt-clock ticks, and flags. Raw counters are decimal strings to preserve all 64 bits when parsed by JavaScript. When a packet is split, its original timestamp is repeated: the first sample in a record is at `packet_offset_frames` after that timestamp. The native device frame for that sample is `device_frame + packet_offset_frames` when valid.

On Windows, raw receipt ticks use QPC and the saved frequency. Other platforms use a monotonic nanosecond clock with frequency 1 GHz. This milestone preserves observations; it does not yet estimate cross-device clock drift or Kinect latency.

Checkpointing occurs after approximately one second of stored audio, and at orderly finalization. WAV headers and packet/event logs are flushed before an atomic checkpoint describing their committed prefix. A writer failure does not advance the known-good checkpoint. The manifest is also replaced through a temporary sibling. This supports future recovery tooling but does not yet implement a `recover` command or guarantee power-loss durability.

If data is missing, the stored PCM contains the received packets without filling gaps; the journal carries the original positions. Both Interrupted takes and completed takes with warnings must be inspected before treating them as a gap-free timeline. Silence insertion and repaired playback/export remain future work.

## Validation and limits

The C++ tests exercise sample-clock arithmetic, signal bounds, stop/drain, reuse, overwrite refusal, bad timestamps, source exceptions, and device-position gaps. An independent Python standard-library test parses WAV chunks and JSON, checks sample values on both channels, exact duration, packet continuity, partial packets, rotation across packet boundaries, Unicode paths, silence, and invalid CLI arguments.

The multichannel fixture covers 4-channel/16 kHz, 8-channel/44.1 kHz and 32-channel/192 kHz sources, a nonstandard two-channel mask, and reuse with mono. It checks every meter, maximum packet capacity, the RIFF rotation cap and rejection above 32 channels. Independent Python checks decode every WAV sample, channel mask and segment boundary, reject malformed extensible headers and verify a byte-exact four-channel Matroska round trip with FFmpeg.

On 17 September 2026, a real 45-second capture with Kinect microphone enhancements disabled produced 720,000 sample frames at 16 kHz across four channels, plus 1,350 native depth frames, with zero warnings, invalid audio timestamps or depth gaps. FFmpeg decoded the four-channel WAV byte for byte. The WASAPI GUI smoke test also completed with four meters and both background exports successful. This checks one device/session, not long-duration endurance across all supported formats.

`audio_recorder --smoke-test NEW_DIRECTORY` creates a hidden window, records 250 ms of simulation through the GUI's recorder path, finalizes it, and saves an adjacent PPM framebuffer capture. This tests renderer/recorder integration without a visible desktop window or a microphone. Normal operation omits this argument.

`--controls-smoke-test NEW_DIRECTORY` drives the actual mouse widgets for Record, Gain, Pause, Resume and Stop, with recording/paused framebuffer captures. The GUI integration test restarts with the saved INI, checks the applied gain and source options, verifies UTF-8 paths, and confirms that a missing remembered microphone is not replaced. Core tests check the gained samples on all four channels, pause exclusion of both media streams, intentional native depth gaps, overload without hard clipping and Stop while paused.

Local validation used Visual Studio 2022/MSVC x64 on Windows: both CTest suites passed, the hidden-window GUI smoke test completed, and a 65-second stereo simulation produced exactly 3,120,000 sample frames with 60-second/5-second WAV rotation. FFprobe recognized the format as `pcm_f32le`, and FFmpeg decoded the generated audio. This accelerated run verifies file generation/rotation, not 65 seconds of real-time hardware endurance.

WASAPI enumeration is tested locally; physical microphone capture, hot-unplug behavior with real hardware, and long-session durability still need qualification. No hardware recording is claimed on the basis of simulated tests. Linux/macOS builds have not been run locally.

Not yet implemented: live input preview before Record, audio monitoring/playback inside the app, a persistent searchable take library, selectable channels on multichannel devices, automatic recovery, disk-capacity preflight, rendered video proxies, XML conform, and web export. Simulated depth and numerical-depth FFV1 export are available separately. [Physical Kinect depth](kinect-implementation.md) is now implemented using Microsoft SDK 2.0. The interface and source/storage separation allow these to be added incrementally.
