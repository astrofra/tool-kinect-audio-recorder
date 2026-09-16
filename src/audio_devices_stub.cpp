#include "recorder/audio.h"
#include <stdexcept>
namespace recorder {
std::vector<AudioDevice> enumerate_audio_devices() { return std::vector<AudioDevice>(); }
std::unique_ptr<AudioSource> make_wasapi_source(const RecordOptions&) {
    throw std::runtime_error("WASAPI is only available on Windows; use --source simulate");
}
}
