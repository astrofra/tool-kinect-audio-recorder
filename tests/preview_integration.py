"""Decode RGB review movies: palette anchors, gaps, segment rotation and failures."""
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

tool = str(pathlib.Path(sys.argv[1]).resolve())
ffmpeg = os.environ.get("RECORDER_TEST_FFMPEG") or shutil.which("ffmpeg")
if not ffmpeg:
    sys.exit(77)


def run(*args, ok=True):
    result = subprocess.run([tool, *map(str, args)], capture_output=True, timeout=60)
    assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
    return result


def decode(path):
    return subprocess.run([ffmpeg, "-v", "error", "-i", str(path), "-map", "0:v:0",
                           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
                          capture_output=True, check=True, timeout=60).stdout


with tempfile.TemporaryDirectory(prefix="rgb-preview-") as temp:
    root = pathlib.Path(temp)
    take = root / "prise é & native"
    (take / "depth").mkdir(parents=True)
    (take / "timing").mkdir()
    manifest = {"state": "complete", "host_clock_frequency": 10_000_000,
                "depth": {"source": "kinect-sdk-2.0", "frames": 3}}
    (take / "manifest.json").write_text(json.dumps(manifest))
    n = 512 * 424
    # Known palette anchors, including invalid zero and out-of-range clamping.
    values = (0, 500, 3250, 6000, 65535)
    colors = (b"\0\0\0", b"\0\0\xff", b"\x7f\xff\x7f", b"\xff\0\0", b"\xff\0\0")
    native_images = [b"".join(struct.pack("<H", values[(p + f) % 5]) for p in range(n)) for f in range(3)]
    rgb_images = [b"".join(colors[(p + f) % 5] for p in range(n)) for f in range(3)]
    journal = []
    for segment, first, count in [(0, 0, 2), (1, 2, 1)]:
        path = "depth/%06d.kd16" % segment
        header = struct.pack("<8s6I2Q2IQ", b"KD16RAW\0", 1, 64, 512, 424, 30, 1, first, count, 48000, 1, 0)
        (take / path).write_bytes(header + b"".join(native_images[first:first + count]))
        for local in range(count):
            index = first + local
            journal.append({"frame": index, "file": path, "file_frame": local,
                            "byte_offset": str(64 + local * n * 2),
                            "receipt_ticks": str(900_000_000 + [0, 333333, 10_000_000][index]),
                            # A sensor-clock reset must not collapse the host timeline.
                            "relative_time_100ns": str([5_000_000, 5_333_333, 0][index]),
                            "timestamp_epoch": str(index // 2)})
    journal_path = take / "timing/depth-frames.jsonl"
    original_journal = "".join(json.dumps(row) + "\n" for row in journal)
    journal_path.write_text(original_journal)
    output = take / "video/preview-rgb.mkv"
    run("export-preview", "--input", take, "--ffmpeg", ffmpeg)
    decoded = decode(output)
    assert decoded == rgb_images[0] + rgb_images[1] * 29 + rgb_images[2], "Palette, orientation or gap timing changed"
    assert not pathlib.Path(str(output) + ".part").exists()
    before = output.read_bytes()
    run("export-preview", "--input", take, "--ffmpeg", ffmpeg, ok=False)
    assert output.read_bytes() == before, "Existing preview was replaced"

    def invalid(label):
        target = root / (label + ".mkv")
        run("export-preview", "--input", take, "--output", target, "--ffmpeg", ffmpeg, ok=False)
        assert not target.exists() and not pathlib.Path(str(target) + ".part").exists()

    manifest["state"] = "recording"
    (take / "manifest.json").write_text(json.dumps(manifest))
    invalid("active")
    manifest["state"] = "complete"
    (take / "manifest.json").write_text(json.dumps(manifest))
    journal_path.write_text(original_journal[:-10])
    invalid("truncated-journal")
    journal[2]["receipt_ticks"] = "0"
    journal_path.write_text("".join(json.dumps(row) + "\n" for row in journal))
    invalid("backwards-clock")
    journal_path.write_text(original_journal.replace("depth/000001.kd16", "../other.kd16"))
    invalid("outside-take")
    journal_path.write_text(original_journal)
    last = take / "depth/000001.kd16"
    original_segment = last.read_bytes()
    last.write_bytes(original_segment[:-1])
    invalid("truncated-image")
    last.write_bytes(original_segment[:52] + b"\0\0\0\0" + original_segment[56:])
    invalid("unfinished-segment")
    last.write_bytes(original_segment)
    run("export-preview", "--input", take, "--output", root / "missing-encoder.mkv",
        "--ffmpeg", root / "no-such-encoder", ok=False)

    # This exercises automatic queueing after capture finalization and combines segments.
    simulated = root / "simulated"
    run("record", "--output", simulated, "--depth", "gradient", "--duration", 1.05,
        "--segment-seconds", 1, "--fast", "--encode-preview", "--ffmpeg", ffmpeg)
    preview = simulated / "video/preview-rgb.mkv"
    assert len(decode(preview)) == 32 * n * 3
    assert json.loads(pathlib.Path(str(preview) + ".json").read_text())["state"] == "complete"
    assert json.loads((simulated / "manifest.json").read_text())["rgb_preview"]["enabled"]
    assert len(list((simulated / "depth").glob("*.kd16"))) == 2
    assert not list(simulated.rglob("*.part"))
    print("RGB palette, native gaps/clock reset, multi-segment review, automatic export and invalid inputs passed.")
