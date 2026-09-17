"""Drive GUI controls, then restart with the same INI to verify persisted behavior."""
import configparser
import json
import pathlib
import subprocess
import sys
import tempfile

gui = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="recorder-gui-settings-") as temp:
    root = pathlib.Path(temp) / "prise-é & configuration"
    root.mkdir()
    ini = root / "recorder.ini"
    ini.write_text('''[audio]
source=simulate
device_id="remembered-unavailable-input"
device_name="USB microphone"
gain_db=6
[video]
source=gradient
[simulation]
channels=2
amplitude=0.02
[recording]
session="Entretien_003"
strict_capture=true
[encoding]
rgb_preview=false
depth_segments=false
[ui]
window_width=1200
window_height=800
[future]
unknown="retained"
''', encoding="utf-8")

    def run(mode, name, ok=True):
        result = subprocess.run([gui, mode, str(root / name), "--config", str(ini)], capture_output=True, timeout=30, cwd=root)
        assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
        return root / name

    first = run("--controls-smoke-test", "first")
    m = json.loads((first / "manifest.json").read_text())
    assert m["state"] == "complete" and m["warnings"] == 0 and m["session"] == "Entretien_003"
    assert m["audio_gain"]["initial_db"] == 6
    packets = [json.loads(line) for line in (first / "timing/audio-packets.jsonl").read_text().splitlines()]
    assert packets[0]["gain_db"] == 6 and packets[-1]["gain_db"] == 18, "Mouse wheel changes captured gain"
    assert any(p["pause_boundary"] for p in packets), "Pause/reprise buttons reach recorder"
    events = [json.loads(line) for line in (first / "timing/events.jsonl").read_text().splitlines()]
    assert [e["type"] for e in events if e["type"] in ("pause", "resume")] == ["pause", "resume"]
    assert 0.6 <= m["stored_duration_seconds"] < 1, "Stop button finalizes before automatic duration"
    config = configparser.ConfigParser(interpolation=None)
    config.read(ini, encoding="utf-8")
    assert config["audio"]["gain_db"] == "18" and config["audio"]["device_id"] == '"remembered-unavailable-input"'
    assert config["future"]["unknown"] == '"retained"'
    second = run("--smoke-test", "second")
    m2 = json.loads((second / "manifest.json").read_text())
    assert m2["audio_gain"]["initial_db"] == 18 and m2["channels"] == 2 and m2["depth"]["pattern"] == "gradient"
    assert m2["session"] == "Entretien_003" and m2["warnings"] == 0
    # An unavailable remembered microphone must never turn into the default input.
    content = ini.read_text(encoding="utf-8").replace('source="simulate"', 'source="wasapi"')
    ini.write_text(content, encoding="utf-8")
    bad = run("--smoke-test", "missing-device", ok=False)
    assert not bad.exists()
    assert 'device_id="remembered-unavailable-input"' in ini.read_text(encoding="utf-8")
    print("Record/gain/pause/resume/stop mouse controls, INI restart and unavailable device retention passed.")
