#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace recorder {
enum { DepthWidth = 512, DepthHeight = 424, DepthFps = 30, DepthHeaderBytes = 64 };
struct DepthFrame {
    std::uint64_t index;
    std::int64_t relative_time_100ns;
    std::uint64_t receipt_ticks;
    unsigned min_reliable_mm, max_reliable_mm;
    std::vector<std::uint16_t> millimetres;
    explicit DepthFrame(std::uint64_t i = 0) : index(i), relative_time_100ns(0), receipt_ticks(0),
        min_reliable_mm(0), max_reliable_mm(0), millimetres(DepthWidth * DepthHeight) {}
};
class DepthSource {
public:
    virtual ~DepthSource() {}
    // All calls, including destruction, belong to the depth acquisition thread.
    virtual void open(const std::atomic<bool>& stop) = 0;
    virtual std::string device_id() const = 0;
    virtual std::string calibration_json() const = 0;
    virtual bool read(DepthFrame& frame, const std::atomic<bool>& stop) = 0;
};
bool kinect_depth_supported();
std::unique_ptr<DepthSource> make_kinect_depth_source();
std::uint64_t depth_frames_for_audio(std::uint64_t samples, unsigned rate);
void generate_depth(DepthFrame& frame, const std::string& pattern);

// Simulation follows audio; Kinect frames retain their independent sensor clock.
// All methods belong to the writer thread.
class DepthWriter {
public:
    DepthWriter(const std::string& directory, const std::string& pattern, unsigned rate, unsigned segment_seconds,
        const std::string& device_id = "", const std::string& calibration = "null");
    ~DepthWriter();
    void advance(std::uint64_t audio_frames);
    void write(const DepthFrame& frame);
    void checkpoint();
    void finish();
    std::uint64_t frames() const { return frames_; }
    std::uint64_t gap_intervals() const { return gap_count_; }
    std::uint64_t timing_bytes() const;
    std::size_t finalized_segments() const { return finalized_segments_; }
    std::shared_ptr<const DepthFrame> latest() const { return latest_; }
    std::string json() const;
private:
    DepthWriter(const DepthWriter&);
    DepthWriter& operator=(const DepthWriter&);
    void header(bool finalized);
    void close_segment();
    void append(std::shared_ptr<DepthFrame> frame, std::uint64_t segment);
    struct File { std::string path; std::uint64_t first, count, segment; };
    std::string directory_, pattern_, device_id_, calibration_;
    unsigned rate_, segment_seconds_;
    std::uint64_t frames_;
    std::size_t finalized_segments_;
    std::FILE* file_;
    std::FILE* timing_;
    std::vector<File> files_;
    std::vector<unsigned char> bytes_;
    std::shared_ptr<const DepthFrame> latest_;
    std::int64_t first_time_, previous_time_;
    std::uint64_t gap_count_;
};

// Export one finalized KD16 segment, optionally with its corresponding WAV segment.
void export_depth_video(const std::string& input, const std::string& output,
    const std::string& audio, const std::string& ffmpeg, bool background = false,
    const std::string& log = std::string());
}
