"""Black-box file validation; Python is a test tool, not a recorder dependency."""
import json
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

exe = str(pathlib.Path(sys.argv[1]).resolve())


def invoke(*args, ok=True):
    p = subprocess.run([exe, *map(str, args)], capture_output=True, timeout=30)
    if ok:
        assert p.returncode == 0, (p.stdout, p.stderr)
    else:
        assert p.returncode != 0, p.stdout
    return p


def read_wav(path):
    b = path.read_bytes()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE"
    assert struct.unpack_from("<I", b, 4)[0] + 8 == len(b)
    chunks = {}
    pos = 12
    while pos < len(b):
        tag, size = struct.unpack_from("<4sI", b, pos)
        assert pos + 8 + size <= len(b)
        chunks[tag] = b[pos + 8:pos + 8 + size]
        pos += 8 + size + size % 2
    code, channels, rate, byte_rate, align, bits, extra = struct.unpack("<HHIIHHH", chunks[b"fmt "])
    assert extra == 0
    assert (code, bits, align, byte_rate) == (3, 32, channels * 4, rate * channels * 4)
    samples = struct.unpack("<%df" % (len(chunks[b"data"]) // 4), chunks[b"data"])
    assert struct.unpack("<I", chunks[b"fact"])[0] == len(samples) // channels
    return rate, channels, samples


with tempfile.TemporaryDirectory(prefix="kinect-audio-tests-") as temp:
    root = pathlib.Path(temp)
    for rate, channels, seconds in [(48000, 2, 2.0234375), (44100, 1, 1.013), (11025, 1, 1.017), (8000, 1, 0.11)]:
        take = root / ("prise-é-%d" % rate)
        invoke("record", "--output", take, "--duration", seconds, "--fast",
               "--sample-rate", rate, "--channels", channels, "--signal", "sine",
               "--frequency", 440, "--segment-seconds", 1)
        manifest = json.loads((take / "manifest.json").read_text(encoding="utf-8"))
        expected = math.floor(seconds * rate + 0.5)
        assert manifest["state"] == "complete" and manifest["frames"] == expected
        assert manifest["clock"] == "synthetic-unpaced"
        data = []
        for entry in manifest["audio_files"]:
            actual_rate, actual_channels, samples = read_wav(take / entry["path"])
            assert (actual_rate, actual_channels) == (rate, channels)
            assert entry["first_stored_frame"] == len(data) // channels
            assert entry["frames"] == len(samples) // channels
            assert len(samples) // channels <= rate
            data.extend(samples)
        assert len(data) == expected * channels
        for frame in range(expected):
            for channel in range(channels):
                reference = 0.25 * math.sin(2 * math.pi * ((frame / rate * 440 * (1.5 if channel else 1)) % 1))
                assert abs(data[frame * channels + channel] - reference) < 2e-6
        packets = [json.loads(line) for line in (take / "timing/audio-packets.jsonl").read_text().splitlines()]
        cursor = 0
        base_timestamp = int(packets[0]["timestamp_100ns"])
        file_offsets = {}
        for p in packets:
            assert int(p["device_frame"]) + p["packet_offset_frames"] == cursor
            assert p["file_frame"] == file_offsets.get(p["file"], 0)
            file_offsets[p["file"]] = p["file_frame"] + p["frames"]
            assert int(p["timestamp_100ns"]) - base_timestamp == int(p["device_frame"]) * 10000000 // rate
            cursor += p["frames"]
        assert cursor == expected
        checkpoint = json.loads((take / "checkpoint.json").read_text())
        assert checkpoint["committed_frames"] == expected
        assert checkpoint["timing_bytes"] == (take / "timing/audio-packets.jsonl").stat().st_size
        original = (take / "manifest.json").read_bytes()
        invoke("record", "--output", take, "--duration", 0.1, "--fast", ok=False)
        assert (take / "manifest.json").read_bytes() == original
    silent = root / "silent"
    invoke("record", "--output", silent, "--duration", 0.03, "--fast", "--amplitude", 0)
    assert all(v == 0 for v in read_wav(silent / "audio/000000.wav")[2])
    for arguments in [("--amplitude", "nan"), ("--channels", "3"), ("--frequency", "24000"),
                      ("--duration", "-1"), ("--fast", "--duration", "0"), ("--source", "typo")]:
        target = root / "invalid"
        invoke("record", "--output", target, *arguments, ok=False)
        assert not target.exists()
    print("Validated float WAVs, exact samples, stereo tones, rotation, timing, Unicode paths, silence and invalid requests.")
