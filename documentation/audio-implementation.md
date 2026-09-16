# Audio recorder implementation: first milestone

This document describes implemented behavior. The [product specification](specification.md) remains the broader target. The implementation language is **C++11**, superseding the original C++20 recommendation.

## Available components

- `recording_tool`: record audio or enumerate Windows capture endpoints.
- `audio_recorder`: optional Dear ImGui/GLFW interface with source selection, Record/Stop, meters, duration, take path, and current-session history.
- `recorder_core`: portable source interface, deterministic simulator, bounded packet queue, background file writer, float32 WAV serialization, and recording state/control.
- `WasapiSource`: Windows shared-mode, event-driven capture using native Windows SDK APIs. COM objects are created, used, and released on the capture thread.

There are no runtime dependencies for the CLI beyond the platform's C/C++ runtime. The optional GUI statically compiles pinned Dear ImGui v1.92.9b and GLFW 3.5.1 sources. The GUI needs an OpenGL 3.3 driver; simulation through the CLI does not need graphics or audio hardware. The application does not install or replace drivers.

The recorder accepts another `AudioSource` implementation through the same interface; tests use this to inject timestamp errors, sequence gaps, and source failures. This does not couple future Kinect capture to the audio device API.

## Simulation

Default: 48 kHz, mono, amplitude 0.25, 440 Hz base tone, plus an 80 ms 1 kHz marker each sample-clock second. Markers have 5 ms ramps. `--signal sine` produces only the base tone; the second stereo channel uses 1.5 times the base frequency. `--amplitude 0` generates silence. Simulation writes samples to the recorder and meters; it does not play sound through the speakers.

Real-time mode schedules packets using the monotonic host clock. `--fast` produces the same samples without real-time waiting and explicitly marks timing as `synthetic-unpaced`. Its generated sample timestamps can run ahead of the real receipt clock; those observations must never be used as measurements of device latency. Fast mode waits for writer capacity, while real-time sources stop with an error if the bounded queue fills.

Duration is converted once to a whole number of sample frames at the actual sample rate. The final packet is trimmed precisely. A sample frame contains one sample per channel. A zero duration records until Stop/Ctrl+C; unbounded fast simulation is rejected.

## Native Windows capture

`devices` returns active input names and opaque endpoint IDs. Capture opens the chosen endpoint or the current default once. The actual mix rate and channel layout are retained; this version accepts mono/stereo float32 and integer PCM input with 8, 16, 24, or 32-bit containers, converting samples to float32 storage. Multichannel selection and resampling are deferred.

WASAPI provides a device sample position and a correlated QPC value in 100 ns units for each packet. Store the original values and flags, including invalid timestamps and silent packets. Integer conversion does not turn a device into a higher-resolution ADC. The native API behavior follows Microsoft's [capture buffer contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) and [event-driven initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize).

The initial WASAPI discontinuity flag is retained without treating startup as mid-take loss. Subsequent discontinuities or valid device-position gaps interrupt the take after preserving the received packet and draining queued data. Timestamp-error packets retain their samples and are excluded from continuity comparisons. No packets for five seconds is an explicit capture failure, not a silently successful empty take.

## File contract

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

WAV files contain little-endian IEEE float32 samples, a format chunk, a sample-count `fact` chunk, and a data chunk. They rotate at the configured sample boundary; rotation may split a source packet. Default rotation is 60 seconds, keeping each file below RIFF size limits even for long takes.

The manifest identifies source kind, selected device, actual sample format/channel mask, clock origin/frequency, simulation recipe, file list, total stored frames, and state. UTC is descriptive metadata. Successful completion follows queue draining, WAV header finalization, file flushing, and manifest replacement. An output directory must be new; the recorder never erases an existing take.

Each JSONL audio entry records the file and sample offset, stored frame count, original packet device position, packet offset when split across files, source timestamp in 100 ns units, raw receipt-clock ticks, and flags. Raw counters are decimal strings to preserve all 64 bits when parsed by JavaScript. When a packet is split, its original timestamp is repeated: the first sample in a record is at `packet_offset_frames` after that timestamp. The native device frame for that sample is `device_frame + packet_offset_frames` when valid.

On Windows, raw receipt ticks use QPC and the saved frequency. Other platforms use a monotonic nanosecond clock with frequency 1 GHz. This milestone preserves observations; it does not yet estimate cross-device clock drift or Kinect latency.

Checkpointing occurs after approximately one second of stored audio, and at orderly finalization. WAV headers and packet/event logs are flushed before an atomic checkpoint describing their committed prefix. A writer failure does not advance the known-good checkpoint. The manifest is also replaced through a temporary sibling. This supports future recovery tooling but does not yet implement a `recover` command or guarantee power-loss durability.

If data is missing, the stored PCM may be a compact prefix plus the flagged packet following a gap; the journal carries the original positions. An Interrupted take must not be treated as a gap-free WAV timeline. Silence insertion and repaired playback/export remain future work.

## Validation and limits

The C++ tests exercise sample-clock arithmetic, signal bounds, stop/drain, reuse, overwrite refusal, bad timestamps, source exceptions, and device-position gaps. An independent Python standard-library test parses WAV chunks and JSON, checks sample values on both channels, exact duration, packet continuity, partial packets, rotation across packet boundaries, Unicode paths, silence, and invalid CLI arguments.

`audio_recorder --smoke-test NEW_DIRECTORY` creates a hidden window, records 250 ms of simulation through the GUI's recorder path, finalizes it, and saves an adjacent PPM framebuffer capture. This tests renderer/recorder integration without a visible desktop window or a microphone. Normal operation omits this argument.

Local validation used Visual Studio 2022/MSVC x64 on Windows: both CTest suites passed, the hidden-window GUI smoke test completed, and a 65-second stereo simulation produced exactly 3,120,000 sample frames with 60-second/5-second WAV rotation. FFprobe recognized the format as `pcm_f32le`, and FFmpeg decoded the generated audio. This accelerated run verifies file generation/rotation, not 65 seconds of real-time hardware endurance.

WASAPI enumeration is tested locally; physical microphone capture, hot-unplug behavior with real hardware, and long-session durability still need qualification. No hardware recording is claimed on the basis of simulated tests. Linux/macOS builds have not been run locally.

Not yet implemented: live input preview before Record, audio monitoring/playback inside the app, a persistent searchable take library, selectable channels on multichannel devices, automatic recovery, disk-capacity preflight, Kinect capture, video proxies, XML conform, and web export. The interface and source/storage separation allow these to be added incrementally.
