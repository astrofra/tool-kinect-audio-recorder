# Microsoft Kinect v2 depth capture

This backend uses the [Kinect for Windows SDK 2.0](https://www.microsoft.com/en-us/download/details.aspx?id=44561) and its native [IDepthFrame interface](https://learn.microsoft.com/en-us/previous-versions/windows/kinect/dn772983(v=ieb.10)). It implements depth acquisition alongside the existing WASAPI or real-time simulated audio. It does not implement the Kinect audio-beam API, RGB, skeleton tracking, playback or clock fitting.

## Build and runtime

Windows x64 builds detect `Kinect.h` under `KINECTSDK20_DIR/inc` or the standard SDK installation. `RECORDER_ENABLE_KINECT` defaults to ON; an absent SDK selects a stub with an actionable error, preserving portable simulation. OFF explicitly selects that stub, even if headers were previously cached. Proprietary SDK headers and runtime binaries are not vendored or redistributed.

The backend dynamically loads the installed `Kinect20.dll` from Windows System32 and resolves `GetDefaultKinectSensor`. This avoids a startup dependency on Kinect for ordinary microphone/simulation use. COM, the sensor, reader and DLL are released on the acquisition thread. The installer supplies the runtime and driver; installing headers alone is insufficient for capture. `recording_tool kinect-info` warms the sensor and prints its ID, format, reliable range, one native timestamp and calibration.

Microsoft lists legacy Windows 8/8.1 support on the SDK download page. A successful local Windows 11 test is not a general compatibility guarantee for other controllers/drivers or a two-hour reliability qualification.

## Acquisition and shutdown

One thread opens the default Kinect v2 and depth reader, verifies 512 x 424, and waits at most 15 seconds for a warm-up image before creating the take. It saves the SDK depth intrinsics and device ID. Acquisition polls `AcquireLatestFrame` with two-millisecond waits for `E_PENDING`; other HRESULT failures are reported. A five-second absence of frames interrupts capture. Both waits respond to Stop.

Each image copies the SDK uint16 millimetres, including zero and values outside the reported reliable range. No mirroring, quantization, filtering or frame duplication occurs. Record indices count delivered images, not hardware exposure numbers. Each image stores `RelativeTime`, the host monotonic receipt tick observed before copying, and minimum/maximum reliable distances. A sensor-time interval over 50 ms is marked as a gap; no exact hardware drop count is inferred. A non-increasing native timestamp interrupts the take rather than joining clock epochs.

Depth and audio acquisition share a writer thread through separately bounded queues: at most 60 queued depth images (about 26 MB), plus the existing bounded audio queue. The SDK read buffer is reused; queued frames own their copied storage. Preview receives immutable stored frames and cannot hold an SDK frame handle. On queue overflow or acquisition failure, both streams stop and queued records drain. Disk failures retain the previous consistent checkpoint. A take with no recorded depth cannot be marked Complete. Warm-up frames are discarded; capture can include small independent start/end offsets, which remain observable in the journals.

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

`timing/depth-frames.jsonl` stores the segment path, byte offset, local/global record index, `relative_time_100ns`, `receipt_ticks`, `sensor_delta_100ns`, `gap_before`, `min_reliable_mm` and `max_reliable_mm`. Sensor and host ticks must not be equated. There is deliberately no fabricated `pts_100ns` or `audio_sample_floor`. The host frequency and audio timestamp observations are preserved for a later calibrated offset/drift fit. Audio synchronization accuracy has not yet been measured.

The simulation FFV1 exporter rejects `KD16RAW`, and CLI/GUI prevent background encoding for physical capture. Retiming native frames to a constant cadence without an explicit timing solution would hide capture gaps and could desynchronize audio. Native recordings remain the source for that future export path.

## Verification

CTest injects a depth source through the same acquisition/queue/writer path. It verifies all uint16 values, native timestamps and gaps, time-based rotation, independent audio counts, finalization, calibration/ID persistence, source failure, clock reset, cancellation during warm-up, absence of depth, and rejection by the constant-rate exporter. Existing simulation, WASAPI-independent audio and FFV1 round-trip tests remain in place. Hardware smoke-test results, when available, are recorded separately below.

`audio_recorder --kinect-smoke-test NEW_DIRECTORY` records three seconds of physical depth with simulated audio in a hidden window and saves an adjacent PPM framebuffer. It requires a connected sensor and runs at approximately display cadence.

### Local hardware checks, 17 September 2026

Microsoft SDK 2.0.1410.19000, Windows 11 x64, MSVC 2022; the official runtime/SDK/driver installer completed with exit code 0 and no restart. Windows initially reported missing driver code 28 and then reported the Kinect sensor as OK.

- `kinect-info` received 512 x 424 uint16 frames, a 500–4500 mm reliable range and valid depth intrinsics. The warm-up now waits for intrinsics as well as frames: a first frame can arrive before valid calibration after reopening the sensor.
- Five seconds of Kinect depth with real-time simulated audio produced 150 images and 240,000 audio sample frames, with zero detected depth gaps. Native inter-frame intervals were 32.663–34.338 ms. Raw headers, payload sizes, monotonic timing and segment finalization were independently inspected.
- Three seconds of Kinect depth plus the WASAPI **VB-Audio Virtual Cable** input produced 90 images and 144,000 stereo sample frames at 48 kHz, with zero detected depth gaps. This verifies the concurrent WASAPI path, not physical microphone sound quality.
- Hidden-window GUI runs rendered the real depth map and finalized successfully, but recorded 87 images and two gap intervals, both before and after limiting the smoke renderer to approximately 60 Hz. The latter test ran alongside CTest. The observed gaps remain in the archive and controller status. Their cause has not been established; these brief tests do not establish sustained loss-free capture.
- The **Xbox NUI Sensor microphone** endpoint advertises 48 kHz stereo here but reports a discontinuity in its second WASAPI packet (device positions 0 then 160, while the first packet contains 463 frames). The same failure reproduces in audio-only capture. The recorder interrupts and journals the fault; this endpoint is not qualified for reliable audio on this PC. No discontinuity checks were relaxed to make it pass. Use an independently validated microphone; the Kinect SDK audio-beam path is not implemented.
- All seven CTest checks pass. A separate `RECORDER_ENABLE_KINECT=OFF` build compiles, records simulation and reports an explicit unsupported-build error for `kinect-info`.

Physical microphone reliability, unplug/reconnect endurance, long recordings, loss under load and calibrated audiovisual offset/drift still require validation.
