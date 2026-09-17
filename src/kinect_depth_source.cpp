#include "recorder/depth.h"
#include "recorder/platform.h"
#include <windows.h>
#include <Kinect.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace recorder {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream s;
        s << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result)
          << "). Check Kinect v2 power, USB 3.0 and Microsoft SDK 2.0 drivers.";
        throw std::runtime_error(s.str());
    }
}
class KinectDepthSource : public DepthSource {
public:
    KinectDepthSource() : library_(0), com_(false), index_(0) {}
    ~KinectDepthSource() {
        reader_.Reset();
        if (sensor_) sensor_->Close();
        sensor_.Reset();
        if (library_) FreeLibrary(library_);
        if (com_) CoUninitialize();
    }
    void open(const std::atomic<bool>& stop) {
        check(CoInitializeEx(0, COINIT_MULTITHREADED), "Initialize Kinect COM"); com_ = true;
        // Load only the installed runtime, never an arbitrary DLL from the current directory.
        library_ = LoadLibraryExW(L"Kinect20.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) throw std::runtime_error("Kinect20.dll is unavailable. Install Microsoft Kinect for Windows SDK 2.0/runtime and its drivers, then reconnect the powered Kinect v2 to USB 3.0.");
        typedef HRESULT (WINAPI *GetSensor)(IKinectSensor**);
        const GetSensor get_sensor = reinterpret_cast<GetSensor>(GetProcAddress(library_, "GetDefaultKinectSensor"));
        if (!get_sensor) throw std::runtime_error("Installed Kinect20.dll does not expose GetDefaultKinectSensor");
        check(get_sensor(sensor_.GetAddressOf()), "Get Kinect v2 sensor");
        if (!sensor_) throw std::runtime_error("No Kinect v2 sensor found. Check power and USB 3.0.");
        check(sensor_->Open(), "Open Kinect v2");
        ComPtr<IDepthFrameSource> source;
        check(sensor_->get_DepthFrameSource(source.GetAddressOf()), "Get Kinect depth source");
        ComPtr<IFrameDescription> description;
        check(source->get_FrameDescription(description.GetAddressOf()), "Get Kinect depth format");
        int width = 0, height = 0;
        check(description->get_Width(&width), "Get depth width");
        check(description->get_Height(&height), "Get depth height");
        if (width != DepthWidth || height != DepthHeight) throw std::runtime_error("Unsupported Kinect depth resolution");
        check(source->OpenReader(reader_.GetAddressOf()), "Open Kinect depth reader");
        // Warm up before the take starts. All waits are cancellable and bounded.
        ComPtr<ICoordinateMapper> mapper;
        check(sensor_->get_CoordinateMapper(mapper.GetAddressOf()), "Get Kinect calibration mapper");
        CameraIntrinsics intrinsics = {};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!stop) {
            ComPtr<IDepthFrame> warmup;
            if (!acquire(warmup, stop, deadline)) return;
            const HRESULT result = mapper->GetDepthCameraIntrinsics(&intrinsics);
            // The first frame can precede calibration readiness after reopening the sensor.
            if (SUCCEEDED(result) && intrinsics.FocalLengthX > 0 && intrinsics.FocalLengthY > 0) break;
            if (FAILED(result) && result != E_PENDING) check(result, "Read depth intrinsics");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Kinect calibration did not become ready within 15 seconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (stop) return;
        WCHAR id[256] = {};
        check(sensor_->get_UniqueKinectId(256, id), "Read Kinect device ID");
        id_ = to_utf8(id);
        const float values[] = { intrinsics.FocalLengthX, intrinsics.FocalLengthY, intrinsics.PrincipalPointX,
            intrinsics.PrincipalPointY, intrinsics.RadialDistortionSecondOrder,
            intrinsics.RadialDistortionFourthOrder, intrinsics.RadialDistortionSixthOrder };
        for (unsigned i = 0; i < 7; ++i) if (!std::isfinite(values[i])) throw std::runtime_error("Invalid Kinect calibration");
        if (values[0] <= 0 || values[1] <= 0) throw std::runtime_error("Kinect calibration is not ready");
        std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(9);
        s << "{\"model\":\"kinect-sdk-2.0-depth-intrinsics\",\"width\":512,\"height\":424,"
          << "\"focal_length_x\":" << values[0] << ",\"focal_length_y\":" << values[1]
          << ",\"principal_point_x\":" << values[2] << ",\"principal_point_y\":" << values[3]
          << ",\"radial_distortion_second_order\":" << values[4]
          << ",\"radial_distortion_fourth_order\":" << values[5]
          << ",\"radial_distortion_sixth_order\":" << values[6]
          << ",\"grid\":\"native-sdk-depth-raster\"}";
        calibration_ = s.str();
    }
    std::string device_id() const { return id_; }
    std::string calibration_json() const { return calibration_; }
    bool read(DepthFrame& output, const std::atomic<bool>& stop) {
        ComPtr<IDepthFrame> frame;
        if (!acquire(frame, stop, std::chrono::steady_clock::now() + std::chrono::seconds(5))) return false;
        output.receipt_ticks = clock_ticks(); // Observe receipt before copying/processing.
        TIMESPAN timestamp = 0;
        check(frame->get_RelativeTime(&timestamp), "Read Kinect depth timestamp");
        UINT16 min_distance = 0, max_distance = 0;
        check(frame->get_DepthMinReliableDistance(&min_distance), "Read minimum reliable depth");
        check(frame->get_DepthMaxReliableDistance(&max_distance), "Read maximum reliable depth");
        output.millimetres.resize(DepthWidth * DepthHeight);
        check(frame->CopyFrameDataToArray(static_cast<UINT>(output.millimetres.size()), output.millimetres.data()), "Copy Kinect depth frame");
        output.index = index_++;
        output.relative_time_100ns = timestamp;
        output.min_reliable_mm = min_distance; output.max_reliable_mm = max_distance;
        return true;
    }
private:
    bool acquire(ComPtr<IDepthFrame>& frame, const std::atomic<bool>& stop, std::chrono::steady_clock::time_point deadline) {
        while (!stop) {
            const HRESULT result = reader_->AcquireLatestFrame(frame.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result) && frame) return true;
            if (FAILED(result) && result != E_PENDING) check(result, "Acquire Kinect depth frame");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("No Kinect depth frame arrived before timeout. Check sensor power, USB 3.0, drivers and other Kinect applications.");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    HMODULE library_;
    bool com_;
    ComPtr<IKinectSensor> sensor_;
    ComPtr<IDepthFrameReader> reader_;
    std::uint64_t index_;
    std::string id_, calibration_;
};
}
bool kinect_depth_supported() { return true; }
std::unique_ptr<DepthSource> make_kinect_depth_source() {
    return std::unique_ptr<DepthSource>(new KinectDepthSource());
}
}
