"""Automatic encoding: closed pairs, overlap with capture, publication, PCM/depth integrity."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

exe = str(pathlib.Path(sys.argv[1]).resolve())
ffmpeg = os.environ.get("RECORDER_TEST_FFMPEG") or shutil.which("ffmpeg")
if not ffmpeg:
    print("FFmpeg unavailable; automatic encoding integration skipped.")
    sys.exit(77)


def decode(path, video):
    args = [ffmpeg, "-v", "error", "-i", str(path)]
    args += ["-map", "0:v:0", "-pix_fmt", "gray16le", "-f", "rawvideo", "-"] if video else [
        "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", "-"]
    return subprocess.run(args, check=True, capture_output=True, timeout=30).stdout


def command(take, encoder=ffmpeg):
    return [exe, "record", "--output", str(take), "--depth", "gradient", "--duration", "3.05",
            "--segment-seconds", "1", "--channels", "2", "--encode-depth", "--ffmpeg", str(encoder)]


with tempfile.TemporaryDirectory(prefix="encoding-tests-") as temp:
    root = pathlib.Path(temp)
    take = root / "prise \u00e9 & queued"
    process = subprocess.Popen(command(take), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    overlapped = False
    deadline = time.monotonic() + 30
    try:
        while process.poll() is None and time.monotonic() < deadline:
            if (take / "video/000000.mkv").exists():
                # The capture manifest is atomically replaced independently of the worker.
                manifest = json.loads((take / "manifest.json").read_text())
                overlapped |= manifest["state"] == "recording"
            time.sleep(0.02)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, (stdout, stderr)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert overlapped, "First segment should encode while later segments are captured"
    assert b"4 completed, 0 failed" in stdout, stdout
    manifest = json.loads((take / "manifest.json").read_text())
    assert manifest["state"] == "complete" and manifest["frames"] == 146400
    assert manifest["background_encoding"]["enabled"]
    for source in sorted((take / "depth").glob("*.kd16")):
        video = take / "video" / (source.stem + ".mkv")
        wav = take / "audio" / (source.stem + ".wav")
        assert decode(video, True) == source.read_bytes()[64:]
        assert decode(video, False) == wav.read_bytes()[58:]
        assert json.loads(pathlib.Path(str(video) + ".json").read_text())["state"] == "complete"
        assert pathlib.Path(str(video) + ".log").exists()
        assert not pathlib.Path(str(video) + ".part").exists()

    # Encoder startup failure is separate from capture status and does not prevent later jobs.
    failed = root / "missing encoder"
    result = subprocess.run(command(failed, root / "missing-ffmpeg") + ["--fast"], capture_output=True, timeout=30)
    assert result.returncode == 1 and b"0 completed, 4 failed" in result.stdout, result.stdout
    assert json.loads((failed / "manifest.json").read_text())["state"] == "complete"
    assert len(list((failed / "depth").glob("*.kd16"))) == 4
    assert not list((failed / "video").glob("*.mkv"))
    assert all(json.loads(p.read_text())["state"] == "failed" for p in (failed / "video").glob("*.json"))

    # Audio-only mode rejects an encoding request before creating the take.
    invalid = root / "invalid"
    result = subprocess.run([exe, "record", "--output", str(invalid), "--encode-depth"], capture_output=True, timeout=10)
    assert result.returncode != 0 and not invalid.exists()
    print("Background encoding overlaps capture; all closed segments round-trip exactly; failures preserve capture.")
