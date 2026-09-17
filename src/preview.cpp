#include "recorder/depth.h"
#include "recorder/platform.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace recorder {
void colorize_depth(const std::vector<std::uint16_t>& depth, std::vector<unsigned char>& rgb) {
    rgb.resize(depth.size() * 3);
    for (std::size_t i = 0; i < depth.size(); ++i) {
        const float t = std::max(0.0f, std::min(1.0f, (depth[i] - 500.0f) / 5500.0f));
        rgb[i * 3] = depth[i] ? static_cast<unsigned char>(255 * t) : 0;
        rgb[i * 3 + 1] = depth[i] ? static_cast<unsigned char>(255 * (1 - std::abs(2 * t - 1))) : 0;
        rgb[i * 3 + 2] = depth[i] ? static_cast<unsigned char>(255 * (1 - t)) : 0;
    }
}
namespace {
typedef std::unique_ptr<std::FILE, int(*)(std::FILE*)> File;
const std::uint64_t frame_bytes = DepthWidth * DepthHeight * 2;
std::uint64_t little(const unsigned char* p, unsigned n) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= std::uint64_t(p[i]) << (8 * i);
    return value;
}
void seek_end(std::FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) throw std::runtime_error("Cannot measure depth file");
#else
    if (fseeko(f, 0, SEEK_END)) throw std::runtime_error("Cannot measure depth file");
#endif
}
std::string contents(const std::string& path) {
    File f(open_file(path, "rb"), std::fclose);
    std::string text;
    char buffer[4096]; std::size_t count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), f.get())) != 0) {
        text.append(buffer, count);
        if (text.size() > 8 * 1024 * 1024) throw std::runtime_error("Manifest too large");
    }
    if (std::ferror(f.get())) throw std::runtime_error("Cannot read manifest");
    return text;
}
// Read scalar fields of the recorder's own JSON format. Paths used below are
// restricted to the generated depth/NNNNNN.kd16 names, never arbitrary JSON paths.
std::string field(const std::string& text, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    std::size_t p = text.find(marker);
    if (p == std::string::npos) throw std::runtime_error("Missing preview metadata: " + key);
    p = text.find_first_not_of(" \r\n\t", p + marker.size());
    if (p == std::string::npos) throw std::runtime_error("Invalid preview metadata: " + key);
    const bool quoted = text[p] == '"';
    if (quoted) ++p;
    const std::size_t end = text.find_first_of(quoted ? "\"\\\r\n" : ",} \r\n\t", p);
    if (end == std::string::npos || end == p || (quoted && text[end] != '"'))
        throw std::runtime_error("Invalid preview metadata: " + key);
    return text.substr(p, end - p);
}
std::uint64_t number(const std::string& text, const std::string& key) {
    const std::string value = field(text, key);
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9' || n > (std::numeric_limits<std::uint64_t>::max() - (value[i] - '0')) / 10)
            throw std::runtime_error("Invalid preview number: " + key);
        n = n * 10 + (value[i] - '0');
    }
    return n;
}
struct Segment { std::string path; std::uint64_t first, count; };
struct Frame { std::uint64_t slot; std::size_t segment; };
Segment segment_info(const std::string& take, const std::string& path, bool native) {
    File f(open_file(path_join(take, path), "rb"), std::fclose);
    unsigned char h[DepthHeaderBytes];
    if (std::fread(h, 1, sizeof(h), f.get()) != sizeof(h) ||
            std::memcmp(h, native ? "KD16RAW\0" : "KD16SIM\0", 8) ||
            little(h + 8, 4) != 1 || little(h + 12, 4) != DepthHeaderBytes ||
            little(h + 16, 4) != DepthWidth || little(h + 20, 4) != DepthHeight ||
            little(h + 24, 4) != DepthFps || little(h + 28, 4) != 1 || little(h + 52, 4) != 1)
        throw std::runtime_error("Preview requires finalized 512x424 depth segments: " + path);
    Segment segment = {path, little(h + 32, 8), little(h + 40, 8)};
    seek_end(f.get());
    if (!segment.count || segment.count > (std::numeric_limits<std::uint64_t>::max() - DepthHeaderBytes) / frame_bytes ||
            file_position(f.get()) != DepthHeaderBytes + segment.count * frame_bytes)
        throw std::runtime_error("Truncated or inconsistent depth segment: " + path);
    return segment;
}
}
void export_depth_preview(const std::string& take, const std::string& output,
        const std::string& ffmpeg, bool background, const std::string& log) {
    if (take.empty() || output.empty() || ffmpeg.empty()) throw std::invalid_argument("Preview requires take, output and FFmpeg");
    const std::string partial = output + ".part";
    if (path_exists(output) || path_exists(partial)) throw std::runtime_error("Preview output already exists: " + output);
    const std::string manifest = contents(path_join(take, "manifest.json"));
    const std::string state = field(manifest, "state");
    if (state != "complete" && state != "interrupted") throw std::runtime_error("Stop and finalize the take before exporting its preview");
    const std::size_t depth_start = manifest.find("\"depth\":");
    if (depth_start == std::string::npos) throw std::runtime_error("Take has no depth metadata");
    const std::string depth = manifest.substr(depth_start);
    const std::string source = field(depth, "source");
    if (source != "kinect-sdk-2.0" && source != "simulate") throw std::runtime_error("Unsupported preview source");
    const bool native = source == "kinect-sdk-2.0";
    const std::uint64_t frequency = native ? number(manifest, "host_clock_frequency") : 10000000;
    const std::uint64_t expected = number(depth, "frames");
    if (!frequency || !expected) throw std::runtime_error("Take contains no timed depth frames");
    File journal(open_file(path_join(take, "timing/depth-frames.jsonl"), "rb"), std::fclose);
    std::vector<Segment> segments;
    std::vector<Frame> frames;
    std::uint64_t origin = 0, previous = 0, in_segment = 0;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), journal.get())) {
        const std::string line(buffer);
        if (line.empty() || line.back() != '\n') throw std::runtime_error("Truncated depth timing journal");
        const std::string path = field(line, "file");
        if (segments.empty() || segments.back().path != path) {
            if (!segments.empty() && in_segment != segments.back().count) throw std::runtime_error("Depth journal omits stored frames");
            std::ostringstream name; name << "depth/" << std::setw(6) << std::setfill('0') << segments.size() << ".kd16";
            if (path != name.str()) throw std::runtime_error("Unexpected depth segment path in journal");
            segments.push_back(segment_info(take, path, native));
            if (segments.back().first != frames.size()) throw std::runtime_error("Non-contiguous depth segment index");
            in_segment = 0;
        }
        if (number(line, "frame") != frames.size() || number(line, "file_frame") != in_segment ||
                in_segment >= segments.back().count || number(line, "byte_offset") != DepthHeaderBytes + in_segment * frame_bytes)
            throw std::runtime_error("Inconsistent depth timing journal index");
        const std::uint64_t time = number(line, native ? "receipt_ticks" : "pts_100ns");
        if (frames.empty()) origin = time;
        else if (time < previous) throw std::runtime_error("Non-monotonic preview timeline");
        previous = time;
        const long double seconds = static_cast<long double>(time - origin) / frequency;
        if (seconds > 7 * 86400) throw std::runtime_error("Preview timeline exceeds seven days");
        Frame frame = {static_cast<std::uint64_t>(seconds * DepthFps + 0.5L), segments.size() - 1};
        frames.push_back(frame); ++in_segment;
        if (frames.size() > expected) throw std::runtime_error("Depth journal exceeds manifest frame count");
    }
    if (std::ferror(journal.get()) || frames.size() != expected || segments.empty() || in_segment != segments.back().count)
        throw std::runtime_error("Incomplete depth timing journal");
    journal.reset();
    const std::size_t separator = output.find_last_of("/\\");
    if (separator != std::string::npos) create_directories(output.substr(0, separator));
    std::vector<std::string> args = {ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n",
        "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size", "512x424", "-framerate", "30", "-i", "pipe:0",
        "-map", "0:v:0", "-an", "-c:v", "ffv1", "-level", "3", "-g", "1", "-slicecrc", "1",
        "-threads", "2", "-pix_fmt", "+bgr0", "-metadata", "title=Depth preview - recorder RGB palette",
        "-metadata", "PREVIEW_PALETTE=blue-green-red;500..6000mm;zero=black",
        "-metadata", native ? "PREVIEW_TIMELINE=host receipt time;30fps;gaps hold last image;no audio sync" :
            "PREVIEW_TIMELINE=stored sample time;30fps;silent",
        "-metadata", "SOURCE_FRAME_COUNT=" + std::to_string(frames.size()), "-f", "matroska", partial};
    const int result = run_process(args, background, log.empty() ? output + ".log" : log, [&](const ProcessWrite& write) {
        File raw(nullptr, std::fclose);
        std::size_t opened = segments.size();
        std::vector<unsigned char> bytes(static_cast<std::size_t>(frame_bytes)), rgb, bgr(DepthWidth * DepthHeight * 4);
        std::vector<std::uint16_t> pixels(DepthWidth * DepthHeight);
        std::uint64_t written = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            // Repeat only in this disposable review video; raw files and journals retain every gap.
            while (written < frames[i].slot) { write(bgr.data(), bgr.size()); ++written; }
            if (opened != frames[i].segment) {
                opened = frames[i].segment;
                raw.reset(open_file(path_join(take, segments[opened].path), "rb"));
                unsigned char header[DepthHeaderBytes];
                if (std::fread(header, 1, sizeof(header), raw.get()) != sizeof(header)) throw std::runtime_error("Cannot read depth header");
            }
            if (std::fread(bytes.data(), 1, bytes.size(), raw.get()) != bytes.size()) throw std::runtime_error("Cannot read depth image");
            for (std::size_t p = 0; p < pixels.size(); ++p) pixels[p] = static_cast<std::uint16_t>(little(&bytes[p * 2], 2));
            colorize_depth(pixels, rgb);
            for (std::size_t p = 0; p < pixels.size(); ++p) {
                bgr[p * 4] = rgb[p * 3 + 2]; bgr[p * 4 + 1] = rgb[p * 3 + 1]; bgr[p * 4 + 2] = rgb[p * 3];
            }
        }
        write(bgr.data(), bgr.size());
    });
    if (result) throw std::runtime_error("FFmpeg preview failed (exit " + std::to_string(result) + "); see " + (log.empty() ? output + ".log" : log));
    File encoded(open_file(partial, "rb"), std::fclose);
    unsigned char magic[4];
    if (std::fread(magic, 1, 4, encoded.get()) != 4 || little(magic, 4) != 0xa3df451a)
        throw std::runtime_error("Encoder did not produce a Matroska preview");
    encoded.reset();
    publish_file(partial, output);
}
}
