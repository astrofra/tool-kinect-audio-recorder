#pragma once
#include "recorder/audio.h"
#include <cstdio>
#include <string>

namespace recorder {
bool wav_extensible(AudioFormat format);
std::uint64_t wav_max_frames(AudioFormat format);
class WavWriter {
public:
    WavWriter(const std::string& path, AudioFormat format);
    ~WavWriter();
    void write(const float* samples, std::uint64_t frames);
    void checkpoint();
    void close();
    std::uint64_t frames() const { return frames_; }
private:
    WavWriter(const WavWriter&);
    WavWriter& operator=(const WavWriter&);
    void header();
    std::FILE* file_;
    AudioFormat format_;
    std::uint64_t frames_;
};
}
