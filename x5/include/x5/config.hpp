#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>

namespace x5 {

struct Config {
    std::filesystem::path config_path;
    std::filesystem::path output_directory = "../outputs";
    std::string serial;
    std::string exposure_mode = "auto";
    std::optional<int> iso;
    std::optional<double> shutter_s;
    double ev = 0;
    int white_balance_kelvin = 0;
    int resolution = 960; // Native square side of EACH of the two lenses.
    int fps = 30;
    std::uint32_t bitrate_bps = 8000000;
    bool imu = true;
    bool preview = true;
    bool save = true;
    double duration_s = 0;
    double status_interval_s = 1;
    double sensor_delay_s = 0;

    int64_t sensor_delay_ns() const;
    int ev_bias() const;
    int sdk_resolution_enum() const;
    YAML::Node to_yaml() const;
};

Config load_config(const std::filesystem::path& path);
void validate_config(const Config& config);

}  // namespace x5
