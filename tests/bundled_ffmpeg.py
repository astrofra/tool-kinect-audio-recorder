"""Verify relocation, encoder precedence, and export without FFmpeg on PATH."""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

tool = pathlib.Path(sys.argv[1]).resolve()
encoder = os.environ.get("RECORDER_TEST_FFMPEG") or shutil.which("ffmpeg")
if not encoder:
    print("No FFmpeg available for the relocation test.")
    sys.exit(77)

with tempfile.TemporaryDirectory(prefix="recorder-bundle-") as temp:
    root = pathlib.Path(temp)
    package = root / "paquet é & moved"
    vendor = package / "extern/ffmpeg"
    vendor.mkdir(parents=True)
    copied_tool = package / tool.name
    copied_encoder = vendor / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
    shutil.copy2(tool, copied_tool)
    shutil.copy2(encoder, copied_encoder)
    cwd = root / "unrelated working directory"
    cwd.mkdir()
    env = dict(os.environ, PATH="")
    source = cwd / "depth.kd16"
    output = cwd / "depth.mkv"
    header = struct.pack("<8s6I2Q2IQ", b"KD16SIM\0", 1, 64, 512, 424, 30, 1, 0, 1, 48000, 1, 0)
    payload = b"".join(struct.pack("<H", i % 65536) for i in range(512 * 424))
    source.write_bytes(header + payload)
    result = subprocess.run([str(copied_tool), "export-depth", "--input", str(source), "--output", str(output)],
                            cwd=cwd, env=env, capture_output=True, timeout=60)
    assert result.returncode == 0, (result.stdout, result.stderr)
    decoded = subprocess.run([str(copied_encoder), "-v", "error", "-i", str(output), "-pix_fmt", "gray16le",
                              "-f", "rawvideo", "-"], cwd=cwd, env=env, capture_output=True, timeout=60)
    assert decoded.returncode == 0 and decoded.stdout == payload, decoded.stderr
    override = cwd / "override.mkv"
    result = subprocess.run([str(copied_tool), "export-depth", "--input", str(source), "--output", str(override),
                             "--ffmpeg", str(cwd / "does-not-exist")], cwd=cwd, env=env, capture_output=True, timeout=60)
    assert result.returncode != 0 and not override.exists(), "Explicit override must take priority over bundle"
    print("Moved package exports byte-exact depth with empty PATH; explicit encoder override takes priority.")
