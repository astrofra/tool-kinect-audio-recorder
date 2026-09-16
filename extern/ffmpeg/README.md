# Bundled FFmpeg for the Kinect Audio Recorder

This directory accompanies a **minimal Windows x64 FFmpeg 9.0.1 executable**,
built from the unmodified official source archive with Microsoft Visual C++.
The recorder invokes it as a separate process. It is not linked into the recorder.

The executable is **LGPL version 2.1 or later**. GPL, nonfree, external codec
libraries, hardware codecs and network protocols are not enabled. The FFmpeg
source tree includes components with other licenses; its complete license texts
and original notices are preserved. See `licenses/LICENSE.md` for the upstream
explanation and `licenses/COPYING.LGPLv2.1` for the applicable binary license.
The recorder itself retains its separate project license.

This package contains:

- `ffmpeg.exe`: local FFV1/rawvideo/float32 PCM encoding and decoding.
- `licenses/`: unmodified upstream license texts.
- `sources/ffmpeg-9.0.1.tar.xz`: the exact, unmodified FFmpeg source archive.
- `build.ps1`, `build.sh`, `sources.json`: build recipe, dependency URLs and pinned
  SHA-256 hashes. No source patches are applied.
- `version.txt`, `config.h`, `config_components.h`: compiler/version/configuration
  information for this executable.

The parent release's `SHA256SUMS.txt` covers all these files, including the source
archive and binary. Source provenance: <https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz>.
FFmpeg licensing guidance: <https://ffmpeg.org/legal.html>.

## Supported use

The build supports the recorder's depth exports and byte-exact validation:
FFV1, rawvideo and PCM float32 codecs; rawvideo, WAV and Matroska inputs; Matroska,
rawvideo, raw float32 and WAV outputs; local files and pipes. Basic format/scale
and audio conversion filters are included for FFmpeg's processing pipeline.
Assembly optimizations are disabled to avoid a NASM build dependency.

It is intentionally a limited FFmpeg build. It does not include ffplay, ffprobe,
H.264/H.265 encoders, GPU encoding or network access. Future rendered proxies/web
exports will require an explicitly updated build profile. The recorder's
`--ffmpeg PATH` option can select a different installation when needed.

No FFmpeg-specific DLLs, Git/MSYS runtime or compiler installation is needed to
run this executable. It uses Windows system DLLs and a statically linked MSVC
runtime. Keep the license/source/recipe files with it when redistributing this
package; licensing documentation alone does not replace the corresponding source.

## Rebuild

Requirements: Visual Studio 2022 C++ desktop tools and Windows SDK, Git for Windows
(including Bash), Windows PowerShell 5.1+, `curl.exe`, and `tar.exe` capable of
extracting xz/zstd archives. Local validation used MSVC 19.41.34120. The recorder
remains C++11; this third-party source uses the C standard selected by FFmpeg.

From this directory, choose **new** work and destination directories:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -WorkDirectory C:\temp\ffmpeg-work -Destination C:\temp\ffmpeg-package -CacheDirectory C:\temp\ffmpeg-downloads
```

The script downloads and verifies the pinned FFmpeg source and MSYS2 GNU Make
archives, extracts them into the work directory, and builds with MSVC using Git
Bash's build tools. GNU Make and the MSYS runtime are build tools only, not linked
into `ffmpeg.exe` or included in the release. `RECORDER_GIT_BASH` can point to a
nonstandard Git for Windows `bash.exe` installation.

For an offline rebuild, place the source archive from `sources/` and the pinned
GNU Make archive listed in `sources.json` in the cache directory first. The build
does not alter the source code. Generated configuration files can differ with the
installed compiler, SDK and build directory; byte-identical rebuilding is not
promised. The build log is saved in the work directory.
