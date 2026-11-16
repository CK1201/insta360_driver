#include "go3s/config.hpp"
#include <yaml-cpp/yaml.h>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
namespace go3s {
namespace {
void keys(const YAML::Node& n, const std::set<std::string>& allowed, const std::string& path) {
    if (!n || n.IsNull()) return;
    if (!n.IsMap()) throw std::runtime_error(path + " must be a map");
    std::set<std::string> seen;
    for (const auto& item : n) {
        auto key = item.first.as<std::string>();
        if (!allowed.count(key)) throw std::runtime_error("unknown configuration key: " + path + key);
        if (!seen.insert(key).second) throw std::runtime_error("duplicate configuration key: " + path + key);
    }
}
template<class T> void read(const YAML::Node& n, const char* key, T& value) { if (n && n.IsMap() && n[key]) value = n[key].as<T>(); }
}
void validate_config(const Config& c) {
    (void)c.sensor_delay_ns();
    if (c.port < 1024 || c.port > 65535) throw std::runtime_error("bridge.port must be in [1024,65535]");
    const std::set<std::string> modes = {"auto", "manual", "iso_priority", "shutter_priority"};
    if (!modes.count(c.exposure_mode)) throw std::runtime_error("camera.exposure.mode is unsupported");
    const bool needs_iso = c.exposure_mode == "manual" || c.exposure_mode == "iso_priority";
    const bool needs_shutter = c.exposure_mode == "manual" || c.exposure_mode == "shutter_priority";
    if (needs_iso != c.iso.has_value()) throw std::runtime_error("iso is required only for manual/iso_priority");
    if (needs_shutter != c.shutter_s.has_value()) throw std::runtime_error("shutter_s is required only for manual/shutter_priority");
    if (c.iso && *c.iso <= 0) throw std::runtime_error("iso must be positive");
    if (c.shutter_s && (!std::isfinite(*c.shutter_s) || *c.shutter_s <= 0)) throw std::runtime_error("shutter_s must be finite positive seconds");
    if (!std::isfinite(c.ev)) throw std::runtime_error("ev must be finite");
    if (c.white_balance_kelvin < 0) throw std::runtime_error("white_balance_kelvin must be 0 or positive");
    if (!std::isfinite(c.duration_s) || c.duration_s < 0) throw std::runtime_error("duration_s must be finite and nonnegative");
    if (!std::isfinite(c.status_interval_s) || c.status_interval_s < 0.1 || c.status_interval_s > 60) throw std::runtime_error("status_interval_s must be in [0.1,60] seconds");
    if (c.output_directory.empty()) throw std::runtime_error("output.directory must not be empty");
}
Config load_config(const std::filesystem::path& path) {
    Config c;
    c.source = std::filesystem::absolute(path);
    const auto n = YAML::LoadFile(c.source.string());
    keys(n, {"bridge", "camera", "stream", "preview", "output", "runtime", "timing"}, "");
    keys(n["bridge"], {"adb_serial", "port"}, "bridge.");
    keys(n["camera"], {"serial", "exposure", "white_balance_kelvin"}, "camera.");
    const auto camera = n["camera"];
    const auto exposure = camera ? camera["exposure"] : YAML::Node();
    keys(exposure, {"mode", "iso", "shutter_s", "ev"}, "camera.exposure.");
    keys(n["stream"], {"imu"}, "stream.");
    keys(n["preview"], {"enabled"}, "preview.");
    keys(n["output"], {"save", "directory"}, "output.");
    keys(n["runtime"], {"duration_s", "status_interval_s"}, "runtime.");
    keys(n["timing"], {"sensor_delay_s"}, "timing.");
    try { read(n["timing"], "sensor_delay_s", c.sensor_delay_s); }
    catch(const YAML::Exception& e) { throw std::runtime_error(std::string("timing.sensor_delay_s: ")+e.what()); }
    read(n["bridge"], "adb_serial", c.adb_serial);
    read(n["bridge"], "port", c.port);
    read(camera, "serial", c.camera_serial);
    read(camera, "white_balance_kelvin", c.white_balance_kelvin);
    read(exposure, "mode", c.exposure_mode); read(exposure, "ev", c.ev);
    if (exposure && exposure["iso"] && !exposure["iso"].IsNull()) c.iso = exposure["iso"].as<int>();
    if (exposure && exposure["shutter_s"] && !exposure["shutter_s"].IsNull()) c.shutter_s = exposure["shutter_s"].as<double>();
    read(n["stream"], "imu", c.imu); read(n["preview"], "enabled", c.preview);
    read(n["output"], "save", c.save);
    std::string output = c.output_directory.string(); read(n["output"], "directory", output); c.output_directory = output;
    if (c.output_directory.is_relative()) c.output_directory = (c.source.parent_path() / c.output_directory).lexically_normal();
    read(n["runtime"], "duration_s", c.duration_s); read(n["runtime"], "status_interval_s", c.status_interval_s);
    validate_config(c); return c;
}
Json Config::start_request() const {
    return {{"v",1},{"type","start"},{"camera",{{"serial",camera_serial},{"exposure",{{"mode",exposure_mode},
        {"iso",iso ? Json(*iso) : Json(nullptr)},{"shutter_s",shutter_s ? Json(*shutter_s) : Json(nullptr)},{"ev",ev}}},
        {"white_balance_kelvin",white_balance_kelvin}}},{"stream",{{"imu",imu}}},{"runtime",{{"status_interval_s",status_interval_s}}}};
}
int64_t Config::sensor_delay_ns() const {
    const long double ns=std::round(static_cast<long double>(sensor_delay_s)*1e9L);
    if(!std::isfinite(sensor_delay_s) || sensor_delay_s<0 || ns>std::numeric_limits<int64_t>::max())
        throw std::runtime_error("timing.sensor_delay_s must be finite, nonnegative seconds representable as int64 nanoseconds");
    return static_cast<int64_t>(ns);
}
std::string Config::effective_yaml() const {
    // JSON is a strict YAML subset and avoids ambiguous numeric-looking labels.
    Json j = {{"bridge",{{"adb_serial",adb_serial},{"port",port}}},{"camera",start_request()["camera"]},
        {"stream",{{"imu",imu}}},{"preview",{{"enabled",preview}}},
        {"output",{{"save",save},{"directory",output_directory.string()}}},
        {"runtime",{{"duration_s",duration_s},{"status_interval_s",status_interval_s}}},
        {"timing",{{"sensor_delay_s",sensor_delay_s}}}};
    return j.dump(2) + "\n";
}
}
