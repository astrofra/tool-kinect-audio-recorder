#include "recorder/settings.h"
#include "recorder/platform.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace recorder {
GuiSettings::GuiSettings() : session("Interview"), window_width(1536), window_height(1024), maximized(false) {
    recording.output = "recordings/take";
    recording.depth_pattern = "gradient";
    recording.encode_depth = true;
    recording.encode_preview = true;
    recording.timestamped_output = true;
}
namespace {
std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \r\n\t");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1);
}
std::string quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out + '"';
}
std::string unquote(const std::string &s) {
    if (s.empty() || s[0] != '"')
        return s;
    std::string out;
    for (std::size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            const auto tail = trim(s.substr(i + 1));
            if (!tail.empty() && tail[0] != ';' && tail[0] != '#')
                throw std::runtime_error("text after quoted value");
            return out;
        }
        if (c == '\\') {
            if (++i == s.size())
                break;
            c = s[i];
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c != '\\' && c != '"')
                throw std::runtime_error("use double backslashes inside quoted paths");
        }
        out += c;
    }
    throw std::runtime_error("unterminated quoted value");
}
double numeric(const std::string &value, double low, double high) {
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    double result;
    stream >> result;
    if (!stream || !stream.eof() || !std::isfinite(result) || result < low || result > high)
        throw std::runtime_error("invalid number or out of range");
    return result;
}
unsigned integer(const std::string &value, unsigned low, unsigned high) {
    const double result = numeric(value, low, high);
    if (result != std::floor(result))
        throw std::runtime_error("expected an integer");
    return static_cast<unsigned>(result);
}
bool boolean(const std::string &value) {
    if (value == "true" || value == "1")
        return true;
    if (value == "false" || value == "0")
        return false;
    throw std::runtime_error("expected true or false");
}
std::string choice(const std::string &value, std::initializer_list<const char *> choices) {
    for (const auto *item : choices)
        if (value == item)
            return value;
    throw std::runtime_error("unknown choice");
}
void assign(GuiSettings &s, const std::string &key, const std::string &value) {
    auto &r = s.recording;
    if (key == "audio.source")
        r.source = choice(value, {"simulate", "wasapi"});
    else if (key == "audio.device_id")
        r.device_id = value;
    else if (key == "audio.device_name")
        s.device_name = value;
    else if (key == "audio.gain_db")
        r.gain_db = numeric(value, -24, 36);
    else if (key == "video.source")
        r.depth_pattern = choice(value, {"off", "gradient", "noise", "kinect"});
    else if (key == "recording.output_prefix") {
        if (value.empty())
            throw std::runtime_error("empty output path");
        r.output = value;
    } else if (key == "recording.session")
        s.session = value;
    else if (key == "recording.duration_seconds")
        r.duration_seconds = numeric(value, 0, 86400);
    else if (key == "recording.segment_seconds")
        r.segment_seconds = integer(value, 1, 600);
    else if (key == "recording.strict_capture")
        r.strict_capture = boolean(value);
    else if (key == "simulation.sample_rate")
        r.sample_rate = integer(value, 8000, 192000);
    else if (key == "simulation.channels")
        r.channels = integer(value, 1, 2);
    else if (key == "simulation.signal")
        r.signal = choice(value, {"markers", "sine"});
    else if (key == "simulation.frequency_hz")
        r.frequency = numeric(value, 1, 96000);
    else if (key == "simulation.amplitude")
        r.amplitude = numeric(value, 0, 1);
    else if (key == "encoding.depth_segments")
        r.encode_depth = boolean(value);
    else if (key == "encoding.rgb_preview")
        r.encode_preview = boolean(value);
    else if (key == "encoding.ffmpeg")
        r.ffmpeg = value;
    else if (key == "ui.last_take")
        s.last_take = value;
    else if (key == "ui.window_width")
        s.window_width = integer(value, 960, 7680);
    else if (key == "ui.window_height")
        s.window_height = integer(value, 640, 4320);
    else if (key == "ui.maximized")
        s.maximized = boolean(value);
}
std::string number(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(12) << value;
    return s.str();
}
} // namespace
GuiSettings load_settings(const std::string &path, std::string &warning) {
    GuiSettings result;
    warning.clear();
    if (!path_exists(path))
        return result;
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> file(open_file(path, "rb"), std::fclose);
    std::string text;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file.get())) != 0) {
        text.append(buffer, n);
        if (text.size() > 1024 * 1024)
            throw std::runtime_error("INI exceeds 1 MiB");
    }
    if (std::ferror(file.get()))
        throw std::runtime_error("Cannot read INI");
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    std::istringstream stream(text);
    std::string line, section;
    unsigned index = 0;
    while (std::getline(stream, line)) {
        ++index;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        try {
            if (line[0] == '[' && line.back() == ']') {
                section = trim(line.substr(1, line.size() - 2));
                continue;
            }
            const auto equal = line.find('=');
            if (equal == std::string::npos || section.empty())
                throw std::runtime_error("expected [section] and key=value");
            const auto key = section + "." + trim(line.substr(0, equal));
            const auto raw = trim(line.substr(equal + 1));
            const auto value = unquote(raw);
            if (value.size() > 2047)
                throw std::runtime_error("value exceeds 2047 bytes");
            assign(result, key, value);
            result.extra[key] = raw;
        } catch (const std::exception &e) {
            warning +=
                (warning.empty() ? "" : "; ") + std::string("INI line ") + std::to_string(index) + ": " + e.what();
        }
    }
    return result;
}
std::string serialize_settings(const GuiSettings &s) {
    auto values = s.extra;
    const auto &r = s.recording;
    values["audio.source"] = quote(r.source);
    values["audio.device_id"] = quote(r.device_id);
    values["audio.device_name"] = quote(s.device_name);
    values["audio.gain_db"] = number(r.gain_db);
    values["video.source"] = quote(r.depth_pattern);
    values["recording.session"] = quote(s.session);
    values["recording.output_prefix"] = quote(r.output);
    values["recording.duration_seconds"] = number(r.duration_seconds);
    values["recording.segment_seconds"] = number(r.segment_seconds);
    values["recording.strict_capture"] = r.strict_capture ? "true" : "false";
    values["simulation.sample_rate"] = number(r.sample_rate);
    values["simulation.channels"] = number(r.channels);
    values["simulation.signal"] = quote(r.signal);
    values["simulation.frequency_hz"] = number(r.frequency);
    values["simulation.amplitude"] = number(r.amplitude);
    values["encoding.depth_segments"] = r.encode_depth ? "true" : "false";
    values["encoding.rgb_preview"] = r.encode_preview ? "true" : "false";
    values["encoding.ffmpeg"] = quote(r.ffmpeg);
    values["ui.last_take"] = quote(s.last_take);
    values["ui.window_width"] = number(s.window_width);
    values["ui.window_height"] = number(s.window_height);
    values["ui.maximized"] = s.maximized ? "true" : "false";
    std::string out = "; Recorder settings (UTF-8). Saved automatically. Edit while the app is closed.\n"
                      "; Gain changes the stored samples. Paths are relative to this file.\n"
                      "; Quoted Windows paths use doubled backslashes. Unknown keys are retained.\n";
    std::string section;
    for (const auto &value : values) {
        const auto dot = value.first.find('.');
        if (dot == std::string::npos)
            continue;
        const auto next = value.first.substr(0, dot);
        if (section != next) {
            section = next;
            out += "\n[" + section + "]\n";
        }
        out += value.first.substr(dot + 1) + "=" + value.second + "\n";
    }
    return out;
}
void save_settings(const std::string &path, const GuiSettings &settings) {
    write_atomic(path, serialize_settings(settings));
}
std::string settings_relative_path(const std::string &ini, const std::string &path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'))
        return path;
    const auto slash = ini.find_last_of("/\\");
    return slash == std::string::npos ? path : path_join(ini.substr(0, slash), path);
}
} // namespace recorder
