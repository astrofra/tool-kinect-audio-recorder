"""Independent archive, synchronization and optional FFV1 round-trip checks."""
import json
import math
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

exe = str(pathlib.Path(sys.argv[1]).resolve())
ffmpeg = os.environ.get("RECORDER_TEST_FFMPEG") or shutil.which("ffmpeg")
frame_bytes = 512 * 424 * 2


def run(*args, ok=True):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, timeout=60)
    assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
    return result


def decode(path, kind):
    args = [ffmpeg, "-v", "error", "-i", str(path)]
    if kind == "video":
        args += ["-map", "0:v:0", "-pix_fmt", "gray16le", "-f", "rawvideo", "-"]
    else:
        args += ["-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", "-"]
    return subprocess.run(args, check=True, capture_output=True, timeout=60).stdout


with tempfile.TemporaryDirectory(prefix="kinect-depth-tests-") as temp:
    root = pathlib.Path(temp)
    for rate, seconds, pattern in [(48000, 2.0234375, "gradient"), (11025, 1.017, "noise"), (44100, 0.001, "gradient")]:
        take = root / ("prise-é & " + str(rate))
        run("record", "--output", take, "--depth", pattern, "--sample-rate", rate,
            "--channels", 2, "--duration", seconds, "--segment-seconds", 1, "--fast")
        manifest = json.loads((take / "manifest.json").read_text())
        assert manifest["state"] == "complete"
        assert manifest["schema"] == "kinect-depth-audio-prototype/1"
        samples = math.floor(seconds * rate + 0.5)
        expected = (samples * 30 + rate - 1) // rate
        depth = manifest["depth"]
        assert depth["frames"] == expected and manifest["frames"] == samples
        assert depth["clock"] == "stored-audio-sample-clock" and depth["calibration"] is None
        cursor = 0
        for entry in depth["files"]:
            data = (take / entry["path"]).read_bytes()
            header = struct.unpack("<8s6I2Q2IQ", data[:64])
            assert header[:7] == (b"KD16SIM\0", 1, 64, 512, 424, 30, 1)
            count = min(30, expected - cursor)
            assert header[7:] == (cursor, count, rate, 1, cursor * rate // 30)
            assert entry["first_frame"] == cursor and entry["frames"] == count
            assert len(data) == 64 + count * frame_bytes
            for index in range(count):
                pixels = struct.unpack_from("<217088H", data, 64 + index * frame_bytes)
                assert pixels[0] == 0 and pixels[511] == 0 and pixels[-1] == 0
                assert 500 <= min(pixels[512 + 1:512 + 511]) <= max(pixels) <= 6000
                assert pixels[10 * 512 + 10] == (1000 if (cursor + index) % 30 < 3 else 5000)
            if ffmpeg:
                wav = take / "audio" / (pathlib.Path(entry["path"]).stem + ".wav")
                encoded = root / (str(rate) + " encoded & " + str(cursor) + ".mkv")
                run("export-depth", "--input", take / entry["path"], "--audio", wav,
                    "--output", encoded, "--ffmpeg", ffmpeg)
                assert decode(encoded, "video") == data[64:], "Every depth byte must survive FFV1"
                assert decode(encoded, "audio") == wav.read_bytes()[58:], "PCM samples must survive muxing"
                before = encoded.read_bytes()
                run("export-depth", "--input", take / entry["path"], "--output", encoded, "--ffmpeg", ffmpeg, ok=False)
                assert encoded.read_bytes() == before, "Existing exports must not be overwritten"
            cursor += count
        assert cursor == expected
        journal = [json.loads(line) for line in (take / "timing/depth-frames.jsonl").read_text().splitlines()]
        assert len(journal) == expected
        for index, entry in enumerate(journal):
            assert entry["frame"] == index and entry["file_frame"] == index % 30
            assert int(entry["pts_100ns"]) == index * 10000000 // 30
            assert int(entry["audio_sample_floor"]) == index * rate // 30
            assert int(entry["byte_offset"]) == 64 + (index % 30) * frame_bytes
        checkpoint = json.loads((take / "checkpoint.json").read_text())
        assert checkpoint["depth_frames"] == expected
        assert checkpoint["depth_timing_bytes"] == (take / "timing/depth-frames.jsonl").stat().st_size

    # Gradient samples at known coordinates and motion across two frames.
    gradient = (root / "prise-é & 48000/depth/000000.kd16").read_bytes()
    a = struct.unpack_from("<H", gradient, 64 + (100 * 512 + 100) * 2)[0]
    b = struct.unpack_from("<H", gradient, 64 + frame_bytes + (100 * 512 + 100) * 2)[0]
    assert a == 1636 and b == a + 35
    # Same depth recipe at another audio rate/pacing must produce identical pixels.
    repeated = root / "noise repeated"
    run("record", "--output", repeated, "--depth", "noise", "--duration", 0.05)
    original_noise = (root / "prise-é & 11025/depth/000000.kd16").read_bytes()
    assert (repeated / "depth/000000.kd16").read_bytes()[64:] == original_noise[64:64 + 2 * frame_bytes]
    invalid = root / "invalid"
    run("record", "--output", invalid, "--depth", "unknown", ok=False)
    run("record", "--output", invalid, "--depth", "kinect", "--fast", ok=False)
    run("record", "--output", invalid, "--depth", "kinect", "--encode-depth", ok=False)
    assert not invalid.exists()
    for label, content in [("short", b"KD16"), ("truncated", gradient[:-1]),
                           ("unfinished", gradient[:52] + b"\0\0\0\0" + gradient[56:])]:
        source = root / (label + ".kd16")
        source.write_bytes(content)
        run("export-depth", "--input", source, "--output", root / (label + ".mkv"), ok=False)
        assert not (root / (label + ".mkv")).exists()
    source = root / "prise-é & 48000/depth/000000.kd16"
    run("export-depth", "--input", source, "--output", root / "missing.mkv",
        "--ffmpeg", root / "no-such-ffmpeg", ok=False)
    run("export-depth", "--input", source, "--audio", root / "prise-é & 44100/audio/000000.wav",
        "--output", root / "wrong-audio.mkv", ok=False)

    if ffmpeg:
        # Exercise the entire uint16 range, independent of the simulator's distance range.
        fixture = root / "full-range.kd16"
        header = struct.pack("<8s6I2Q2IQ", b"KD16SIM\0", 1, 64, 512, 424, 30, 1, 0, 1, 48000, 1, 0)
        payload = b"".join(struct.pack("<H", i % 65536) for i in range(512 * 424))
        fixture.write_bytes(header + payload)
        output = root / "full-range.mkv"
        run("export-depth", "--input", fixture, "--output", output, "--ffmpeg", ffmpeg)
        assert decode(output, "video") == payload
        print("FFV1 + PCM round trips are byte-exact, including full uint16 range and Unicode paths.")
    else:
        print("FFmpeg not found: video round-trip checks skipped; native archive checks passed.")
    print("Depth rotation, frame boundaries, sample-clock mapping, patterns and invalid inputs passed.")
