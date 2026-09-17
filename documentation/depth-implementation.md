# Simulated depth capture and lossless video export

Implemented in C++11, with no new recording-time dependency. The GUI can record
a simulated depth raster alongside simulated or WASAPI audio. Actual Kinect USB
capture is now available through the separate [Microsoft SDK backend](kinect-implementation.md).
Point-cloud rendering and browser playback remain future work. This simulation milestone needs no Kinect driver.

## Depth is an image of distances

Kinect v2 depth is a 512 x 424 raster, nominally 30 frames/s. Libfreenect2 exposes
float32 values in millimetres; non-positive values, NaNs and infinities represent
invalid/missing depth. See the upstream [frame definition](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/include/libfreenect2/frame_listener.hpp).

This simulator generates **uint16 millimetres**, with zero meaning invalid depth.
Converting future sensor floats to that representation will require explicit
validation and rounding, introducing up to 0.5 mm rounding error for valid,
in-range distances. That conversion is not implemented here. Preserving original
float32 sensor values requires a different archival profile.

A uint16 depth frame is 434,176 bytes: about 13.03 MB/s or 46.89 GB/hour before
compression. The original pixels and their timing are the master; the GUI's
8-bit false-color preview is only a visualization.

## Capture and preview

The GUI defaults to **Simulated gradient** in its Depth source selector. Choose
**Simulated noise** for a less compressible signal, or **Off** for audio only.
Record/Stop controls both streams. The large timecode continues to follow stored
audio; the preview shows the most recently stored depth image and its frame count.

```powershell
.\release\recording_tool.exe record --output recordings\depth-test --duration 10 --depth gradient
.\release\recording_tool.exe record --output recordings\noise-test --duration 10 --depth noise --fast
```

The CLI keeps audio-only capture as its default. Both depth patterns are
deterministic functions of pixel coordinates and frame index, independent of GUI
speed, packet boundaries and pacing. Valid distances are 500..6000 mm. The border
is invalid (zero). A 32 x 32 marker near the top-left corner changes distance for
100 ms at every sample-clock second; default simulated audio has an 80 ms marker
at the same start time. The gradient translates over time; the noise pattern
changes each frame using explicitly unsigned integer arithmetic.

Generation and writing run on the existing disk worker. The audio acquisition
thread remains separate, with its bounded queue and explicit overflow failure.
The GUI receives one immutable shared frame, not an ever-growing frame history.
This synthetic source is driven by stored audio; it is not a model of independent
Kinect/device clocks or a validation of real microphone-to-Kinect latency/drift.

## Timing and storage

Frame `n` has presentation time `n/30` seconds on the stored-audio timeline. For
`S` audio sample frames at rate `R`, store `ceil(S * 30 / R)` depth frames. This
covers the half-open interval `[0, S/R)` with a frame at zero. Zero audio produces
zero depth frames. Calculation uses integer arithmetic, including sample rates
not divisible by 30. Stop drains both streams; failures mark the take Interrupted.
Depth follows the stored PCM sequence even if an interrupted audio take contains
a device-position gap. Its journals must not be treated as repaired device timing.

Depth-enabled takes use schema `kinect-depth-audio-prototype/1`; audio-only takes
retain `kinect-audio-prototype/1`. The manifest adds a `depth` object describing
the source/pattern, dimensions, rational frame rate, units, invalid value, clock,
frame count and segment list. Calibration is explicitly `null`.

```text
take/
  manifest.json
  checkpoint.json
  audio/000000.wav
  depth/000000.kd16
  timing/audio-packets.jsonl
  timing/depth-frames.jsonl
  timing/events.jsonl
```

Depth and audio rotate at the same whole-second interval, default 60 seconds.
`depth/000001.kd16` pairs with `audio/000001.wav`, and so on. A partial final
depth frame can extend the displayed video duration by less than 1/30 second
beyond the audio end. No samples are added to or removed from audio to hide this.

KD16 is a small **uncompressed prototype format**, distinct from the proposed
compressed production archive in [the format specification](synchronization-and-format.md).
Each segment starts with this 64-byte header; all integers are little-endian:

| Byte offset | Type | Meaning |
| --- | --- | --- |
| 0 | 8 bytes | `KD16SIM` followed by a zero byte |
| 8 | uint32 | Version: 1 |
| 12 | uint32 | Header size: 64 |
| 16, 20 | uint32 each | Width: 512; height: 424 |
| 24, 28 | uint32 each | FPS numerator: 30; denominator: 1 |
| 32 | uint64 | First frame index on the take timeline |
| 40 | uint64 | Number of complete frames in this segment |
| 48 | uint32 | Actual audio sample rate |
| 52 | uint32 | Finalized flag: 0 while writing, 1 after orderly segment close |
| 56 | uint64 | First corresponding audio sample index |

The payload is tightly packed, row-major uint16 depth: no color conversion,
normalization, gamma correction or row padding. Frame bytes start at
`64 + local_frame_index * 434176`. Headers are checkpointed with their frame count
and finalized on close. An interrupted process may leave an unfinished tail;
automatic repair/recovery is not implemented.

Each depth journal entry records global/local frame indices, segment path, byte
offset, relative PTS in 100 ns units, floor of the corresponding audio sample
position, and the host ticks when the synthetic frame was generated. Large raw
counters use decimal strings. Exact presentation time is the rational `frame/30`;
integer PTS and audio positions are floors of that value, not separate clocks.
Generation ticks measure writer activity and must not be interpreted as sensor
capture timestamps. Checkpoints include the depth frame count and journal bytes,
after flushing depth, audio and their journals.

## FFV1 export

**Yes, numerical depth can be encoded as video without losing its uint16 values.**
[FFV1](https://ffmpeg.org/~michael/ffv1.html) is an open-source lossless intra-frame
codec. FFmpeg's [encoder implementation](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/ffv1enc.c)
supports `gray16le`. Use that exact format, without converting the raster to an
ordinary 8-bit color image.

```powershell
.\release\recording_tool.exe export-depth `
  --input recordings\depth-test\depth\000000.kd16 `
  --audio recordings\depth-test\audio\000000.wav `
  --output recordings\depth-test\depth-video.mkv
```

The command exports **one finalized segment**. It validates the KD16 header and
payload size; optional audio must have the recorder's float32 WAV layout, matching
sample rate and compatible duration. Select the WAV with the same segment number
from the same take: equal duration alone cannot prove source identity. Omit
`--audio` for depth-only video. Existing outputs are rejected. If FFmpeg fails,
any partial output is incomplete and must not be used as a successful export.

The Windows release bundles a minimal **FFmpeg 9.0.1** executable under
`release/extern/ffmpeg/`. Export finds it relative to the recorder executable,
independently of the working directory. `--ffmpeg C:\path\ffmpeg.exe` overrides
automatic selection; PATH is a fallback only when no bundled binary exists.
FFmpeg is launched directly with an argument list, without a shell; paths with
spaces, Unicode and shell metacharacters are supported. Capture and preview still
work independently of the encoder. Manual export errors are reported to the console.

The bundle includes LGPL notices, the exact unmodified source archive, SHA-256
pins, MSVC build scripts and generated configuration files. It is rebuilt from
source by `rebuild_ffmpeg.bat` and reused by `build_release.bat`, without external codec libraries, GPL/nonfree
components, network protocols, GPU encoding or assembly dependencies. See the
[FFmpeg package description](../extern/ffmpeg/README.md). The recorder remains
C++11; FFmpeg is an independently built C program.

The export uses Matroska, FFV1 version 3, range coding, intra frames, slice CRCs,
strict `gray16le`, and four CPU threads. Optional audio is PCM float32. Both streams
start at segment-local zero. Matroska metadata retains units, invalid value and
the source's first frame index. The original manifest and journals remain the
authority for the take timeline; container timestamps alone do not preserve all
sensor observations or the precision of the rational frame index.

This is an archival/data-video export, not the future rendered video proxy or
browser delivery format. It does not yet join multiple segments, add timecode
tracks, render geometry, or implement DaVinci Resolve XML conform.

## Background encoding queue

The GUI enables **Encode finished segments to Matroska in background** by default.
The CLI enables the same behavior with `record --depth gradient --encode-depth`;
`record --ffmpeg PATH --encode-depth` overrides automatic encoder selection.
Audio-only capture and manual export do not start an encoding worker.

The disk writer submits a job only after **both** the matching WAV and KD16 files
have been finalized and closed. Rotation submits full segments during capture;
Stop submits the last partial pair after draining audio and depth. The default
segment interval remains 60 seconds. No encoder reads an actively written segment.

One application-owned C++11 `std::thread`, one mutex, one condition variable and
a FIFO of paths implement the queue. There is no thread pool and no frame data in
the queue. The mutex is released before any file I/O or subprocess work. One
FFmpeg child runs at a time across successive takes in the same recorder instance.
Independent application instances have independent queues.

Background FFV1 encoding uses two codec threads and one thread per filter pool.
FFmpeg has additional internal I/O threads; this is not a two-thread limit for the
whole process. Windows creates the child at `IDLE_PRIORITY_CLASS`; POSIX attempts
to lower its priority with `setpriority(..., 10)` after spawning it. Only the child
is affected. [Windows documents CPU priority separately from I/O contention](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass):
this is resource moderation, not a promise of zero capture impact. Disk throughput,
free space and long-session behavior still require testing on the intended machine.
The [FFmpeg options](https://ffmpeg.org/ffmpeg.html) restrict codec/filter parallelism;
no GPU encoder or additional dependency is introduced.

The queue panel displays the current path, waiting count, successes and failures
across the application session. Stop does not wait for encoding. Once capture has
finalized, a new take can start while earlier jobs continue. On orderly window
close, capture stops and the GUI keeps rendering until the queue drains; **Keep
window open** cancels closing. The CLI drains the queue before exit. A failed job
does not stop capture or later jobs; the CLI returns nonzero if any job failed.
The capture manifest's `state` still describes capture, independently of encoding.

Background outputs are derived artifacts under the take's `video/` directory:

```text
video/000000.mkv        # Published only after successful encoding
video/000000.mkv.json   # encoding / complete / failed, source paths and error
video/000000.mkv.log    # FFmpeg stdout/stderr (empty on success is normal)
video/000000.mkv.part   # Present while encoding; retained on failure if created
```

FFmpeg must exit successfully and produce a Matroska header before the `.part`
file is published under its final name. Publication refuses to replace any existing
destination. This is not a full decode verification on every job; byte-exact codec
round trips are checked by the test suite. Originals and timing journals are never
deleted. Filesystem failures are reported even when status/log files cannot be
written. The manifest records whether background encoding was enabled for the take.

The pending queue is capped at 1024 jobs plus one running job. If it fills, new
jobs are rejected and counted as failures without waiting or stopping capture;
their raw segments remain available for manual export. Pending jobs live in memory,
and this version has no automatic restart recovery, retries or encoder cancellation.
Forced exit/crash can leave missing videos or `.part` files; recover by manually
exporting the corresponding finalized raw pairs to new output paths. The normal
close path waits for an encoder to return; a hung external encoder requires external
intervention. The queue is not a file watcher for earlier takes or external files.

## CPU versus GPU

This implementation uses the CPU FFV1 encoder. Conventional GPU video engines
target color-video formats: for example, [NVIDIA NVENC's documented capabilities](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html)
cover H.264/HEVC/AV1 with format/bit-depth limits depending on hardware, rather than
a direct gray16 depth archive. Lossless codec modes do not undo a prior lossy
pixel-format conversion. A reversible packing scheme might be evaluated later,
with byte-for-byte round-trip tests and actual GPU qualification. FFV1 is a
straightforward portable baseline; GPU compression is not implemented.

Initial local measurements on Windows/MSVC x64 used the external FFmpeg 7.0.2
(Gyan essentials), before the minimal 9.0.1 bundle was introduced:

| Three-second input | Depth frames | Raw depth bytes | FFV1 + mono PCM bytes | Export wall time |
| --- | ---: | ---: | ---: | ---: |
| Moving gradient | 90 | 39,075,904 | 2,730,977 | approximately 0.15 s |
| Changing noise | 90 | 39,075,904 | 39,076,528 | approximately 0.60 s |

Both real-time recordings stored exactly 144,000 audio sample frames at 48 kHz.
These short, cached, synthetic measurements are not Kinect compression ratios,
cross-machine performance promises or long-session endurance qualification.
Background encoding is now available; production capture still needs
measurements to choose between FFV1 and the previously proposed Zstandard chunks.

## Validation

CTest covers exact depth counts, frame boundaries, non-divisible sample rates,
segment rotation, file lengths/headers, invalid pixels, the marker, deterministic
motion/noise, journal/sample-clock mapping, checkpoints, Stop/drain and source
faults. Invalid/unfinished/truncated depth files and mismatched audio are rejected.
When FFmpeg is installed, tests encode and decode gradient/noise segments with
stereo audio, compare every depth byte and PCM sample, and test all 65,536 uint16
values independently of the simulator's range. Unicode paths, missing encoders
and existing-output refusal are also checked. Python is optional for tests only.

The package test also copies the recorder and encoder into a directory with spaces
and Unicode characters, clears PATH, and exports from a different working
directory. It verifies byte-exact depth recovery and explicit `--ffmpeg` precedence.
The release build runs codec tests using its newly compiled bundled encoder.

Queue tests block an injected encoder while finalizing and starting another take,
check FIFO execution, a single concurrent job, bounded overflow, continued processing
after failure and orderly destructor drain. A subprocess failure test checks the
Windows child priority and verifies that incomplete output is never published and
existing destinations are preserved. Integration tests encode rotated segments
during real-time simulation, compare all decoded depth/PCM bytes, check the final
partial segment and verify that a missing encoder leaves capture complete.

The hidden-window GUI smoke test now records both streams and captures the depth
preview alongside the timecode. Local visual inspection confirmed the preview.
Physical Kinect capture has separate [implementation and validation notes](kinect-implementation.md).
Float conversion, device drift, GPU encoding, web decoding and non-Windows execution remain unqualified.
