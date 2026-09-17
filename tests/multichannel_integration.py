"""Check every recorded channel independently and round-trip multichannel Matroska."""
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

fixture, cli = map(lambda p: str(pathlib.Path(p).resolve()), sys.argv[1:3])
ffmpeg = os.environ.get("RECORDER_TEST_FFMPEG") or shutil.which("ffmpeg")


def run(*args, ok=True):
    result = subprocess.run(list(map(str, args)), capture_output=True, timeout=45)
    assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
    return result.stdout


def chunks(data):
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    assert struct.unpack_from("<I", data, 4)[0] == len(data) - 8
    pos, result = 12, {}
    while pos < len(data):
        name, size = struct.unpack_from("<4sI", data, pos)
        result[name] = data[pos + 8:pos + 8 + size]
        assert len(result[name]) == size
        pos += 8 + size + (size & 1)
    assert pos == len(data)
    return result


with tempfile.TemporaryDirectory(prefix="recorder-array-") as temp:
    root = pathlib.Path(temp)
    run(fixture, root)
    for channels, rate, mask in [(4, 16000, 0), (8, 44100, 0x63f), (32, 192000, 0), (2, 16000, 0xc), (1, 8000, 4)]:
        take = root / str(channels)
        manifest = json.loads((take / "manifest.json").read_text())
        assert (manifest["channels"], manifest["sample_rate"], manifest["channel_mask"]) == (channels, rate, mask)
        assert manifest["segment_seconds"] == (174 if channels == 32 else 1)
        assert len(manifest["audio_files"]) == (3 if channels == 4 else 1)
        cursor = 0
        for entry in manifest["audio_files"]:
            wav = take / entry["path"]
            data = wav.read_bytes()
            parts = chunks(data)
            fmt = parts[b"fmt "]
            ext = channels != 1
            assert len(fmt) == (40 if ext else 18)
            assert struct.unpack_from("<HHIIHHH", fmt) == (0xfffe if ext else 3, channels, rate, rate * channels * 4, channels * 4, 32, 22 if ext else 0)
            if ext:
                assert struct.unpack_from("<HI", fmt, 18) == (32, mask)
                assert fmt[24:] == bytes.fromhex("0300000000001000800000aa00389b71")
            frames, = struct.unpack("<I", parts[b"fact"])
            assert frames == entry["frames"] and entry["first_stored_frame"] == cursor
            assert len(parts[b"data"]) == frames * channels * 4
            for index, (value,) in enumerate(struct.iter_unpack("<f", parts[b"data"])):
                frame, channel = cursor + index // channels, index % channels
                expected = ((channel + 1) / 64 + (frame % 17) / 4096) * (-1 if channel % 2 else 1)
                assert value == expected, (channels, frame, channel, value, expected)
            cursor += frames
            if ffmpeg and channels == 4:
                depth = take / "depth" / (wav.stem + ".kd16")
                video = root / (wav.stem + ".mkv")
                run(cli, "export-depth", "--input", depth, "--audio", wav, "--output", video, "--ffmpeg", ffmpeg)
                decoded = run(ffmpeg, "-v", "error", "-guess_layout_max", "0", "-i", video, "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", "-")
                assert decoded == parts[b"data"], "Matroska must retain all channels byte for byte"
                if wav.stem == "000000":
                    # Wrong subformat, truncated header and incorrect data size must fail before FFmpeg.
                    corruptions = [data[:44] + b"\x01" + data[45:], data[:60], data[:76] + b"\0\0\0\0" + data[80:]]
                    for n, corrupt in enumerate(corruptions):
                        bad, output = root / (str(n) + "-bad.wav"), root / (str(n) + "-bad.mkv")
                        bad.write_bytes(corrupt)
                        run(cli, "export-depth", "--input", depth, "--audio", bad, "--output", output, "--ffmpeg", ffmpeg, ok=False)
                        assert not output.exists()
        assert cursor == manifest["frames"]
    print("Every channel, WAV metadata, segment boundary and invalid header checked.")
    print("Multichannel Matroska round trip passed." if ffmpeg else "FFmpeg unavailable: Matroska checks skipped.")
