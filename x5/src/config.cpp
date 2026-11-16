#include "x5/config.hpp"
#include <camera/photography_settings.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace x5 {
namespace {
[[noreturn]] void invalid(const Config& config, const std::string& key,
                          const std::string& reason) {
    const auto source = config.config_path.empty() ? "configuration" : config.config_path.string();
    throw std::runtime_error(source + ": " + key + ": " + reason);
}

void check_map(const Config& config, const YAML::Node& node, const std::string& key,
               std::initializer_list<std::string_view> allowed) {
    if (!node.IsMap()) invalid(config, key, "expected a mapping");
    std::set<std::string> seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) invalid(config, key, "expected a string setting name");
        const auto name = entry.first.as<std::string>();
        if (!seen.insert(name).second) {
            invalid(config, key == "root" ? name : key + "." + name, "duplicate setting");
        }
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            invalid(config, key == "root" ? name : key + "." + name, "unknown setting");
        }
    }
}

template <typename T>
void read(const Config& config, const YAML::Node& node, const std::string& name,
          const std::string& prefix, T& target) {
    const auto value = node[name];
    if (!value.IsDefined()) return;
    const auto key = prefix + "." + name;
    if (!value.IsScalar()) invalid(config, key, "expected a scalar value");
    try {
        target = value.as<T>();
    } catch (const YAML::Exception& error) {
        invalid(config, key, std::string("invalid value: ") + error.what());
    }
}

template <typename T>
void read_optional(const Config& config, const YAML::Node& node, const std::string& name,
                   std::optional<T>& target) {
    const auto value = node[name];
    if (!value.IsDefined() || value.IsNull()) return;
    T parsed{};
    read(config, node, name, "camera.exposure", parsed);
    target = parsed;
}

int checked_ev_bias(const Config& config) {
    if (!std::isfinite(config.ev) || config.ev < -4 || config.ev > 4) {
        invalid(config, "camera.exposure.ev", "must be finite and within [-4, 4]");
    }
    const auto scaled = config.ev * 20;
    if (std::abs(scaled - std::round(scaled)) > 1e-8) {
        invalid(config, "camera.exposure.ev", "must be representable in SDK steps of 0.05 EV");
    }
    return static_cast<int>(std::round(scaled));
}
}  // namespace

Config load_config(const std::filesystem::path& path) {
    Config config;
    config.config_path = std::filesystem::absolute(path).lexically_normal();
    YAML::Node root;
    try {
        root = YAML::LoadFile(config.config_path.string());
    } catch (const YAML::Exception& error) {
        invalid(config, "YAML", error.what());
    }
    // yaml-cpp represents an empty document as null; all defaults remain useful.
    if (root.IsNull()) root = YAML::Node(YAML::NodeType::Map);
    check_map(config, root, "root", {"camera", "stream", "preview", "output", "runtime", "timing"});

    if (const auto camera = root["camera"]; camera.IsDefined()) {
        check_map(config, camera, "camera", {"serial", "exposure", "white_balance_kelvin"});
        read(config, camera, "serial", "camera", config.serial);
        read(config, camera, "white_balance_kelvin", "camera", config.white_balance_kelvin);
        if (const auto exposure = camera["exposure"]; exposure.IsDefined()) {
            check_map(config, exposure, "camera.exposure", {"mode", "iso", "shutter_s", "ev"});
            read(config, exposure, "mode", "camera.exposure", config.exposure_mode);
            read_optional(config, exposure, "iso", config.iso);
            read_optional(config, exposure, "shutter_s", config.shutter_s);
            read(config, exposure, "ev", "camera.exposure", config.ev);
        }
    }
    if (const auto stream = root["stream"]; stream.IsDefined()) {
        if (stream.IsMap() && (stream["width"].IsDefined() || stream["height"].IsDefined())) {
            invalid(config, "stream.resolution", "replace width/height with one per-lens square side, e.g. resolution: 960 for BOTH lenses");
        }
        check_map(config, stream, "stream", {"resolution", "fps", "bitrate_bps", "imu"});
        read(config, stream, "resolution", "stream", config.resolution);
        read(config, stream, "fps", "stream", config.fps);
        long long bitrate = config.bitrate_bps;
        read(config, stream, "bitrate_bps", "stream", bitrate);
        if (bitrate <= 0 || bitrate > std::numeric_limits<std::uint32_t>::max()) {
            invalid(config, "stream.bitrate_bps", "must be a positive 32-bit unsigned integer");
        }
        config.bitrate_bps = static_cast<std::uint32_t>(bitrate);
        read(config, stream, "imu", "stream", config.imu);
    }
    if (const auto preview = root["preview"]; preview.IsDefined()) {
        check_map(config, preview, "preview", {"enabled"});
        read(config, preview, "enabled", "preview", config.preview);
    }
    if (const auto output = root["output"]; output.IsDefined()) {
        check_map(config, output, "output", {"save", "directory"});
        read(config, output, "save", "output", config.save);
        auto directory = config.output_directory.string();
        read(config, output, "directory", "output", directory);
        config.output_directory = directory;
    }
    if (const auto runtime = root["runtime"]; runtime.IsDefined()) {
        check_map(config, runtime, "runtime", {"duration_s", "status_interval_s"});
        read(config, runtime, "duration_s", "runtime", config.duration_s);
        read(config, runtime, "status_interval_s", "runtime", config.status_interval_s);
    }
    if (const auto timing = root["timing"]; timing.IsDefined()) {
        check_map(config, timing, "timing", {"sensor_delay_s"});
        read(config, timing, "sensor_delay_s", "timing", config.sensor_delay_s);
    }
    if (config.output_directory.empty()) invalid(config, "output.directory", "must not be empty");
    if (config.output_directory.is_relative()) {
        config.output_directory = config.config_path.parent_path() / config.output_directory;
    }
    config.output_directory = config.output_directory.lexically_normal();
    validate_config(config);
    return config;
}

void validate_config(const Config& config) {
    (void)config.sensor_delay_ns();
    const bool automatic = config.exposure_mode == "auto" || config.exposure_mode == "full_auto";
    const bool manual = config.exposure_mode == "manual";
    const bool iso_priority = config.exposure_mode == "iso_priority";
    const bool shutter_priority = config.exposure_mode == "shutter_priority";
    if (!automatic && !manual && !iso_priority && !shutter_priority) {
        invalid(config, "camera.exposure.mode",
                "expected auto, full_auto, manual, iso_priority or shutter_priority");
    }
    if ((automatic && (config.iso || config.shutter_s)) ||
        (manual && (!config.iso || !config.shutter_s)) ||
        (iso_priority && (!config.iso || config.shutter_s)) ||
        (shutter_priority && (!config.shutter_s || config.iso))) {
        invalid(config, "camera.exposure",
                "auto/full_auto require null ISO and shutter; manual requires both; "
                "iso_priority requires only ISO; shutter_priority requires only shutter_s");
    }
    (void)checked_ev_bias(config);
    if (!automatic && config.ev != 0) {
        invalid(config, "camera.exposure.ev", "nonzero EV applies only to auto/full_auto");
    }
    if (config.iso && *config.iso <= 0) {
        invalid(config, "camera.exposure.iso", "must be positive; device capabilities checked at startup");
    }
    (void)config.sdk_resolution_enum();
    if (config.bitrate_bps == 0) invalid(config, "stream.bitrate_bps", "must be positive");
    if (config.shutter_s && (!std::isfinite(*config.shutter_s) || *config.shutter_s <= 0 ||
                              *config.shutter_s > 1.0 / config.fps)) {
        invalid(config, "camera.exposure.shutter_s", "must be finite, positive and at most 1/fps seconds");
    }
    if (config.white_balance_kelvin != 0 &&
        (config.white_balance_kelvin < 2000 || config.white_balance_kelvin > 10000)) {
        invalid(config, "camera.white_balance_kelvin", "must be 0 (auto), or within 2000..10000");
    }
    if (!std::isfinite(config.duration_s) || config.duration_s < 0) {
        invalid(config, "runtime.duration_s", "must be finite and nonnegative");
    }
    if (!std::isfinite(config.status_interval_s) || config.status_interval_s <= 0) {
        invalid(config, "runtime.status_interval_s", "must be finite and positive");
    }
    if (config.output_directory.empty()) invalid(config, "output.directory", "must not be empty");
}

int Config::sdk_resolution_enum() const {
    using namespace ins_camera;
    // Enforce the fixed FPS at the shared mapping boundary used by both SDK calls.
    if (fps != 30) invalid(*this, "stream.fps", "only 30 fps is enabled");
    struct Mode { int side; VideoResolution value; };
    static constexpr Mode modes[] = {
        {960, RES_1920_960P30}, {1280, RES_2560_1280P30},
        {1920, RES_3840_1920P30}
    };
    for (const auto& mode : modes) {
        if (mode.side == resolution) return static_cast<int>(mode.value);
    }
    if (resolution == 1080) {
        invalid(*this, "stream.resolution", "SDK lists 1080, but X5 v1.13.21 emits 2160x1088 without crop metadata; exact square sizes remain 960/1280/1920");
    }
    invalid(*this, "stream.resolution", "mapped per-lens sizes are 960/1280/1920 at 30 fps");
}

int Config::ev_bias() const { return checked_ev_bias(*this); }

int64_t Config::sensor_delay_ns() const {
    const long double ns = std::round(static_cast<long double>(sensor_delay_s) * 1e9L);
    if (!std::isfinite(sensor_delay_s) || sensor_delay_s < 0 || ns > std::numeric_limits<int64_t>::max())
        invalid(*this, "timing.sensor_delay_s", "must be finite, nonnegative seconds representable as int64 nanoseconds");
    return static_cast<int64_t>(ns);
}

YAML::Node Config::to_yaml() const {
    YAML::Node root;
    root["camera"]["serial"] = serial;
    root["camera"]["exposure"]["mode"] = exposure_mode;
    root["camera"]["exposure"]["iso"] = iso ? YAML::Node(*iso) : YAML::Node(YAML::NodeType::Null);
    root["camera"]["exposure"]["shutter_s"] = shutter_s ? YAML::Node(*shutter_s) : YAML::Node(YAML::NodeType::Null);
    root["camera"]["exposure"]["ev"] = ev;
    root["camera"]["white_balance_kelvin"] = white_balance_kelvin;
    root["stream"]["resolution"] = resolution;
    root["stream"]["fps"] = fps;
    root["stream"]["bitrate_bps"] = bitrate_bps;
    root["stream"]["imu"] = imu;
    root["preview"]["enabled"] = preview;
    root["output"]["save"] = save;
    root["output"]["directory"] = output_directory.string();
    root["runtime"]["duration_s"] = duration_s;
    root["runtime"]["status_interval_s"] = status_interval_s;
    root["timing"]["sensor_delay_s"] = sensor_delay_s;
    return root;
}
}  // namespace x5
