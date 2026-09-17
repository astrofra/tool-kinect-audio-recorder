#include "recorder/depth.h"
#include "recorder/platform.h"
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/types.h>
#endif

namespace recorder {
namespace {
void write_bytes(std::FILE* f, const void* data, std::size_t size) {
    if (std::fwrite(data, 1, size, f) != size) throw std::runtime_error("Depth write failed");
}
void seek(std::FILE* f, std::int64_t offset, int whence) {
#ifdef _WIN32
    const int result = _fseeki64(f, offset, whence);
#else
    const int result = fseeko(f, static_cast<off_t>(offset), whence);
#endif
    if (result) throw std::runtime_error("Depth seek failed");
}
void put(unsigned char* b, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) b[i] = static_cast<unsigned char>(value >> (i * 8));
}
std::uint64_t get(const unsigned char* b, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(b[i]) << (i * 8);
    return value;
}
std::uint64_t audio_position(std::uint64_t frame, unsigned rate) {
    return frame / DepthFps * rate + frame % DepthFps * rate / DepthFps;
}
}

std::uint64_t depth_frames_for_audio(std::uint64_t samples, unsigned rate) {
    if (!rate) throw std::invalid_argument("Depth simulation requires an audio sample rate");
    // One frame at every n/30 strictly before the audio end, including frame zero.
    return samples / rate * DepthFps + ((samples % rate) * DepthFps + rate - 1) / rate;
}
void generate_depth(DepthFrame& frame, const std::string& pattern) {
    if (pattern != "gradient" && pattern != "noise") throw std::invalid_argument("Unknown depth pattern");
    frame.millimetres.resize(DepthWidth * DepthHeight);
    for (unsigned y = 0; y < DepthHeight; ++y) for (unsigned x = 0; x < DepthWidth; ++x) {
        std::uint32_t value;
        if (pattern == "noise") {
            // Defined unsigned arithmetic: deterministic across compilers/platforms.
            value = (y * DepthWidth + x) ^ (static_cast<std::uint32_t>(frame.index) * 747796405U) ^ 2891336453U;
            value ^= value >> 16; value *= 2246822519U;
            value ^= value >> 13; value *= 3266489917U; value ^= value >> 16;
            value = 500 + value % 5501;
        } else {
            value = 500 + (x * 4000 / (DepthWidth - 1) + y * 1500 / (DepthHeight - 1) +
                static_cast<unsigned>(frame.index % 5501) * 35) % 5501;
        }
        // A visible 100 ms marker at each audio sample-clock second.
        if (x >= 8 && x < 40 && y >= 8 && y < 40) value = frame.index % DepthFps < 3 ? 1000 : 5000;
        if (!x || !y || x == DepthWidth - 1 || y == DepthHeight - 1) value = 0; // Invalid depth border.
        frame.millimetres[y * DepthWidth + x] = static_cast<std::uint16_t>(value);
    }
}
DepthWriter::DepthWriter(const std::string& directory, const std::string& pattern, unsigned rate, unsigned segment_seconds,
        const std::string& device_id, const std::string& calibration, bool allow_clock_reset)
    : directory_(directory), pattern_(pattern), device_id_(device_id), calibration_(calibration),
      rate_(rate), segment_seconds_(segment_seconds), frames_(0), finalized_segments_(0), file_(0), timing_(0),
      bytes_(DepthWidth * DepthHeight * 2), first_time_(0), previous_time_(0), gap_count_(0),
      allow_clock_reset_(allow_clock_reset), timestamp_epoch_(0), epoch_segment_base_(0) {
    if (!rate_ || !segment_seconds_ || (pattern_ != "gradient" && pattern_ != "noise" && pattern_ != "kinect"))
        throw std::invalid_argument("Invalid depth writer settings");
    create_directories(path_join(directory_, "depth"));
    timing_ = open_file(path_join(directory_, "timing/depth-frames.jsonl"), "wb");
}
DepthWriter::~DepthWriter() {
    if (file_) std::fclose(file_);
    if (timing_) std::fclose(timing_);
}
void DepthWriter::header(bool finalized) {
    unsigned char b[DepthHeaderBytes] = {};
    std::memcpy(b, pattern_ == "kinect" ? "KD16RAW" : "KD16SIM", 7);
    put(b + 8, 1, 4); put(b + 12, DepthHeaderBytes, 4);
    put(b + 16, DepthWidth, 4); put(b + 20, DepthHeight, 4);
    put(b + 24, DepthFps, 4); put(b + 28, 1, 4);
    put(b + 32, files_.back().first, 8); put(b + 40, files_.back().count, 8);
    put(b + 48, rate_, 4); put(b + 52, finalized ? 1 : 0, 4);
    // Native frames have no resolved audio-sample mapping yet.
    put(b + 56, pattern_ == "kinect" ? 0 : audio_position(files_.back().first, rate_), 8);
    seek(file_, 0, SEEK_SET); write_bytes(file_, b, sizeof(b)); seek(file_, 0, SEEK_END);
}
void DepthWriter::close_segment() {
    if (!file_) return;
    header(true); sync_file(file_);
    std::FILE* closing = file_; file_ = 0;
    if (std::fclose(closing)) throw std::runtime_error("Depth file close failed");
    ++finalized_segments_;
}
void DepthWriter::advance(std::uint64_t audio_frames) {
    if (pattern_ == "kinect") return;
    const std::uint64_t target = depth_frames_for_audio(audio_frames, rate_);
    while (frames_ < target) {
        std::shared_ptr<DepthFrame> frame(new DepthFrame(frames_));
        generate_depth(*frame, pattern_);
        append(frame, frames_ / (static_cast<std::uint64_t>(segment_seconds_) * DepthFps));
    }
}
void DepthWriter::write(const DepthFrame& frame) {
    if (pattern_ != "kinect" || frame.millimetres.size() != DepthWidth * DepthHeight ||
        frame.relative_time_100ns < 0 || (frames_ && frame.receipt_ticks < latest_->receipt_ticks))
        throw std::runtime_error("Invalid or non-monotonic Kinect depth frame");
    const bool reset = frames_ && frame.relative_time_100ns <= previous_time_;
    if (reset && !allow_clock_reset_) throw std::runtime_error("Kinect depth clock reset");
    if (reset) {
        ++timestamp_epoch_; first_time_ = frame.relative_time_100ns;
        epoch_segment_base_ = files_.back().segment + 1;
    }
    if (!frames_) first_time_ = frame.relative_time_100ns;
    const bool gap = reset || (frames_ && frame.relative_time_100ns - previous_time_ > 500000);
    append(std::shared_ptr<DepthFrame>(new DepthFrame(frame)),
        epoch_segment_base_ + static_cast<std::uint64_t>(frame.relative_time_100ns - first_time_) / (10000000ULL * segment_seconds_));
    previous_time_ = frame.relative_time_100ns;
    if (gap) ++gap_count_;
}
void DepthWriter::append(std::shared_ptr<DepthFrame> frame, std::uint64_t segment) {
    if (!file_ || files_.back().segment != segment) {
        close_segment();
        std::ostringstream name; name << "depth/" << std::setw(6) << std::setfill('0') << files_.size() << ".kd16";
        File entry = { name.str(), frames_, 0, segment }; files_.push_back(entry);
        file_ = open_file(path_join(directory_, entry.path), "wb"); header(false);
    }
    for (std::size_t i = 0; i < frame->millimetres.size(); ++i) put(&bytes_[i * 2], frame->millimetres[i], 2);
    const std::uint64_t offset = file_position(file_);
    write_bytes(file_, bytes_.data(), bytes_.size());
    std::ostringstream line;
    line << "{\"frame\":" << frames_ << ",\"file\":" << json_string(files_.back().path)
         << ",\"file_frame\":" << files_.back().count << ",\"byte_offset\":\"" << offset << '"';
    if (pattern_ == "kinect") {
        const std::int64_t delta = frames_ ? frame->relative_time_100ns - previous_time_ : 0;
        line << ",\"relative_time_100ns\":\"" << frame->relative_time_100ns
             << "\",\"source_frame\":\"" << frame->index << "\",\"timestamp_epoch\":\"" << timestamp_epoch_
             << "\",\"receipt_ticks\":\"" << frame->receipt_ticks
             << "\",\"sensor_delta_100ns\":\"" << delta
             << "\",\"gap_before\":" << (frames_ && (delta <= 0 || delta > 500000) ? "true" : "false")
             << ",\"min_reliable_mm\":" << frame->min_reliable_mm
             << ",\"max_reliable_mm\":" << frame->max_reliable_mm << "}\n";
    } else {
        line << ",\"pts_100ns\":\"" << frames_to_100ns(frames_, DepthFps)
         << "\",\"audio_sample_floor\":\"" << audio_position(frames_, rate_)
         << "\",\"generated_at_ticks\":\"" << clock_ticks() << "\"}\n";
    }
    const std::string text = line.str(); write_bytes(timing_, text.data(), text.size());
    ++frames_; ++files_.back().count; latest_ = frame;
}

void DepthWriter::checkpoint() {
    if (file_) { header(false); sync_file(file_); }
    sync_file(timing_);
}
void DepthWriter::finish() { close_segment(); sync_file(timing_); }
std::uint64_t DepthWriter::timing_bytes() const { return file_position(timing_); }
std::string DepthWriter::json() const {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "{\"source\":" << json_string(pattern_ == "kinect" ? "kinect-sdk-2.0" : "simulate")
      << ",\"device_id\":" << json_string(device_id_) << ",\"pattern\":" << json_string(pattern_)
      << ",\"width\":512,\"height\":424,\"fps_num\":30,\"fps_den\":1,\"pixel_format\":\"gray16le\","
         "\"units\":\"millimetres\",\"invalid_value\":0,\"clock\":"
      << json_string(pattern_ == "kinect" ? "kinect-relative-100ns" : "stored-audio-sample-clock")
      << ",\"timing_resolved\":" << (pattern_ == "kinect" ? "false" : "true")
      << ",\"calibration\":" << calibration_ << ",\"gap_intervals\":" << gap_count_
      << ",\"clock_resets\":" << timestamp_epoch_
      << ",\"frames\":" << frames_ << ",\"files\":[";
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (i) s << ',';
        s << "{\"path\":" << json_string(files_[i].path) << ",\"first_frame\":" << files_[i].first
          << ",\"frames\":" << files_[i].count << '}';
    }
    s << "]}"; return s.str();
}

void export_depth_video(const std::string& input, const std::string& output,
        const std::string& audio, const std::string& ffmpeg, bool background, const std::string& log) {
    if (input.empty() || output.empty() || ffmpeg.empty()) throw std::invalid_argument("Export requires input, output and FFmpeg");
    // Some FFmpeg versions return success for -n refusal; check before launching as well.
    if (path_exists(output)) throw std::runtime_error("Export output already exists: " + output);
    std::unique_ptr<std::FILE, int(*)(std::FILE*)> file(open_file(input, "rb"), std::fclose);
    unsigned char b[DepthHeaderBytes];
    if (std::fread(b, 1, sizeof(b), file.get()) != sizeof(b) || std::memcmp(b, "KD16SIM\0", 8) ||
        get(b + 8, 4) != 1 || get(b + 12, 4) != DepthHeaderBytes || get(b + 16, 4) != DepthWidth ||
        get(b + 20, 4) != DepthHeight || get(b + 24, 4) != DepthFps || get(b + 28, 4) != 1 ||
        get(b + 52, 4) != 1) throw std::runtime_error("Expected a finalized KD16 simulation segment (512x424, 30 fps)");
    const std::uint64_t count = get(b + 40, 8), bytes_per_frame = DepthWidth * DepthHeight * 2;
    const unsigned rate = static_cast<unsigned>(get(b + 48, 4));
    const std::uint64_t first = get(b + 32, 8);
    if (rate < 8000 || rate > 192000 || first % DepthFps ||
        first / DepthFps > std::numeric_limits<std::uint64_t>::max() / rate ||
        get(b + 56, 8) != audio_position(first, rate)) throw std::runtime_error("Invalid depth segment timeline");
    seek(file.get(), 0, SEEK_END);
    const std::uint64_t size = file_position(file.get());
    if (!count || count > (std::numeric_limits<std::uint64_t>::max() - DepthHeaderBytes) / bytes_per_frame ||
        size != DepthHeaderBytes + count * bytes_per_frame) throw std::runtime_error("Truncated or inconsistent depth segment");
    file.reset();
    if (!audio.empty()) {
        // Accept the recorder's own float32 WAV layout, not arbitrary resampled media.
        std::unique_ptr<std::FILE, int(*)(std::FILE*)> wav(open_file(audio, "rb"), std::fclose);
        unsigned char h[58];
        if (std::fread(h, 1, sizeof(h), wav.get()) != sizeof(h) || std::memcmp(h, "RIFF", 4) ||
            std::memcmp(h + 8, "WAVEfmt ", 8) || get(h + 16, 4) != 18 || get(h + 20, 2) != 3 ||
            get(h + 24, 4) != rate || get(h + 34, 2) != 32 || get(h + 36, 2) != 0 ||
            std::memcmp(h + 38, "fact", 4) || get(h + 42, 4) != 4 || std::memcmp(h + 50, "data", 4))
            throw std::runtime_error("Expected a matching recorder float32 WAV segment");
        const unsigned channels = static_cast<unsigned>(get(h + 22, 2));
        const std::uint64_t samples = get(h + 46, 4), data_bytes = get(h + 54, 4);
        seek(wav.get(), 0, SEEK_END);
        if (!channels || channels > 2 || get(h + 32, 2) != channels * 4 || get(h + 28, 4) != rate * channels * 4 ||
            data_bytes != samples * channels * 4 || file_position(wav.get()) != 58 + data_bytes ||
            get(h + 4, 4) != 50 + data_bytes || depth_frames_for_audio(samples, rate) != count)
            throw std::runtime_error("Audio and depth segment durations/formats do not match");
    }
    // argv is passed directly to the OS. No shell expansion of paths or executable names.
    std::vector<std::string> args = { ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-n",
        "-f", "rawvideo", "-pixel_format", "gray16le", "-video_size", "512x424", "-framerate", "30",
        "-skip_initial_bytes", "64", "-i", input };
    if (!audio.empty()) { args.push_back("-i"); args.push_back(audio); }
    if (background) {
        args.push_back("-filter_threads"); args.push_back("1");
        args.push_back("-filter_complex_threads"); args.push_back("1");
    }
    const char* video[] = { "-map", "0:v:0", "-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1",
        "-g", "1", "-slicecrc", "1", "-threads", background ? "2" : "4", "-pix_fmt", "+gray16le" };
    args.insert(args.end(), video, video + sizeof(video) / sizeof(video[0]));
    if (!audio.empty()) {
        args.push_back("-map"); args.push_back("1:a:0"); args.push_back("-c:a"); args.push_back("pcm_f32le");
    }
    args.push_back("-metadata"); args.push_back("DEPTH_UNITS=millimetres");
    args.push_back("-metadata"); args.push_back("DEPTH_INVALID_VALUE=0");
    args.push_back("-metadata"); args.push_back("SOURCE_FIRST_FRAME=" + std::to_string(get(b + 32, 8)));
    args.push_back("-f"); args.push_back("matroska"); args.push_back(output);
    const int result = run_process(args, background, log);
    if (result) throw std::runtime_error("FFmpeg export failed (exit " + std::to_string(result) + "); any partial output is incomplete");
    std::unique_ptr<std::FILE, int(*)(std::FILE*)> encoded(open_file(output, "rb"), std::fclose);
    unsigned char magic[4];
    if (std::fread(magic, 1, 4, encoded.get()) != 4 || get(magic, 4) != 0xa3df451a)
        throw std::runtime_error("Encoder did not produce a Matroska file");
}
}
