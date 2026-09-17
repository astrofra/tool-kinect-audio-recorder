#include "recorder/depth.h"
#include <stdexcept>

namespace recorder {
bool kinect_depth_supported() { return false; }
std::unique_ptr<DepthSource> make_kinect_depth_source() {
    throw std::runtime_error("Kinect v2 capture is unavailable in this build. Install Microsoft Kinect for Windows SDK 2.0 and rebuild for Windows x64 with RECORDER_ENABLE_KINECT=ON (KINECTSDK20_DIR).");
}
}
