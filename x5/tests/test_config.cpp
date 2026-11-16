#include "x5/config.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    fs::path directory = fs::temp_directory_path() /
        ("x5-config-test-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(directory / "settings"); }
    ~Fixture() { fs::remove_all(directory); }
    fs::path write(const std::string& yaml) {
        const auto file = directory / "settings" / "camera.yaml";
        std::ofstream stream(file);
        stream << yaml;
        stream.close();
        return file;
    }
    x5::Config load(const std::string& yaml) { return x5::load_config(write(yaml)); }
};

void rejects(Fixture& fixture, const std::string& yaml, const std::string& key) {
    const auto file = fixture.write(yaml);
    try {
        (void)x5::load_config(file);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        require(message.find(file.string()) != std::string::npos,
                "error must identify YAML file: " + message);
        require(message.find(key) != std::string::npos,
                "error must identify setting " + key + ": " + message);
        return;
    }
    throw std::runtime_error("accepted invalid setting: " + key);
}
}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"defaults and relative output are independent of working directory", [] {
            Fixture f;
            const auto c = f.load("");
            require(c.config_path == f.directory / "settings/camera.yaml", "source path missing");
            require(c.output_directory == f.directory / "outputs", "default output path unresolved");
            require(c.exposure_mode == "auto" && !c.iso && !c.shutter_s && c.ev_bias() == 0,
                    "default exposure invalid");
            require(c.resolution == 960 && c.fps == 30 &&
                    c.bitrate_bps == 8000000 && c.imu && c.preview && c.save,
                    "stream defaults invalid");
        }},
        {"all fields load and effective configuration preserves overrides", [] {
            Fixture f;
            auto c = f.load(R"(camera:
  serial: X5-123
  exposure: {mode: manual, iso: 400, shutter_s: 0.005, ev: 0}
  white_balance_kelvin: 5500
stream: {resolution: 1280, fps: 30, bitrate_bps: 12000000, imu: false}
preview: {enabled: false}
output: {save: false, directory: ../captures}
runtime: {duration_s: 12.5, status_interval_s: 0.5}
)");
            require(c.serial == "X5-123" && c.iso == 400 && c.shutter_s == 0.005 &&
                    c.white_balance_kelvin == 5500 && c.resolution == 1280 &&
                    c.bitrate_bps == 12000000 && !c.imu && !c.preview && !c.save &&
                    c.duration_s == 12.5 && c.status_interval_s == 0.5, "field ignored");
            require(c.output_directory == f.directory / "captures", "relative output unresolved");
            c.duration_s = 9.25;
            const auto yaml = c.to_yaml();
            require(yaml["runtime"]["duration_s"].as<double>() == 9.25, "override omitted");
            require(yaml["output"]["directory"].as<std::string>() == c.output_directory.string(),
                    "effective output not resolved");
            YAML::Emitter emitter;
            emitter << yaml;
            const auto roundtrip = f.load(emitter.c_str());
            require(roundtrip.serial == c.serial && roundtrip.iso == 400 &&
                    roundtrip.shutter_s == 0.005 && roundtrip.duration_s == 9.25 && roundtrip.resolution == c.resolution &&
                    !roundtrip.imu && !roundtrip.preview && !roundtrip.save,
                    "effective configuration loses fields");
        }},
        {"sensor delay defaults conversion and roundtrip", [] {
            Fixture f;
            require(f.load("{}").sensor_delay_ns() == 0, "delay must default to zero");
            auto c = f.load("timing: {sensor_delay_s: 0.120}");
            require(c.sensor_delay_ns() == 120000000, "sensor delay seconds to ns conversion");
            require(c.to_yaml()["timing"]["sensor_delay_s"].as<double>() == 0.120, "delay missing from effective config");
            require(f.load(YAML::Dump(c.to_yaml())).sensor_delay_ns() == 120000000, "delay roundtrip");
            for (const auto& value : {"-0.1", ".nan", ".inf", "1e30", "null", "oops"})
                rejects(f, std::string("timing: {sensor_delay_s: ") + value + "}", "timing.sensor_delay_s");
        }},
        {"supported modes and SDK EV conversion", [] {
            Fixture f;
            auto c = f.load("camera: {exposure: {mode: full_auto, ev: -1.35}}\n");
            require(c.ev_bias() == -27, "EV must use SDK EV times 20");
            (void)f.load("camera: {exposure: {mode: iso_priority, iso: 800}}\n");
            (void)f.load("camera: {exposure: {mode: shutter_priority, shutter_s: 0.01}}\n");
            (void)f.load("camera: {exposure: {mode: auto, iso: null, shutter_s: null}}\n");
            for (const auto& yaml : {
                "camera: {exposure: {mode: magic}}", "camera: {exposure: {mode: manual}}",
                "camera: {exposure: {mode: iso_priority}}",
                "camera: {exposure: {mode: shutter_priority}}",
                "camera: {exposure: {mode: auto, iso: 100}}",
                "camera: {exposure: {mode: full_auto, shutter_s: 0.01}}",
                "camera: {exposure: {mode: iso_priority, iso: 100, shutter_s: 0.01}}",
                "camera: {exposure: {mode: shutter_priority, shutter_s: 0.01, iso: 100}}",
                "camera: {exposure: {mode: manual, iso: 100, shutter_s: 0.01, ev: 1}}",
                "camera: {exposure: {mode: manual, iso: 100, shutter_s: 0.01, ev: 0.0000000001}}",
                "camera: {exposure: {ev: 0.001}}", "camera: {exposure: {ev: 4.05}}",
                "camera: {exposure: {ev: .nan}}",
                "camera: {exposure: {mode: manual, iso: 0, shutter_s: 0.01}}",
                "camera: {exposure: {mode: manual, iso: 100, shutter_s: 0.1}}",
                "camera: {exposure: {mode: manual, iso: 100, shutter_s: -.inf}}"
            }) rejects(f, yaml, "camera.exposure");
        }},
        {"one per-lens resolution configures both lenses", [] {
            Fixture f;
            auto c = f.load("stream: {resolution: 960, fps: 30}");
            require(c.resolution == 960, "per-lens resolution ignored");
            auto effective = c.to_yaml();
            require(effective["stream"]["resolution"].as<int>() == 960, "scalar resolution missing");
            require(!effective["stream"]["width"].IsDefined() && !effective["stream"]["height"].IsDefined(),
                    "effective config still exposes combined width/height");
        }},
        {"only 30fps reaches a valid SDK request", [] {
            Fixture f;
            for (const auto& entry : std::vector<std::pair<std::string, int>>{
                {"stream: {resolution: 960, fps: 30}", 2},
                {"stream: {resolution: 1280, fps: 30}", 1},
                {"stream: {resolution: 1920, fps: 30}", 0}
            }) {
                const auto config = f.load(entry.first);
                require(config.sdk_resolution_enum() == entry.second, "FPS was not mapped to its SDK request");
            }
            for (int side : {960, 1280, 1920}) {
                for (int fps : {15, 20, 24, 25, 48, 50, 60, 100, 120}) {
                    rejects(f, "stream: {resolution: " + std::to_string(side) +
                            ", fps: " + std::to_string(fps) + "}", "stream.fps");
                }
            }
        }},
        {"malformed YAML, unknown keys and wrong types are contextual errors", [] {
            Fixture f;
            for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
                {"unknown: 1", "unknown"}, {"camera: {expsoure: {}}", "camera.expsoure"},
                {"stream: {fps: 30, fps: 20}", "stream.fps"},
                {"camera: {}\ncamera: {}", "camera"},
                {"stream: {widht: 1920}", "stream.widht"},
                {"camera: []", "camera"}, {"camera: {exposure: manual}", "camera.exposure"},
                {"preview: true", "preview"}, {"output: []", "output"},
                {"runtime: {duration_s: nope}", "runtime.duration_s"},
                {"stream: {resolution: 1.5}", "stream.resolution"},
                {"stream: {width: 1440, height: 720}", "stream.resolution"},
                {"camera: {lens: rear}", "camera.lens"},
                {"stream: {bitrate_bps: -1}", "stream.bitrate_bps"},
                {"stream: {bitrate_bps: 4294967296}", "stream.bitrate_bps"},
                {"stream: {imu: maybe}", "stream.imu"},
                {"camera: {serial: []}", "camera.serial"},
                {"runtime: {duration_s: null}", "runtime.duration_s"},
                {"[1, 2]", "root"}, {"camera: [", "YAML"}
            }) rejects(f, entry.first, entry.second);
        }},
        {"stream runtime and white balance validation", [] {
            Fixture f;
            for (const auto& shape : {"[960,30]", "[1280,30]", "[1920,30]"}) {
                const auto values = YAML::Load(shape);
                x5::Config c;
                c.resolution = values[0].as<int>(); c.fps = values[1].as<int>();
                x5::validate_config(c);
            }
            // The SDK lists 1080, but this X5 emits uncropped 2160x1088.
            // Reject it with the measured geometry rather than promising squares.
            rejects(f, "stream: {resolution: 1080}", "2160x1088");
            for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
                {"stream: {resolution: 0}", "stream.resolution"}, {"stream: {resolution: -1}", "stream.resolution"},
                {"stream: {resolution: 721}", "stream.resolution"},
                {"stream: {resolution: 720}", "stream.resolution"},
                {"stream: {fps: 0}", "stream"}, {"stream: {fps: 60}", "stream"},
                {"stream: {bitrate_bps: 0}", "stream.bitrate_bps"},
                {"camera: {white_balance_kelvin: 1999}", "camera.white_balance_kelvin"},
                {"camera: {white_balance_kelvin: 10001}", "camera.white_balance_kelvin"},
                {"runtime: {duration_s: -1}", "runtime.duration_s"},
                {"runtime: {duration_s: .inf}", "runtime.duration_s"},
                {"runtime: {status_interval_s: 0}", "runtime.status_interval_s"},
                {"runtime: {status_interval_s: .nan}", "runtime.status_interval_s"}
            }) rejects(f, entry.first, entry.second);
            (void)f.load("camera: {white_balance_kelvin: 2000}");
            (void)f.load("camera: {white_balance_kelvin: 10000}");
            auto c = f.load("{}");
            c.duration_s = -1;
            bool failed = false;
            try { x5::validate_config(c); } catch (const std::exception&) { failed = true; }
            require(failed, "CLI override bypasses validation");
        }},
        {"absolute output paths and missing source files", [] {
            Fixture f;
            const auto output = (f.directory / "absolute").string();
            const auto c = f.load("output: {directory: '" + output + "'}");
            require(c.output_directory == output, "absolute path was changed");
            bool failed = false;
            try { (void)x5::load_config(f.directory / "missing.yaml"); }
            catch (const std::exception& error) {
                failed = std::string(error.what()).find("missing.yaml") != std::string::npos;
            }
            require(failed, "missing YAML should identify source file");
        }}
    };
    int failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failed << '/' << tests.size() << " configuration tests passed\n";
    return failed ? 1 : 0;
}
