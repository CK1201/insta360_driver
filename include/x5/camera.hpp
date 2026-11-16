#pragma once
#include "x5/config.hpp"
#include "x5/capture.hpp"
#include <camera/camera.h>

namespace x5 {
class CameraSession {
public:
    explicit CameraSession(const Config& config);
    ~CameraSession();
    CameraSession(const CameraSession&) = delete;
    CameraSession& operator=(const CameraSession&) = delete;
    YAML::Node configure(const Config& config);
    YAML::Node identity() const;
    Codec codec() const;
    void start(const Config& config, std::shared_ptr<ins_camera::StreamDelegate>& delegate);
    void stop();
    ins_camera::Camera& camera() { return *camera_; }
private:
    std::shared_ptr<ins_camera::Camera> camera_;
    std::string model_, serial_, firmware_;
    bool streaming_ = false;
};
}
