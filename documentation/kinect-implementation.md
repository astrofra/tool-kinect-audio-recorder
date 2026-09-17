# Microsoft Kinect v2 depth capture

This backend uses the [Kinect for Windows SDK 2.0](https://www.microsoft.com/en-us/download/details.aspx?id=44561) and its native [IDepthFrame interface](https://learn.microsoft.com/en-us/previous-versions/windows/kinect/dn772983(v=ieb.10)). It implements depth acquisition alongside the existing WASAPI or real-time simulated audio. It does not implement the Kinect audio-beam API, RGB, skeleton tracking, playback or clock fitting.

## Build and runtime

Windows x64 builds detect `Kinect.h` under `KINECTSDK20_DIR/inc` or the standard SDK installation. `RECORDER_ENABLE_KINECT` defaults to ON; an absent SDK selects a stub with an actionable error, preserving portable simulation. OFF explicitly selects that stub, even if headers were previously cached. Proprietary SDK headers and runtime binaries are not vendored or redistributed.

The backend dynamically loads the installed `Kinect20.dll` from Windows System32 and resolves `GetDefaultKinectSensor`. This avoids a startup dependency on Kinect for ordinary microphone/simulation use. COM, the sensor, reader and DLL are released on the acquisition thread. The installer supplies the runtime and driver; installing headers alone is insufficient for capture. `recording_tool kinect-info` warms the sensor and prints its ID, format, reliable range, one native timestamp and calibration.

Microsoft lists legacy Windows 8/8.1 support on the SDK download page. A successful local Windows 11 test is not a general compatibility guarantee for other controllers/drivers or a two-hour reliability qualification.

## Acquisition and shutdown

One thread opens the default Kinect v2 and depth reader, verifies 512 x 424, and waits at most 15 seconds for a warm-up image before creating the take. It saves the SDK depth intrinsics and device ID. Acquisition polls `AcquireLatestFrame` with two-millisecond waits for `E_PENDING`; other HRESULT failures are reported. A five-second absence of frames now logs a warning and retries the same source, allowing audio to continue. `--strict` retains interruption on read errors. Both waits respond to Stop. Initial open/calibration failure remains fatal.

Each image copies the SDK uint16 millimetres, including zero and values outside the reported reliable range. No mirroring, quantization, filtering or frame duplication occurs. Record indices count stored images, not hardware exposure numbers; `source_frame` retains the delivered index even after a queue drop. Each image stores `RelativeTime`, the host monotonic receipt tick observed before copying, and minimum/maximum reliable distances. A sensor-time interval over 50 ms is marked as a gap; no exact hardware drop count is inferred. In tolerant mode a non-increasing native timestamp starts a new `timestamp_epoch` and segment, with a warning; original timestamps are unchanged. Strict mode still interrupts on a clock reset.

Depth and audio acquisition share a writer thread through separately bounded queues: at most 60 queued depth images (about 26 MB), plus the existing bounded audio queue. The SDK read buffer is reused; queued frames own their copied storage. Preview receives immutable stored frames and cannot hold an SDK frame handle. Default tolerant capture skips incoming images on queue overflow with an explicit warning, and retries acquisition failures without stopping audio. Strict mode stops both streams. Disk failures remain fatal and retain the previous consistent checkpoint. A take with no stored depth can finish with a `depth_empty` warning, retaining available audio; it cannot look like a clean capture. Checkpoints also advance from depth receipt when audio stalls. Warm-up frames are discarded; capture can include small independent start/end offsets, which remain observable in the journals.

## Archive

Physical takes use `kinect-depth-audio-prototype/2`, kind `kinect-depth-audio-capture`. Audio WAVs and packet journals are unchanged. The `depth` manifest object identifies `kinect-sdk-2.0`, the device, intrinsics, native raster, nominal 30 Hz, frame count and number of detected gap intervals. Intrinsics come from [ICoordinateMapper::GetDepthCameraIntrinsics](https://learn.microsoft.com/en-us/previous-versions/windows/kinect/dn772971(v=ieb.10)); exact SDK deprojection equivalence still needs qualification before point-cloud playback.

Native `.kd16` files retain the 64-byte little-endian layout documented for simulation, with these differences:

| Field | Physical capture |
| --- | --- |
| Magic, bytes 0–7 | `KD16RAW\0` |
| Version, bytes 8–11 | 1 (this binary layout; separate from manifest schema) |
| Frame rate, bytes 24–31 | Nominal 30/1, **not** presentation timing |
| First/count, bytes 32–47 | Delivered record indices/count |
| Audio rate, bytes 48–51 | Actual audio rate, for identification only |
| Finalized flag, bytes 52–55 | 1 only after successful segment closure |
| Audio position, bytes 56–63 | Reserved zero; no implied synchronization |

Payloads are tightly packed row-major uint16 millimetres. Depth rotation uses elapsed native time since the first stored frame; gaps may produce segments with fewer images. Filenames enumerate depth files independently of audio. Empty intervals do not create fabricated images or empty segments.

`timing/depth-frames.jsonl` stores the segment path, byte offset, local/global record index, `source_frame`, `timestamp_epoch`, `relative_time_100ns`, `receipt_ticks`, `sensor_delta_100ns`, `gap_before`, `min_reliable_mm` and `max_reliable_mm`. Sensor and host ticks must not be equated. There is deliberately no fabricated `pts_100ns` or `audio_sample_floor`. The host frequency and audio timestamp observations are preserved for a later calibrated offset/drift fit. Audio synchronization accuracy has not yet been measured.

The archival simulation FFV1 exporter (`export-depth` / `--encode-depth`) still rejects `KD16RAW`. Synchronized archival export needs an explicit timing solution.

The separate `export-preview --input TAKE` / `--encode-preview` path creates a **silent RGB review**, also available automatically after Stop in the GUI. It combines every finalized depth segment into `video/preview-rgb.mkv`, using the same shared 500..6000 mm blue/green/red palette as the live preview. Encoding uses FFV1 `bgr0`, at 512x424 and 30 fps, streamed directly into FFmpeg; originals are not modified. Finalized interrupted takes with consistent depth files can also be reviewed.

The review starts at the first depth receipt and uses `(receipt_ticks - first_receipt_ticks) / host_clock_frequency`, rounded to the nearest playback slot. Multiple arrivals in one slot keep the latest image; empty slots repeat the preceding image. Thus long gaps retain their duration and clock resets do not shorten the movie. The final image occupies one playback frame. This receipt-based visualization includes delivery jitter; it is not a calibrated sensor timeline or audio synchronization. The Matroska metadata identifies the palette, timing policy and source frame count. Files are published from `.part` only after successful encoding; failure leaves raw inputs untouched.

## Verification

CTest injects a depth source through the same acquisition/queue/writer path. It verifies all uint16 values, native timestamps and gaps, time-based rotation, independent audio counts, finalization, calibration/ID persistence, source failure, clock reset, cancellation during warm-up, absence of depth, and rejection by the constant-rate exporter. Existing simulation, WASAPI-independent audio and FFV1 round-trip tests remain in place. Hardware smoke-test results, when available, are recorded separately below.

`audio_recorder --kinect-smoke-test NEW_DIRECTORY` records three seconds of physical depth with simulated audio in a hidden window and saves an adjacent PPM framebuffer. It requires a connected sensor and runs at approximately display cadence.

### Local hardware checks, 17 September 2026

Microsoft SDK 2.0.1410.19000, Windows 11 x64, MSVC 2022; the official runtime/SDK/driver installer completed with exit code 0 and no restart. Windows initially reported missing driver code 28 and then reported the Kinect sensor as OK.

- `kinect-info` received 512 x 424 uint16 frames, a 500–4500 mm reliable range and valid depth intrinsics. The warm-up now waits for intrinsics as well as frames: a first frame can arrive before valid calibration after reopening the sensor.
- Five seconds of Kinect depth with real-time simulated audio produced 150 images and 240,000 audio sample frames, with zero detected depth gaps. Native inter-frame intervals were 32.663–34.338 ms. Raw headers, payload sizes, monotonic timing and segment finalization were independently inspected.
- Three seconds of Kinect depth plus the WASAPI **VB-Audio Virtual Cable** input produced 90 images and 144,000 stereo sample frames at 48 kHz, with zero detected depth gaps. This verifies the concurrent WASAPI path, not physical microphone sound quality.
- Hidden-window GUI runs rendered the real depth map and finalized successfully, but recorded 87 images and two gap intervals, both before and after limiting the smoke renderer to approximately 60 Hz. The latter test ran alongside CTest. The observed gaps remain in the archive and controller status. Their cause has not been established; these brief tests do not establish sustained loss-free capture.
- The **Xbox NUI Sensor microphone** endpoint advertises 48 kHz stereo here but reports discontinuities/device-position inconsistencies (positions 0 then 160, while the first packet contains 463 frames). The original strict policy interrupted the take. With the new default warning policy, a five-second joint capture completed with 240,000 stereo audio sample frames, 149 depth images, zero detected depth gaps and 499 recorded audio warnings. This permits end-to-end testing without claiming valid device positions or calibrated synchronization. The Kinect SDK audio-beam path is not implemented.
- All seven CTest checks pass. A separate `RECORDER_ENABLE_KINECT=OFF` build compiles, records simulation and reports an explicit unsupported-build error for `kinect-info`.
- The updated static-runtime package completed two successive Kinect/microphone takes using the same timestamped prefix: each contained 96,000 stereo audio sample frames (two seconds), 60/61 depth images, no detected depth gaps and 200 journaled audio warnings. Both returned exit code 0; distinct paths, warning counts and committed journal lengths were checked independently. The packaged GUI simulation/FFmpeg smoke test also passed.

Physical microphone reliability, unplug/reconnect endurance, long recordings, loss under load and calibrated audiovisual offset/drift still require validation.
