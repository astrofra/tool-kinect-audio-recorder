# Kinect v2 interview recorder: design documentation

Research date: **16 September 2026**. Status: **proposed specification, before hardware validation**.

The application records a Kinect v2 depth stream and an external microphone on a Windows PC, then presents the interview as an animated, ghostlike point cloud with synchronized sound. Capture is native; the final publication target is an ordinary web browser. A command-line workflow renders timecoded video proxies for conventional editing and rebuilds a Kinect/audio recording from the exported XML edit list.

## Documents

1. [Feasibility and technology choices](feasibility.md): the 2026 SDK assessment, language and GUI choices, resource estimates, risks, and implementation stages.
2. [Product and architecture specification](specification.md): scope, operator workflow, capture and playback behavior, desktop architecture, web export, and acceptance criteria.
3. [Synchronization and recording format](synchronization-and-format.md): clock mapping, latency calibration, drift correction, archive structure, and browser playback timing.
4. [Video proxy editing and XML conform](editing-workflow.md): fixed-camera video/audio rendering, timecode and source mapping, XML interchange, and creation of a new edited Kinect/audio recording.

## Recommended direction

| Question | Recommendation |
| --- | --- |
| Which open-source Kinect v2 SDK? | Start with **libfreenect2**, pinned to an audited revision. Windows driver compatibility is the first feasibility gate. |
| Which language? | **C++20 and CMake** for capture, desktop playback, and export. Neither Python nor C# is required. |
| How should synchronization work? | Record device timestamps, audio sample positions, and their relationship to a shared Windows monotonic clock. Estimate drift, calibrate offset, and export explicit presentation timestamps. |
| Which desktop UI? | **Dear ImGui + GLFW + OpenGL**. Keep capture independent of the GUI. |
| How should the web version work? | Publish timestamped depth chunks and compressed audio; render with **WebGL 2**, following the audio playback timeline. |
| How should interviews be edited? | Edit timecoded video proxies in **DaVinci Resolve**, export Final Cut Pro 7 XML, then conform the original depth and audio with `recording_tool`. |

The overall conclusion is a **conditional go**: the architecture is practical, but reliable operation on the actual Kinect, USB controller, Windows installation, GPU, and audio interface must be demonstrated. No hardware capture, driver installation, benchmark, or synchronization measurement was performed for this study. Source references and the distinction between verified facts and proposed design choices appear in the documents.

## Working assumptions

- One Kinect v2 and one microphone endpoint, with one or two selected audio channels.
- A fixed indoor interview setup and a predominantly frontal subject.
- Up to two hours per take as an initial engineering target.
- Depth without RGB is sufficient for the intended appearance.
- Publication means recorded, on-demand playback; live broadcasting is outside the initial scope.
- Desktop browsers are the first web target. Mobile support follows measured performance.
- Editing uses DaVinci Resolve, initially with straight cuts, one visible video track, and linked source audio at normal speed. The first interchange dialect is Final Cut Pro 7 XML (`xmeml`); the installed Resolve version still needs qualification.

These are proposed defaults, not requirements already confirmed by the project owner. The design can accommodate changes without replacing the capture and timing model.
