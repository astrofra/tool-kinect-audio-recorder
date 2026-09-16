# Feasibility and technology choices

Research date: **16 September 2026**.

## 1. Conclusion and limits

This project is feasible as a native C++ application with a separate browser player. The critical unknowns are Windows driver compatibility, sustained depth decoding, and the accuracy of the relationship between sensor time and microphone time. None requires adopting Python or C# by default.

One Kinect records visible surfaces from one viewpoint. The result is an animated **2.5D surface**, suitable for a ghostlike silhouette and modest camera movement. It cannot supply the subject's hidden back or a complete walk-around reconstruction. Treat this as an artistic constraint; do not promise full volumetric capture.

Recommendations below are engineering judgments based on the cited primary sources. Performance numbers are calculations or proposed targets, not measurements of the connected hardware.

## 2. Open-source SDK assessment in 2026

| Candidate | Relevance | Decision |
| --- | --- | --- |
| OpenKinect **libfreenect2** | Native C++ access to Kinect v2 depth, IR, RGB, and calibration/registration. | Preferred open-source capture backend. |
| OpenKinect **libfreenect** | Targets the earlier Kinect generation. | Wrong device generation. |
| OpenNI2 with a libfreenect2 adapter | An additional API layer over the same Kinect v2 backend. | No useful benefit for this single-device recorder. |
| Azure Kinect Sensor SDK | Designed for Azure Kinect hardware. | Not a replacement driver for Kinect v2. |
| Python bindings, e.g. [pylibfreenect2](https://github.com/r9y9/pylibfreenect2) | Another application interface to a native backend. | Do not resolve USB driver or timing problems; unnecessary here. |
| Microsoft Kinect for Windows SDK 2.0 | Proprietary legacy SDK with a native C++ API. | Optional Windows fallback if the open-source backend fails qualification. |

Device scope and available interfaces are documented by [OpenKinect/libfreenect2](https://github.com/OpenKinect/libfreenect2), [OpenKinect/libfreenect](https://github.com/OpenKinect/libfreenect), [Azure Kinect Sensor SDK](https://github.com/microsoft/Azure-Kinect-Sensor-SDK), and Microsoft's [IDepthFrame C++ reference](https://learn.microsoft.com/en-us/previous-versions/windows/kinect/dn772983%28v%3Dieb.10%29). The OpenNI2 adapter is included in the libfreenect2 project.

### Maintenance evidence

At the research date, GitHub reports libfreenect2's latest release as **v0.2.1, published 6 August 2021**. Its default-branch head is `fd64c5d9b214df6f6a55b4419357e51083f15d93`, with a committer date of **1 March 2020**; the release points to that same commit. Release publication and source modification dates are different. The repository is not archived, but this is a legacy dependency, not evidence of active 2026 maintenance. See the [release](https://github.com/OpenKinect/libfreenect2/releases/tag/v0.2.1), [release metadata](https://api.github.com/repos/OpenKinect/libfreenect2/releases/latest), and [pinned commit](https://github.com/OpenKinect/libfreenect2/commit/fd64c5d9b214df6f6a55b4419357e51083f15d93).

Pin source and build dependencies, keep local compatibility patches small, and record their hashes. Assess any fork by its specific fixes, reproducible build, and long capture test. A newer fork timestamp alone is insufficient reason to adopt it.

### Windows feasibility gate

The upstream Windows procedure offers **UsbDk** or **libusbK**, with different driver integration behavior; it explicitly says not to install both. Its operating-system discussion predates Windows 11. Therefore, Windows 11 support on this PC remains unverified. Do not make automatic driver replacement part of the recorder. Provide a separate, documented setup and rollback procedure after validating one backend. See the [upstream Windows instructions](https://github.com/OpenKinect/libfreenect2#windows--visual-studio).

The first implementation stage must inventory the sensor/adapter, existing driver, exact Windows build, USB controller, GPU, and audio endpoint. Test on a direct USB 3 connection with the appropriate Kinect power adapter. Preserve an existing working configuration when comparing drivers; validate the microphone concurrently if it also uses USB.

Microsoft still provides [SDK 2.0, version 2.0.1410.19000](https://www.microsoft.com/en-us/download/details.aspx?id=44561). The page's 2024 publication date does not establish a new SDK generation or Windows 11 certification. Its listed supported systems are older Windows versions. A fallback adapter can use native `IDepthFrame`; **C# is not required**, but open-source-only dependency requirements would exclude this fallback.

### Features to implement independently

The intended product needs external sound-card audio and a depth silhouette. Its design does not depend on Kinect microphone-array access or Microsoft body tracking. Implement segmentation from depth cropping and an optional empty-background reference; do not assume that libfreenect2 provides a turnkey person mask.

## 3. Language, build, and dependency strategy

Use **C++20**, the Windows SDK, and CMake presets. C++ provides a direct interface to the sensor library, WASAPI, explicit buffer ownership, and GPU rendering. C could implement much of the application but would still need a bridge to the C++ sensor API. Python can remain an optional analysis convenience, outside capture and distribution. There is no demonstrated technical need for a managed application runtime.

Use an MSVC x64 toolchain as the initial candidate, with one consistent runtime configuration across native libraries. Choose and pin an actually tested compiler and CMake version during the hardware spike; the project should not promise compatibility with every recent toolchain.

There is an immediate build issue to check: upstream libfreenect2 specifies CMake 2.8-era compatibility, while CMake 4 removed compatibility below 3.5. Use an audited policy compatibility patch/override or a pinned compatible build tool for this dependency, then validate the result. See [libfreenect2's build file](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/CMakeLists.txt) and [CMake's compatibility rules](https://cmake.org/cmake/help/latest/command/cmake_minimum_required.html).

| Component | Proposed dependency | Rationale |
| --- | --- | --- |
| Sensor access | libfreenect2, libusb, qualified Windows USB driver | Necessary hardware interface. |
| Depth processing | CPU baseline; evaluate OpenCL and OpenGL if needed | Qualify sustained throughput before selecting the production backend. |
| Sensor build dependency | libjpeg-turbo | Upstream Windows build requires it even if RGB recording is disabled. |
| Audio capture/playback | Native WASAPI | Exposes positions and timing without adding a cross-platform audio framework. |
| GUI and windowing | Dear ImGui and GLFW | Small native interface using the requested libraries. |
| Preview | OpenGL 3.3 core, plus a small GL loader | Render depth directly on the GPU and display it inside the ImGui interface. |
| Recording compression | Zstandard | One reusable codec for native depth storage and a browser WASM decoder. |
| Metadata | One small, pinned JSON implementation | Avoid hand-written general-purpose JSON parsing. |
| Edit-list parsing | One small, pinned native XML parser | Implement the qualified XML subset in C++; no Python runtime required. |
| Proxy/video and audio encoding | Pinned FFmpeg executable for export commands | Encodes timecoded video proxies and delivery audio outside the live recording path. |
| Web player | JavaScript, WebGL 2, a worker, Zstandard decoder WASM | No application server or large front-end framework required. |

The upstream build also exposes switches for examples, OpenNI2, CUDA, OpenCL, and OpenGL. Disable unused targets explicitly. Its OpenGL processing backend also uses GLFW; build against a consistent GLFW version and coordinate its lifecycle with the application. These are build observations, not a claim that a chosen configuration has compiled successfully. [Upstream CMake configuration](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/CMakeLists.txt).

[Zstandard](https://github.com/facebook/zstd) is the proposed lossless byte compressor. Its effective ratio on these depth recordings must be measured. [FFmpeg](https://ffmpeg.org/ffmpeg.html) is an export tool, not a mandatory recorder service. Keep dependency revisions and distribution notices with each release; the repository currently contains a GPLv3 license text.

### Dear ImGui and GLFW

Use Dear ImGui's supplied `imgui_impl_glfw` platform backend and `imgui_impl_opengl3` renderer backend. GLFW owns the native window, input, and OpenGL context; Dear ImGui supplies the controls and library panels. These integrations are maintained in the [official backend collection](https://github.com/ocornut/imgui/blob/master/docs/BACKENDS.md) and illustrated by the [GLFW/OpenGL example](https://github.com/ocornut/imgui/blob/master/examples/example_glfw_opengl3/main.cpp).

Render the point cloud to an offscreen framebuffer and show its texture in an ImGui panel. Keep a single OS window initially; docking and multiple native viewports are optional later features. Use Windows native file/folder dialogs through a small platform adapter instead of introducing another GUI toolkit. Provide DPI-aware text and controls, keyboard navigation, and explicit save/recording status.

If libfreenect2's OpenGL pipeline is selected, its GLFW initialization, hidden processing contexts, callbacks, and shutdown must coexist with the UI. Do not assume sensor and preview contexts share objects. Restore the UI context before rendering; stop sensor processing before global GLFW shutdown. GLFW has explicit [main-thread restrictions](https://www.glfw.org/docs/latest/intro_guide.html#thread_safety). The sensor backend also changes global window hints, so explicitly set the UI's visibility, profile, and version before creating its window. See the [processing context implementation](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/src/opengl_depth_packet_processor.cpp). Qualify this integration during Stage A. CPU or OpenCL processing remains an option if it simplifies context ownership and meets throughput targets.

Compile the pinned Dear ImGui sources and the two selected backends as a CMake target. Package GLFW, the required native DLLs, a font asset, and the application's shaders. The browser viewer uses ordinary web controls; it does not need a port of the desktop authoring UI.

## 4. Data volume and rendering feasibility

Budget for 512 × 424 depth pixels at a nominal 30 frames/s, or **217,088 possible points per frame**. The library exposes decoded depth as 32-bit floating-point millimetres. [Frame definition](https://github.com/OpenKinect/libfreenect2/blob/fd64c5d9b214df6f6a55b4419357e51083f15d93/include/libfreenect2/frame_listener.hpp).

The following calculations use decimal MB and GB and exclude compression, headers, indexes, masks, and filesystem overhead. They describe decoded recording data, not USB wire traffic.

| Representation | Bytes/frame | MB/s at 30 Hz | GB/hour |
| --- | ---: | ---: | ---: |
| Depth, unsigned 16-bit millimetres | 434,176 | 13.025 | 46.891 |
| Depth, float32 millimetres | 868,352 | 26.051 | 93.782 |
| XYZ, three float32 values per point | 2,605,056 | 78.152 | 281.346 |
| Audio, 48 kHz mono float32 PCM | N/A | 0.192 | 0.691 |
| Audio, 48 kHz stereo float32 PCM | N/A | 0.384 | 1.382 |

The normal archive should retain the depth raster and calibration, then reconstruct points during playback. Expanding to XYZ before storage multiplies volume without adding captured information. A two-hour uint16-depth/mono-audio take is about **95.2 GB uncompressed**; reserve approximately **120 GB** including 25% headroom. The optional float32-depth profile needs approximately **240 GB** with the same headroom. Recalculate for the selected duration and channels; do not rely on an assumed compression ratio to permit a take.

Quantizing float depth to integer millimetres introduces up to 0.5 mm rounding error for valid in-range values. Zstandard preserves those integer samples exactly, but that profile is **not lossless relative to the original float stream**. Offer float32 preservation for archival or analysis needs.

Full-resolution uint16 depth is approximately **104.2 Mbit/s before compression**. Halving both dimensions to 256 × 212 at 30 Hz gives 26.1 Mbit/s before compression. Web delivery therefore needs cropping, compression, and measured quality profiles. A 5–15 Mbit/s combined download budget is a provisional product target, not a predicted Zstandard ratio. If it cannot be met, reduce spatial density first, then evaluate temporal prediction or a dedicated geometry codec.

An organized depth image permits GPU reconstruction with one grid point per valid pixel. This makes an initial WebGL implementation plausible, but GPU upload, transparency overdraw, decoder cost, and memory use still need testing. Avoid one JavaScript object per point.

## 5. Synchronization feasibility

Starting both devices together is insufficient: their clocks can differ in rate as well as start time. A hypothetical 50 ppm rate difference creates **180 ms of drift in one hour**. A clap at the beginning corrects one offset; it does not measure the subsequent rate difference.

The proposed solution is software synchronization with preserved evidence: timestamp each stream, relate it to Windows QPC, estimate a rate and offset, and calibrate capture latency using an event visible in depth and audible in the microphone. See the [detailed timing design](synchronization-and-format.md).

This does not establish hardware genlock or sample-accurate exposure alignment. In particular, libfreenect2 does not expose a ready-made Kinect-to-QPC calibration in its frame API. A timestamp measured when a decoded frame arrives includes transport, processing, and scheduling latency. A robust estimator can reduce jitter and measure drift; it cannot infer an unknown fixed latency from arrivals alone. Physical validation is a release gate.

## 6. Risks and implementation stages

| Risk | Mitigation or decision gate |
| --- | --- |
| Legacy driver fails on the actual Windows installation | Prove capture first; compare the native Microsoft backend if acceptable. Do not proceed on an assumed compatibility claim. |
| Slow decoding or GPU contention | Benchmark CPU/OpenCL/OpenGL; prioritize capture over preview and keep queue limits explicit. |
| Variable capture latency biases clock estimates | Preserve raw observations, use a visible/audible test event, and reject unsupported accuracy claims. |
| Subject mask includes the room or removes moving limbs | Qualify a fixed set, keep full depth masters, and allow crop/background adjustments during export. |
| Disk stalls, full disk, or process crash | Append recoverable records, rotate files, checkpoint, and test fault recovery. |
| Browser throughput or bandwidth is insufficient | Export multiple profiles, use bounded buffering, and retain a rendered 2D video fallback. |
| XML dialect or editorial operation is unsupported | Qualify one editor/export profile first, freeze proxy mappings, and reject unrepresentable edits with explicit diagnostics. |
| Source ecosystem becomes unavailable | Pin and archive source, dependencies, calibration, format documentation, and known-good binary builds. |

Suggested sequence, assuming one experienced native developer and existing working hardware:

| Stage | Deliverable and exit gate | Indicative effort |
| --- | --- | --- |
| A: hardware and timing spike | Minimal C++ capture; USB/backend comparison; simultaneous audio; early browser depth sample; measured timestamp behavior. | 3–5 working days, excluding driver investigation. |
| B: recorder and archive | Two-hour recording, recovery, timing diagnostics, desktop replay, calibrated drift correction. | 1–2 weeks. |
| C: usable desktop tool | Dear ImGui controls, meters, point-cloud preview, library metadata, packaging on a clean PC. | 1–2 weeks. |
| D: external editing | Timecoded proxies, XML parser, materialized conform, and an end-to-end round trip through DaVinci Resolve. | 1–2 weeks for the specified cuts-only subset. |
| E: web publication | Exporter, static-hosted player, seeking/buffering tests, browser timing verification. | 1–2 weeks. |
| F: hardening | Fault injection, setup documentation, longer trials, visual and listening review. | 1–2 weeks. |

These are planning estimates, not commitments; driver failures or a new compression scheme can dominate the schedule. Proceed to a production implementation only when Stage A demonstrates stable concurrent capture and a credible path to the synchronization targets.

## 7. Feasibility of conventional video editing

The [proxy/XML workflow](editing-workflow.md) is feasible without changing capture hardware or language. Offline rendering removes the real-time encoding requirement, and conform can copy selected depth payloads and PCM according to an explicit edit plan. The main additional work is preserving source identity, translating XML timing correctly, and maintaining sample/frame correspondence through cuts.

DaVinci Resolve is the selected editing application. Use its Final Cut Pro 7 XML export as the first interchange target. Blackmagic documents FCP7/XML interchange in its manuals and Resolve 20 training material; this is distinct from modern FCPXML. The installed version and its actual XML output must be qualified with fixtures, rather than assuming any `.xml` file is compatible. See Blackmagic's [Resolve 20 Colorist Guide](https://documents.blackmagicdesign.com/UserManuals/DaVinci-Resolve-20-Colorist-Guide.pdf) and Apple's [xmeml version history](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/VersionsoftheInterchangeFormat/VersionsoftheInterchangeFormat.html).

The proposed baseline deliberately materializes a new depth/audio recording at the proxy's edit cadence. It preserves source spatial precision, while explicitly recording depth-frame repeats/skips introduced when mapping native timestamps to video frames. This makes the edited output reproduce the montage that was approved in the video editor. The proxy's fixed view does not constrain later rendering of the retained 3D data.
