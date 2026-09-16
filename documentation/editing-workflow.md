# Video proxy editing and XML conform

Version: **0.1 proposal**. Date: **16 September 2026**.

## 1. Purpose and workflow

Edit Kinect interviews in **DaVinci Resolve**, then reproduce the selected cuts in a new Kinect depth/audio recording. The video is an editorial proxy; the final result retains depth geometry and can still be rendered from a different viewpoint or published through the web player.

1. Render each source recording as a video with a fixed camera, synchronized audio, and source timecode.
2. Import those videos into DaVinci Resolve and assemble the interview using ordinary cuts.
3. Export the selected timeline as a supported XML edit list.
4. Validate and conform that XML against the original recordings and their proxy mappings.
5. Replay the self-contained edited recording, render a review video, or export it for the web.

The required deliverable is a **command-line tool**. There is no requirement to implement a video editing timeline inside the Dear ImGui application.

## 2. Interchange format and supported edits

“XML EDL” is used here as shorthand for an XML document describing editorial decisions. The initial adapter is **Final Cut Pro 7 XML, `xmeml` version 5**, exported by **DaVinci Resolve**. Modern **FCPXML** is a different dialect and requires its own adapter. A traditional CMX 3600 EDL is another format; it is not the initial XML interface. Qualify the actual installed Resolve version, including Free/Studio edition, in the round-trip test report.

Apple's [xmeml structure documentation](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Basics/Basics.html) and [current FCPXML reference](https://developer.apple.com/documentation/professional-video-applications/fcpxml-reference) describe the distinct XML families. Adobe documents [Final Cut Pro XML export](https://helpx.adobe.com/premiere/desktop/render-and-export/export-files/export-a-project-as-a-final-cut-pro-xml-file.html). These support the format choice; compatibility with a particular application/version must still be tested with actual exports.

| Operation | Initial behavior |
| --- | --- |
| Trim a clip, remove a passage, reorder clips | Supported. |
| Repeat a source interval or combine compatible takes | Supported; each occurrence gets distinct output provenance. |
| Linked source audio at normal speed | Supported, including one or two channels. |
| Empty timeline intervals | Preserve duration as empty geometry and silence. |
| Multiple sequences in one XML | Require an explicit sequence identifier. |
| Multiple active video layers, multicam, nested sequences | Reject; flatten to the supported single-track form in the editor first. |
| Dissolves, other transitions, speed changes, reverse, freeze-frame effects | Reject with clip/operation diagnostics. Repetition of an ordinary interval is supported, but retiming is not. |
| J/L cuts, detached audio, music, mixing, gain automation | Reject in the first version; audio must use the same source intervals as depth. |
| Non-default crop, motion, titles, or color effects in the editor | Reject unsupported visible effects; the proxy camera and appearance are configured before editing. |
| Recognized identity effects, labels, markers, organizational metadata | Accept identity values; retain useful metadata or report non-rendering fields as informational. |

Never silently discard an operation that changes the intended picture, sound, or timing. This subset must be documented as an editor export preset and checked before producing output.

### DaVinci Resolve handoff

1. Set the project/timeline frame rate to the proxy rate before assembling clips; do not reinterpret clip frame rates. Keep the project audio at 48 kHz.
2. Import the generated MOV files as the source media. Their sidecars remain alongside them for the conform tool; they do not need to be imported into Resolve. These are editorial stand-ins for depth recordings, not a requirement to use Resolve's automatic proxy-generation feature.
3. Assemble one visible video track with its linked microphone audio. Keep effects, retiming, independent audio edits, and transitions outside the initial workflow.
4. Export the selected timeline as **Final Cut Pro 7 XML** while retaining references to the original generated MOV files. Use the timeline interchange export, not a flattened rendered movie or an interchange package relinked to newly rendered clips. Record the exact export option/menu labels during installed-version qualification.
5. Run `conform --dry-run`, resolve any reported incompatibilities, then run conform and review its resulting recording/video.

Blackmagic's [Resolve 20 Colorist Guide](https://documents.blackmagicdesign.com/UserManuals/DaVinci-Resolve-20-Colorist-Guide.pdf) documents XML interchange workflows. The implementation must include a real Resolve-exported xmeml fixture; a hand-written XML example is insufficient to certify this handoff.

## 3. Proposed command-line interface

These commands specify the desired interface; they are not implemented yet. Paths are illustrative Windows paths.

```text
recording_tool render-proxy --input "takes\take-a" --output "proxies\take-a.mov" --camera "presets\interview-camera.json" --fps 30/1 --size 1920x1080 --timecode 01:00:00:00 --burn-in --profile edit-prores

recording_tool conform --xml "edit\interview.xml" --format fcp7-xml --sequence-id "sequence-1" --proxy-root "proxies" --source-root "takes" --output "edits\interview-final" --dry-run

recording_tool conform --xml "edit\interview.xml" --format fcp7-xml --sequence-id "sequence-1" --proxy-root "proxies" --source-root "takes" --output "edits\interview-final"

recording_tool render-proxy --input "edits\interview-final" --output "review\interview-final.mov" --camera "presets\interview-camera.json" --fps 30/1 --timecode 01:00:00:00 --profile edit-prores
```

`render-proxy` automatically creates its sidecar and frame map beside the video. `conform --dry-run` emits a human-readable summary and a machine-readable normalized edit plan without writing an edited recording. Running without `--dry-run` performs the same validation and then writes the output; a separate approval interaction is not required.

Support a relink file for moved source/proxy paths, JSON progress/report output, deterministic exit codes for unsupported XML, missing media, invalid timing, and I/O failure, and cancellation that leaves an identifiable incomplete output. Do not overwrite a master or an existing successful output by default.

Rendering may run faster or slower than real time using a hidden GLFW/OpenGL context. It shares the native renderer and needs no Kinect connection. Conform copies/repackages data and requires neither the sensor nor a graphics context.

## 4. Proxy rendering contract

### Fixed camera and appearance

The camera preset records position, orientation, projection type, vertical field of view or orthographic scale, clipping planes, target size, and background. It also identifies the crop/mask and point-cloud appearance recipe. Provide a documented default frontal preset and allow arbitrary fixed viewpoints; do not automatically reframe every frame.

Save the preset and a hash of it with the proxy. The default editing image is 1920 × 1080; this is rendering resolution, not additional depth detail. The fixed camera affects only the video picture. Retain source-space depth in the master and conformed recording.

### Video, audio, and timecode

Use constant frame rate (CFR), default **30/1 fps**, with **24/1 and 25/1** as additional initial edit rates. Use the same rate for all proxies and the editor sequence. The parser must understand rate declarations as rationals and reject unsupported/mismatched rates explicitly; it must never round 30000/1001 to 30. Fractional rates and drop-frame timecode are deferred in the first implementation.

The recommended editing preset is MOV with an intraframe ProRes Proxy picture and 48 kHz, 24-bit PCM audio, encoded by the pinned FFmpeg build. Blackmagic lists ProRes MOV decoding for Windows in the [Resolve 20 codec matrix](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_20_Supported_Codec_List.pdf); still validate the generated files in the installed version. A compact H.264/AAC preset may follow after encoder-delay and frame-seek qualification; it is not the reference timing path. FFmpeg documents MOV timecode-track support in its [MOV muxer options](https://ffmpeg.org/ffmpeg-formats.html#MOV_002fMPEG_002d4_002fISOMBFF-muxers).

Store a proper source timecode track/metadata, defaulting to non-drop `01:00:00:00`. Optionally burn in source timecode, a short take ID, and proxy frame index. Burn-in helps human review; the importer reads XML and the sidecar, not text from pixels. Sequence timecode is a separate label; a sequence starting at one hour still maps its first output frame to media time zero.

Prepare float32 audio from the same frozen timing solution as the depth render, including known silence gaps and any required sample-rate conversion to 48 kHz. Do not use microphone callback arrival times or refit the clocks on each export. Proxy PCM and conform PCM must have identical sample positions, counts, and channel order. Declare the proxy's 24-bit amplitude conversion; compare it against the correspondingly converted reference. Conform retains prepared master precision and does not take its audio from the proxy. Preserve the conversion/resampling recipe and verify its reference checksums when regenerating prepared audio.

For duration `D` and rate `R`, generate `ceil(D * R)` proxy frames and matching audio duration. Mark any final fractional-frame padding explicitly in the sidecar: silence after the last real audio sample, and a declared held/empty depth state. Padding remains distinguishable from captured content. Encoding must not introduce an extra leading frame or silently shorten a stream to the other stream's duration.

### Sidecar and source identity

Produce `take-a.proxy.json` and `take-a.frames.bin` alongside `take-a.mov`. Their versioned content must include:

- Proxy UUID, master UUID, media hash, unique filename/reel label, and source manifest/calibration hashes.
- Frozen timing-solution ID and hash, source media origin, audio preparation recipe, and source PCM checksums.
- Rational edit rate, frame count, audio sample rate/layout, timecode base and format, duration, and padding regions.
- Camera/appearance recipe and hash, renderer/encoder versions, and frame-selection policy.
- For every proxy frame: source depth-record identity or explicit missing/held status, source media time, and any repeated/skipped depth relationship.

Select depth from the corrected audio-led media timeline using the same closest-frame and gap policy as playback. Preserve this exact decision in the frame map. A sensor timestamp is not assumed to equal `frame_index / 30`.

Embed a unique source identity in available media metadata and filenames, but do not assume every editor preserves custom metadata. Resolve XML media references through the sidecars and an explicit relink map when needed. Filename and timecode alone are insufficient: two takes may share a timecode start, and multiple renders of one take may use different timing solutions. Reject missing, ambiguous, or modified mappings.

## 5. XML normalization and boundary rules

Resolve XML file IDs/references, source paths, rates, and sequence selection before reading clips. In xmeml, source `in`/`out` and destination `start`/`end` have different coordinate origins; source media and sequence timecode must not be added twice. Apple documents these relations in [Timing Values](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Topics/Topics.html#//apple_ref/doc/uid/TP30001149-CH294-SW1).

Normalize each supported clip to:

```text
source_proxy_id
source_in_frame, source_out_frame
destination_in_frame, destination_out_frame
edit_rate_numerator, edit_rate_denominator
audio_channel_map
```

Internal intervals are **half-open**: `[in, out)`. Thus frames 300 through 449 are expressed as `[300, 450)` and contain 150 frames. Validate the adapter's XML boundary conversion with one-frame clips, adjacent clips, nonzero source timecode, and nonzero sequence timecode. For normal-speed clips at the same rate, require `source_out - source_in = destination_out - destination_in`.

Sort by destination position, retain gaps, and reject unintended overlaps or out-of-bounds source ranges. Interpret enabled/disabled clips consistently. Do not infer source duration from the sequence duration or treat transition sentinels such as `-1` as ordinary frame numbers. Rate-conforming, subclips with unresolved offsets, and unsupported nesting must produce explicit errors.

Use an XML parser with external entity resolution/network retrieval disabled and bounded input size/depth. Common exported declarations such as `<!DOCTYPE xmeml>` must not require fetching a DTD. Validate editorial semantics after parsing; well-formed XML alone does not establish a usable edit list.

## 6. Building the edited depth/audio recording

### Timeline and depth payloads

The initial conform mode **materializes** a new self-contained recording. A virtual playlist may be added later, but cannot replace this deliverable.

For a clip with source start `I`, destination start `S`, and length `L`, output frame `S + j` references proxy frame `I + j`, for `0 <= j < L`. Copy that frame's mapped original depth raster into the output. Preserve spatial precision and calibration; do not derive depth from rendered pixels. Recompression may be lossless, but geometric quantization or foreground removal is not implicit in conform.

Set each output frame's PTS from its absolute destination index:

```text
T_output(frame) = frame * rate_denominator / rate_numerator
PTS_us(frame)   = round(T_output(frame) * 1000000)
```

Never accumulate a rounded microsecond frame duration. Output frame `j` is displayed over `[j / R, (j + 1) / R)`, using the declared editorial rate to resolve boundaries; do not use closest-frame midpoint selection for this derived timeline. Otherwise a cut could become visible half a frame too early. The derived recording explicitly records any source depth repeats/skips introduced by CFR conversion. This prioritizes matching the montage seen in the editor; the untouched masters retain native sensor cadence.

Across cuts, original native counters may move backward or repeat. Preserve them as provenance only. Use new output record IDs and the resolved edit timeline for playback, without fitting a new Kinect clock. Cut markers reset temporal filters and prevent motion interpolation or held geometry from the preceding shot leaking into the next.

Version 1 accepts multiple source takes only when depth dimensions, sample representation, and calibration hash are compatible. Audio is prepared to one project rate/layout before proxy generation. Reject incompatible geometry or channel layouts with a clear report; handling varying calibrations/transforms per shot is a later extension.

### Audio cuts

For the initial integer edit rates, 48 kHz divides exactly into 2,000 samples/frame at 24 fps, 1,920 at 25 fps, and 1,600 at 30 fps. Map source and destination frame boundaries directly to sample-frame boundaries, and apply the same half-open interval convention to all channels. At 30 fps, `[300, 450)` selects audio sample frames `[480000, 720000)`.

Copy the corresponding prepared master PCM interval to the destination. Preserve timeline gaps as silence, preserve repeated/reordered intervals, and do not add an automatic crossfade or time stretch. Each clip's output sample count must exactly match its destination interval. A future fractional-rate implementation must use rational arithmetic and absolute sample boundaries with a documented rounding policy, not independently rounded clip durations.

### Output structure and validation

The result uses the normal recording package plus `kind = conformed`, its edit rate, `edit-manifest.json`, `source-provenance.json`, and an import/conform report. Include all selected depth/audio data, calibration, indexes, the normalized plan, source hashes, and the frozen timing references needed to explain the result. The report retains the imported XML hash and selected sequence ID. The input masters are unchanged.

Copy proxy camera/appearance recipes into the provenance for reproducible review. The final viewing camera can be changed independently. For an exact visual cut review, use matching appearance and camera settings; proxy burn-in remains an optional overlay and is never baked into depth data.

Create output under an incomplete name, validate it, then finalize it. Normal archive validation must check record integrity, frame/sample counts, PTS order, cut markers, and expected duration. An interrupted conform can be rerun from the immutable sources. The edited recording must play and export for the web after the source folders and proxies have been moved away.

## 7. Worked cut example

At 30 fps, assemble:

| Event | Source interval | Destination interval | Meaning |
| --- | --- | --- | --- |
| A | Take A frames `[300, 450)` | `[0, 150)` | Five seconds from source 10–15 s. |
| B | Take B frames `[900, 990)` | `[150, 240)` | Three seconds from source 30–33 s. |
| Gap | None | `[240, 270)` | One second of empty geometry and silence. |
| C | Take A frames `[300, 330)` | `[270, 300)` | Repeat the first selected second. |

The result has **300 depth presentation slots, 10 seconds of media, and 480,000 audio sample frames per channel**. The slot at output frame 150 is already from Take B. No interpolation, source clock reset, or proxy timecode offset inserts an extra frame at that cut.

## 8. Acceptance tests

| ID | Fixture | Required result |
| --- | --- | --- |
| EDIT-01 | Full uncut source through proxy/XML/conform | Output reproduces the proxy frame map and prepared PCM timeline, with padding explicitly retained. |
| EDIT-02 | Single-frame cut, adjacent cuts, nonzero timecodes | Exact normalized intervals; no off-by-one frame or double-added timecode offset. |
| EDIT-03 | The example above | Exactly 300 slots and 480,000 audio sample frames; correct source identities at all boundaries. |
| EDIT-04 | Thousands of short cuts at each supported rate | Exact final frame/sample counts and no accumulated timing drift. |
| EDIT-05 | Repeated intervals, timeline gaps, source dropped frames | Correct repetition, preserved gap duration, and distinguishable recorded loss versus editorial empty space. |
| EDIT-06 | Renamed/moved media, duplicate names, changed timing solution | Explicit relink succeeds for matching identities; ambiguity and stale mappings fail before conform. |
| EDIT-07 | Transition, detached audio, retiming, unsupported XML dialect | Nonzero exit with sequence/clip-specific explanation; no apparently successful partial montage. |
| EDIT-08 | Actual DaVinci Resolve timeline export | XML fixture from the qualified version/edition normalizes correctly and review rendering matches the chosen edits and sound. |
| EDIT-09 | Remove access to sources after successful conform | Output remains playable, seekable, and exportable to the browser without source recovery. |

Qualify at least one actual DaVinci Resolve round trip before releasing this workflow. Verify the output using source-frame identities and PCM comparisons, then review the rendered cut boundaries and sound; a matching total duration alone is insufficient.
