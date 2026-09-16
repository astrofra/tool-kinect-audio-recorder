# Synchronization and recording format

Version: **0.1 proposal**. Date: **16 September 2026**.

This is a proposed timing and storage contract, not a claim that the hardware already meets the accuracy targets. The first implementation milestone must verify timestamp behavior on the actual devices.

## 1. Direct answers

**Do we need timecode?** We need timestamps on a shared timeline. SMPTE/LTC timecode is unnecessary for one PC, one Kinect, and one audio input. Timecode labels do not by themselves lock device clocks or remove transport latency. This design does not assume a supported common hardware clock or trigger connection between the Kinect v2 and the sound card.

**Do we need synchronization packets inside a single stream?** Every depth frame needs a timestamp, and every audio block needs a sample position and a mapping to the common timeline. Those records may live in separate files within one versioned session folder. A single interleaved container is optional; multiplexing alone does not synchronize independent clocks.

**Can we start both and hope?** No. Capture should start before Record, clocks should be observed throughout the take, and playback must follow explicit timing. Starting two APIs consecutively does not establish either a shared origin or a shared clock rate.

## 2. Clock domains and units

| Domain | Raw observation | Purpose |
| --- | --- | --- |
| Windows host | `QueryPerformanceCounter` and its frequency | Monotonic reference relating the two devices. |
| Kinect | Native timestamp and frame sequence | Sensor ordering, spacing, losses, and rate estimation. |
| Audio input | First sample-frame position and correlated WASAPI QPC timestamp | Accurate placement of captured audio blocks on the host timeline. |
| Published media | Integer presentation timestamps relative to exported audio sample zero | Independent, portable desktop/browser playback. |
| UTC | Human-readable session creation date | Library sorting and metadata only. |

Windows documents QPC as a high-resolution interval clock independent of external time-of-day references. Do not use UTC or a wall clock that can be corrected during recording. [Microsoft QPC guidance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).

Use signed 64-bit integer **microseconds** for stored session and published presentation timestamps, with `time_base = 1/1000000`. Keep native counters in their original integer types as well. Center calculations near a reference observation, and use overflow-safe conversion; do not multiply an arbitrary absolute counter by one million in a narrow integer type.

### Kinect observations

The libfreenect2 frame header defines a 32-bit timestamp with **125 microseconds per tick**, a sequence number, and frame status. Preserve all three, plus QPC sampled immediately on application callback entry. A frame callback occurs after decoding; that observation is not an exposure timestamp. [Pinned frame API](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/include/libfreenect2/frame_listener.hpp).

Unwrap the timestamp to a 64-bit counter within an uninterrupted device epoch. A full 32-bit cycle at this unit is about **6.21 days**. A reconnect, restart, implausible backward jump, or sequence discontinuity indicating reset starts a new epoch instead of being blindly interpreted as wraparound. In the initial product, a device reset ends the take. Include synthetic wraparound tests even though normal takes are much shorter.

Verify the timestamp unit and sequence cadence experimentally. Determine the practical meaning of the sensor timestamp relative to the depth exposure/processing cycle; the header does not establish a complete exposure-to-QPC calibration. If necessary, instrument the library at an earlier USB packet stage while retaining the same observation model.

### Audio observations

Use event-driven WASAPI capture. `IAudioCaptureClient::GetBuffer` supplies the first audio-frame device position and a correlated QPC timestamp **already converted to 100-nanosecond units**. It also provides silence, data-discontinuity, and timestamp-error flags. A sample frame contains one sample per channel. Preserve these fields and release the buffer promptly. [Microsoft GetBuffer reference](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer).

Do not confuse returned 100 ns values with raw QPC ticks. Convert the session origin to the same units before subtraction. Store the original device sample position separately from the sample's offset in a WAV file: they diverge when data is missing or files rotate. Invalid timestamps are observations with a quality flag, not valid anchors for fitting.

Prefer shared-mode capture for initial endpoint compatibility; inspect the actual mix format and channel layout. An endpoint exposing only stereo may still contain a mono interview channel. Preserve the selected channel mapping. Explicit exclusive-mode support can follow if required by a tested interface; ASIO is outside the initial design.

## 3. Mapping clocks and estimating drift

For a stable interval, use centered affine mappings:

```text
H_audio(n)  = A_audio  + B_audio  * (n - n_ref)
H_kinect(k) = A_kinect + B_kinect * (k - k_ref)
```

`H` is host time in seconds, `n` is audio sample-frame position, and `k` is unwrapped Kinect ticks. Nominal slopes are `1 / negotiated_sample_rate` and `0.000125` seconds respectively. Fit the actual slopes rather than forcing nominal rates forever.

The WASAPI observations directly associate audio position with host time. The Kinect observations instead have the form:

```text
H_arrival(k) = H_capture(k) + transport_and_decode_latency(k)
```

A proposed estimator uses a robust fit over 30–60 second windows, downweights outliers, and tracks the lower-latency envelope of arrivals. This can reduce the effect of scheduling spikes; it does **not** reveal an unknown fixed transport delay. Sustained changes in decoding load can also look like clock drift. Log fit residuals, observation quality, and latency distribution, and qualify the estimator under load.

Retain observations throughout recording. Compute provisional models for live diagnostics, then a final timing solution offline using the whole take. Use a piecewise model only when evidence justifies it, with continuity constraints and explicit epoch boundaries. A model update must not create backward time or shift already-written raw observations.

Report relative drift in ppm, fit residual statistics, and the fraction of rejected observations. Small residuals indicate internal fit consistency, not proof of physical audio/depth alignment. Timestamp errors or unexplained jumps must lower the take's timing confidence.

`SyncMultiFrameListener` is a facility for receiving Kinect frame types; its name is not a promise to synchronize an external microphone. Use the device timestamp model regardless of listener selection. [Upstream listener usage](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/doc/mainpage.dox).

## 4. Offset calibration and measurement

Use a physical event that changes depth and produces a short sound: for example, a clapboard or two rigid objects closing in view of the Kinect near the interview microphone. An ordinary light flash cannot be assumed visible in a depth-only recording. A hand clap is useful for field checks; a repeatable fixture is preferable for qualification.

Measure the event in both recorded streams. Account for the microphone's distance from the event and for any processing on the audio input. Fit an effective relative offset after clock-rate correction. Define a positive stored depth offset as **moving the depth event later** on the common timeline. Preserve original observations; offset adjustments belong to a named timing solution.

During qualification, repeat events near the beginning and at several positions through a two-hour take. Use multiple events per position to estimate quantization and detection uncertainty. Calibrate using a subset and assess the accuracy targets on separate events; fitting every test marker would not independently validate synchronization. During an interview, start/end reference events can detect setup changes or drift.

At 30 Hz, depth sampling itself limits temporal discrimination. Timestamp precision in microseconds does not mean exposure accuracy at that scale. Report marker-detection uncertainty and the distinction between capture alignment, stored timing, and audible/displayed playback alignment.

Store calibration profiles against sensor serial, audio endpoint/configuration, processing backend, and relevant driver versions. Changing these may invalidate the effective latency correction. Allow a manual offset adjustment during review and retain its provenance. If the target cannot be met consistently, improve timestamp instrumentation or qualify a different backend before claiming reliable interviews.

## 5. Capture timeline versus published audio timeline

The master uses QPC-relative time to retain a common acquisition reference. **Published playback uses audio sample time as the master.** This avoids resampling otherwise continuous audio merely to match Kinect clock drift.

For an uninterrupted audio interval, convert each corrected depth host time back to the corresponding audio position:

```text
n_at_depth = inverse_H_audio(H_kinect(k) + depth_offset_seconds)
P_depth   = (n_at_depth - n_export_start) / nominal_audio_rate
```

The audio file starts at exported sample zero. `P_depth` becomes the depth presentation timestamp, rounded to integer microseconds. Export duration follows the audio samples. This deliberately allows a tiny difference between wall-clock elapsed time and nominal media duration; both streams remain together.

If audio positions reveal a gap, construct a logical continuous sample timeline that includes the missing interval. Insert that much silence in the derivative audio file and preserve the gap marker. Never concatenate the two sides as if nothing happened. If the gap duration cannot be established reliably, flag the export for review rather than inventing precise timing. The normal recorder ends a take on endpoint loss, reducing ambiguity.

For a negotiated rate other than the chosen delivery rate, perform ordinary offline sample-rate conversion and carry the same media times through it. If a future export must follow strict host elapsed time, resample audio against the measured clock model instead; that is a different explicit export policy. Do not both warp depth to audio and independently stretch audio to host time.

For compressed audio, account for encoder priming, decoder delay, and end padding through the container/codec mapping. Store any remaining presentation start offset once. Validate every encoded alternative against the same source sample events; avoid compensating the same delay in both the file and the player.

## 6. Master recording package

Use a portable folder, with immutable captured payloads and separately editable metadata/timing solutions:

```text
take-<uuid>/
  manifest.json
  calibration.json
  appearance.json
  depth/
    000000.kdf
    000001.kdf
  audio/
    000000.wav
    000001.wav
  timing/
    audio-packets.jsonl
    events.jsonl
    solution-0001.json
  indexes/
    depth.idx
  preview.jpg
```

Initial settings rotate depth files every approximately two seconds and PCM WAV files every sixty seconds. Rotation does not reset timestamps, sample positions, or sequence numbers. Short WAV files avoid the classic RIFF 4 GiB size limit; a recovery tool can rebuild a damaged final header from intact PCM and the captured format. A future single-file audio option would require a large-file format such as RF64.

`KDF` and the web counterpart `KWD` are proposed project formats, not existing interchange standards. Their benefit is a small depth-specific reader and explicit recovery behavior; their cost is maintaining a documented schema, independent verification tool, and compatibility fixtures. Keep audio in a standard format and make the complete depth-format specification public alongside the reader before freezing version 1.

### Manifest and calibration

`manifest.json` must include schema version, take ID, creation UTC, take state, selected capture interval, stream descriptions, actual audio format, file list, calibration hash, application/dependency revisions, processing settings, and the active timing-solution ID. Write updates to a temporary sibling, flush, and replace safely so that a partial JSON rewrite cannot destroy the last valid manifest.

`calibration.json` stores sensor identity, original calibration parameters, distortion model, depth image dimensions, pixel-center convention, image orientation, units, coordinate axes, and processing/registration version. A per-pixel ray table or undistortion map may be stored as an additional versioned binary asset if required for reproducible reconstruction.

The uint16 profile maps invalid depth to zero and valid finite depth to rounded millimetres in the declared permitted range. Values outside that range become invalid and are counted. The float32 profile preserves decoded float samples and their invalid representation. Neither profile is a dump of the sensor's raw USB/IR measurements; recreating a different depth reconstruction algorithm would require an additional raw-data capture mode.

### Depth records

The following fields define the semantic contract. Before implementation freezes version 1, publish exact field offsets, enum values, and fixture files; do not serialize a compiler-dependent C++ struct directly.

| Field group | Required contents |
| --- | --- |
| File header | Magic, major/minor version, header size, take/stream IDs, byte order, format, calibration hash. |
| Record envelope | Record type, header size, stored/uncompressed payload lengths, flags, CRC, monotonically increasing record ID. |
| Sensor evidence | Clock epoch, raw timestamp, unwrapped timestamp, sequence, frame status, callback-entry raw QPC. |
| Derived timing | Provisional capture PTS in microseconds and timing-model ID; native evidence remains authoritative for refitting. |
| Depth payload | Dimensions, row stride, sample format, compression ID, complete depth raster. |

Use explicitly little-endian integers and IEEE 754 floats, fixed field widths, and a defined CRC32C coverage over serialized header and payload excluding the CRC field. Each depth frame is independently compressed with Zstandard or stored uncompressed if compression is ineffective. This permits random access and recovery without decoding all earlier frames. Cross-frame prediction is a later format extension.

Readers must cap allocation by declared format limits, verify lengths/checksums, and reject unsupported major versions. Optional unknown record types may be skipped by length. An index maps time/sequence to file and byte offset, but is a rebuildable optimization rather than the sole way to find frames.

### Audio packet timing

Each complete line of `audio-packets.jsonl` records:

- Audio file ID, first stored sample-frame offset, packet sample-frame count, and actual format ID.
- Clock epoch, first device sample-frame position, and original WASAPI QPC value in 100 ns units.
- Application receipt QPC, WASAPI flags, and timing validity.
- Provisional mapped session time and model ID, when available.

Use decimal strings for raw 64-bit counters in JSON so that JavaScript parsers cannot silently round values beyond their exact integer range. Session-relative microseconds may be JSON numbers only within the explicitly bounded safe-integer range. A silence-flagged packet still occupies its proper number of samples.

### Events, checkpoints, and recovery

Events record drops, data gaps, clock resets, invalid timestamps, queue overflow, disk errors, operator start/stop, and known uncertainty. Store both stream position and host time whenever available. Do not infer all failures exclusively from a final frame count.

The writer periodically flushes payload and timing data and commits a checkpoint describing mutually consistent prefixes. Recovery checks file headers, scans complete depth records, validates checksums, rebuilds indexes, repairs the final WAV length, and reconciles packet metadata with actual PCM. It ignores incomplete trailing records or JSON lines. Payload lacking trustworthy timing remains available for inspection but is marked uncertain. Recovery creates a report and never changes a failed take into an apparently flawless one.

## 7. Depth reconstruction for publication

Keep the captured camera grid and its calibration in the master. For the initial web format, export **undistorted** depth with explicit output intrinsics, so the shader does not need to replicate backend-specific distortion code.

For a defined output pixel-center coordinate `(u, v)` and axial depth `z` in metres:

```text
x = (u - cx) * z / fx
y = (v - cy) * z / fy
```

The actual output convention must be written in the manifest. The libfreenect2 reference implementation uses a half-pixel offset when computing XYZ from undistorted depth; do not copy intrinsics into a shader with a different pixel-center convention. Exporting cropped or downsampled images must adjust intrinsics or supply corresponding rays. [Reference deprojection implementation](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/src/registration.cpp).

Choose and declare camera coordinates as X right, Y down, Z forward, in metres; use an explicit view transform for the renderer's convention. Preserve invalid pixels and avoid interpolation across depth discontinuities. Appearance changes must not change the geometric timebase.

## 8. Playback clock and browser limits

For native playback, derive the played audio position from the WASAPI rendering clock, with the queue's source-sample mapping and output latency accounted for. Do not use the number of samples merely submitted to the output buffer. `IAudioClock::GetPosition` and `GetFrequency` define a device-position clock; do not assume its position units equal sample frames without the documented conversion. [Microsoft audio clock reference](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition).

For the initial browser player, use the audio element's media timeline and select the closest valid depth frame by PTS, with midpoint boundaries between adjacent valid frames. Buffer the next frame so this selection can choose either side. A known capture gap overrides normal selection: hold geometry for at most 100 ms, then show missing geometry while the audio timeline continues. Do not stretch adjacent frames across an arbitrary gap.

Network buffering is different: if required existing data is unavailable, pause audio and freeze its actual position until depth and audio are ready. After a seek, use the audio element's resolved position rather than assuming it exactly equals the requested value.

The media timeline does not promise sample-accurate correspondence to the speaker at the moment JavaScript reads it. Audio output latency, browser time precision, display refresh, and compositor scheduling can affect physical alignment. Measure the initial `HTMLAudioElement` design against the browser acceptance tests. It is appropriate to prototype, but its timing target remains a gate.

If it fails, move to an explicitly scheduled Web Audio playback path, mapping media samples to the audio context clock. `AudioContext.getOutputTimestamp()` can relate that context to output/performance time, but only after establishing the media-to-context mapping; it cannot simply replace an HTML audio element's time with an unrelated clock. Streaming decode, seeking, sample accounting, and output-latency handling would then become additional implementation work. [Web Audio timing API](https://www.w3.org/TR/webaudio/#dom-audiocontext-getoutputtimestamp).

Browser robustness comes from bounded buffering, independent timestamps, explicit state transitions, and audio-led frame selection. Browser scheduling jitter must cause a temporary rendering miss, not a progressively diverging interview.
