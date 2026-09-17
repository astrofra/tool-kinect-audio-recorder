#include "recorder/audio.h"
#include "recorder/platform.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace recorder {
namespace {
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream msg;
        msg << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result)
            << "). Check the input device, Windows microphone permissions, and its shared-mode format.";
        throw std::runtime_error(msg.str());
    }
}
class ComApartment {
public:
    ComApartment() { check(CoInitializeEx(0, COINIT_MULTITHREADED), "COM initialization"); }
    ~ComApartment() { CoUninitialize(); }
};
template<class T> class ComPtr {
public:
    ComPtr() : p_(0) {}
    ~ComPtr() { if (p_) p_->Release(); }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { return &p_; }
private:
    ComPtr(const ComPtr&); ComPtr& operator=(const ComPtr&);
    T* p_;
};
struct TaskString { LPWSTR p; TaskString() : p(0) {} ~TaskString() { CoTaskMemFree(p); } };
struct MixFormat { WAVEFORMATEX* p; MixFormat() : p(0) {} ~MixFormat() { CoTaskMemFree(p); } };
struct Event { HANDLE h; Event() : h(0) {} ~Event() { if (h) CloseHandle(h); } };
struct Property { PROPVARIANT v; Property() { PropVariantInit(&v); } ~Property() { PropVariantClear(&v); } };
AudioDevice describe(IMMDevice* device) {
    TaskString id;
    check(device->GetId(&id.p), "Read endpoint ID");
    AudioDevice result; result.id = to_utf8(id.p); result.name = result.id;
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
        Property prop;
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &prop.v)) && prop.v.vt == VT_LPWSTR)
            result.name = to_utf8(prop.v.pwszVal);
    }
    return result;
}
class WasapiSource : public AudioSource {
public:
    explicit WasapiSource(const RecordOptions& options) : requested_id_(options.device_id), capacity_(0),
        bits_(0), is_float_(false), started_(false), last_packet_time_(0) {}
    ~WasapiSource() { if (started_) client_->Stop(); }
    AudioFormat open() {
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.put())), "Create audio enumerator");
        if (requested_id_.empty()) check(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device_.put()), "Find default microphone");
        else check(enumerator->GetDevice(from_utf8(requested_id_).c_str(), device_.put()), "Find selected microphone");
        info_ = describe(device_.get());
        check(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, reinterpret_cast<void**>(client_.put())), "Open microphone");
        MixFormat mix;
        check(client_->GetMixFormat(&mix.p), "Read audio mix format");
        WAVEFORMATEX* w = mix.p;
        format_ = AudioFormat(w->nSamplesPerSec, w->nChannels);
        bits_ = w->wBitsPerSample;
        unsigned type = w->wFormatTag;
        if (type == WAVE_FORMAT_EXTENSIBLE) {
            if (w->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) throw std::runtime_error("Incomplete extensible audio format");
            const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
            format_.channel_mask = ext->dwChannelMask;
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) type = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) type = WAVE_FORMAT_PCM;
        }
        is_float_ = type == WAVE_FORMAT_IEEE_FLOAT;
        if (format_.channels < 1 || format_.channels > MaxAudioChannels || format_.sample_rate < 8000 || format_.sample_rate > 192000 ||
            (is_float_ ? bits_ != 32 : (type != WAVE_FORMAT_PCM || (bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32))) ||
            w->nBlockAlign != format_.channels * bits_ / 8) {
            std::ostringstream message;
            message << "Unsupported WASAPI mix format for " << info_.name << ": " << format_.sample_rate
                << " Hz, " << format_.channels << " channels, " << bits_ << " bits, format tag " << type
                << ", block alignment " << w->nBlockAlign << ". Supported: 1.." << MaxAudioChannels
                << " channels, 8000..192000 Hz, PCM 8/16/24/32 or float32.";
            throw std::runtime_error(message.str());
        }
        check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, w, 0), "Initialize shared capture");
        event_.h = CreateEventW(0, FALSE, FALSE, 0);
        if (!event_.h) throw std::runtime_error("Cannot create audio event");
        check(client_->SetEventHandle(event_.h), "Set audio event");
        check(client_->GetBufferSize(&capacity_), "Read audio buffer size");
        check(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.put())), "Create audio capture service");
        return format_;
    }
    unsigned max_packet_frames() const { return capacity_; }
    std::string name() const { return info_.name; }
    std::string device_id() const { return info_.id; }
    void start() { check(client_->Start(), "Start microphone"); started_ = true; last_packet_time_ = clock_100ns(); }
    bool read(AudioPacket& packet, const std::atomic<bool>& stopped) {
        while (!stopped) {
            UINT32 available = 0;
            check(capture_->GetNextPacketSize(&available), "Poll microphone");
            if (!available) {
                if (clock_100ns() - last_packet_time_ > 50000000ULL) throw std::runtime_error("Microphone produced no packets for five seconds");
                const DWORD result = WaitForSingleObject(event_.h, 50);
                if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) throw std::runtime_error("Audio event wait failed");
                continue;
            }
            BYTE* data = 0; UINT32 frames = 0; DWORD flags = 0; UINT64 position = 0, qpc = 0;
            HRESULT result = capture_->GetBuffer(&data, &frames, &flags, &position, &qpc);
            if (result == AUDCLNT_S_BUFFER_EMPTY) continue;
            check(result, "Read microphone packet");
            try {
                packet.receipt_ticks = clock_ticks();
                if (!frames || frames > capacity_) throw std::runtime_error("Unexpected WASAPI packet size");
                packet.device_frame = position; packet.timestamp_100ns = qpc; packet.flags = flags;
                packet.samples.resize(static_cast<std::size_t>(frames) * format_.channels);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(packet.samples.begin(), packet.samples.end(), 0.0f);
                else if (is_float_) std::memcpy(packet.samples.data(), data, packet.samples.size() * sizeof(float));
                else {
                    const unsigned stride = bits_ / 8;
                    for (std::size_t i = 0; i < packet.samples.size(); ++i) {
                        std::uint32_t raw = 0;
                        for (unsigned b = 0; b < stride; ++b) raw |= static_cast<std::uint32_t>(data[i * stride + b]) << (8 * b);
                        if (bits_ == 8) packet.samples[i] = (static_cast<int>(raw) - 128) / 128.0f;
                        else {
                            std::int64_t value = raw;
                            if (raw & (1U << (bits_ - 1))) value -= (1LL << bits_);
                            packet.samples[i] = static_cast<float>(value / std::ldexp(1.0, bits_ - 1));
                        }
                    }
                }
            } catch (...) { capture_->ReleaseBuffer(frames); throw; }
            check(capture_->ReleaseBuffer(frames), "Release microphone packet");
            last_packet_time_ = clock_100ns();
            return true;
        }
        return false;
    }
    void stop() { if (started_) { started_ = false; check(client_->Stop(), "Stop microphone"); } }
private:
    ComApartment apartment_; // Constructed and destroyed on the capture thread.
    std::string requested_id_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Event event_;
    AudioDevice info_;
    AudioFormat format_;
    UINT32 capacity_;
    unsigned bits_;
    bool is_float_, started_;
    std::uint64_t last_packet_time_;
};
}
std::vector<AudioDevice> enumerate_audio_devices() {
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.put())), "Create audio enumerator");
    check(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.put()), "List microphones");
    UINT count = 0; check(devices->GetCount(&count), "Count microphones");
    std::vector<AudioDevice> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device; check(devices->Item(i, device.put()), "Read microphone entry");
        result.push_back(describe(device.get()));
    }
    return result;
}
std::unique_ptr<AudioSource> make_wasapi_source(const RecordOptions& options) {
    return std::unique_ptr<AudioSource>(new WasapiSource(options));
}
}
