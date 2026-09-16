#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace recorder {
enum { DepthWidth = 512, DepthHeight = 424, DepthFps = 30, DepthHeaderBytes = 64 };
struct DepthFrame {
    std::uint64_t index;
    std::vector<std::uint16_t> millimetres;
    explicit DepthFrame(std::uint64_t i = 0) : index(i), millimetres(DepthWidth * DepthHeight) {}
};
std::uint64_t depth_frames_for_audio(std::uint64_t samples, unsigned rate);
void generate_depth(DepthFrame& frame, const std::string& pattern);

// Simulation follows the stored audio timeline. All methods belong to the writer thread.
class DepthWriter {
public:
    DepthWriter(const std::string& directory, const std::string& pattern, unsigned rate, unsigned segment_seconds);
    ~DepthWriter();
    void advance(std::uint64_t audio_frames);
    void checkpoint();
    void finish();
    std::uint64_t frames() const { return frames_; }
    std::uint64_t timing_bytes() const;
    std::size_t finalized_segments() const { return finalized_segments_; }
    std::shared_ptr<const DepthFrame> latest() const { return latest_; }
    std::string json() const;
private:
    DepthWriter(const DepthWriter&);
    DepthWriter& operator=(const DepthWriter&);
    void header(bool finalized);
    void close_segment();
    struct File { std::string path; std::uint64_t first, count; };
    std::string directory_, pattern_;
    unsigned rate_, segment_seconds_;
    std::uint64_t frames_;
    std::size_t finalized_segments_;
    std::FILE* file_;
    std::FILE* timing_;
    std::vector<File> files_;
    std::vector<unsigned char> bytes_;
    std::shared_ptr<const DepthFrame> latest_;
};

// Export one finalized KD16 segment, optionally with its corresponding WAV segment.
void export_depth_video(const std::string& input, const std::string& output,
    const std::string& audio, const std::string& ffmpeg, bool background = false,
    const std::string& log = std::string());
}
