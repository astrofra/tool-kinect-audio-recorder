#include "recorder/wav_writer.h"
#include "recorder/platform.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace recorder {
bool wav_extensible(AudioFormat format) {
    return format.channels > 2 || (format.channel_mask &&
        format.channel_mask != (format.channels == 1 ? 4U : 3U));
}
std::uint64_t wav_max_frames(AudioFormat format) {
    if (!format.channels || format.channels > MaxAudioChannels) throw std::invalid_argument("Invalid WAV channel count");
    return (0xffffffffULL - (wav_extensible(format) ? 72 : 50)) / (format.channels * 4);
}
static void bytes(std::FILE* f, const void* data, std::size_t n) {
    if (std::fwrite(data, 1, n, f) != n) throw std::runtime_error("Audio write failed (check free disk space)");
}
static void u16(std::FILE* f, unsigned n) {
    unsigned char b[2] = {static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8)};
    bytes(f, b, sizeof(b));
}
static void u32(std::FILE* f, std::uint32_t n) {
    unsigned char b[4] = {static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8),
        static_cast<unsigned char>(n >> 16), static_cast<unsigned char>(n >> 24)};
    bytes(f, b, sizeof(b));
}
WavWriter::WavWriter(const std::string& path, AudioFormat format) : file_(0), format_(format), frames_(0) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE float32 required");
    if (!format.channels || format.channels > MaxAudioChannels || !format.sample_rate || format.sample_rate > 192000)
        throw std::invalid_argument("Invalid WAV format");
    file_ = open_file(path, "wb+");
    try { header(); } catch (...) { std::fclose(file_); file_ = 0; throw; }
}
WavWriter::~WavWriter() { if (file_) std::fclose(file_); }
void WavWriter::header() {
    if (std::fseek(file_, 0, SEEK_SET)) throw std::runtime_error("WAV header seek failed");
    const std::uint32_t size = static_cast<std::uint32_t>(frames_ * format_.channels * 4);
    const bool extensible = wav_extensible(format_);
    bytes(file_, "RIFF", 4); u32(file_, size + (extensible ? 72 : 50)); bytes(file_, "WAVEfmt ", 8); u32(file_, extensible ? 40 : 18);
    u16(file_, extensible ? 0xfffe : 3); u16(file_, format_.channels); u32(file_, format_.sample_rate);
    u32(file_, format_.sample_rate * format_.channels * 4); u16(file_, format_.channels * 4); u16(file_, 32);
    u16(file_, extensible ? 22 : 0);
    if (extensible) {
        u16(file_, 32); u32(file_, format_.channel_mask); // Zero preserves unspecified channel positions (e.g. microphone arrays).
        const unsigned char float_guid[16] = {3, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
        bytes(file_, float_guid, sizeof(float_guid));
    }
    bytes(file_, "fact", 4); u32(file_, 4); u32(file_, static_cast<std::uint32_t>(frames_));
    bytes(file_, "data", 4); u32(file_, size);
    if (std::fseek(file_, 0, SEEK_END)) throw std::runtime_error("WAV data seek failed");
}
void WavWriter::write(const float* samples, std::uint64_t frames) {
    if (!file_) throw std::runtime_error("WAV is closed");
    if (frames > wav_max_frames(format_) - frames_)
        throw std::runtime_error("WAV size limit exceeded; rotate audio files");
    const std::uint16_t one = 1;
    if (*reinterpret_cast<const unsigned char*>(&one) == 1) {
        bytes(file_, samples, static_cast<std::size_t>(frames * format_.channels * 4));
    } else {
        for (std::uint64_t i = 0; i < frames * format_.channels; ++i) {
            std::uint32_t bits; std::memcpy(&bits, samples + i, 4); u32(file_, bits);
        }
    }
    frames_ += frames;
}
void WavWriter::checkpoint() { if (file_) { header(); sync_file(file_); } }
void WavWriter::close() {
    if (!file_) return;
    checkpoint();
    std::FILE* f = file_; file_ = 0;
    if (std::fclose(f)) throw std::runtime_error("WAV close failed");
}
}
