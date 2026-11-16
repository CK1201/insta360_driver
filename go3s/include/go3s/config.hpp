#pragma once
#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
namespace go3s {
using Json = nlohmann::json;
struct Config {
    std::filesystem::path source;
    std::string adb_serial, camera_serial, exposure_mode = "auto";
    int port = 18765;
    std::optional<int> iso;
    std::optional<double> shutter_s;
    double ev = 0;
    int white_balance_kelvin = 0;
    bool imu = true, preview = true, save = false;
    std::filesystem::path output_directory = "../outputs";
    double duration_s = 0, status_interval_s = 1;
    double sensor_delay_s = 0;
    int64_t sensor_delay_ns() const;
    Json start_request() const;
    std::string effective_yaml() const;
};
Config load_config(const std::filesystem::path& path);
void validate_config(const Config& c);
}
