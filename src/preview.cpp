#include "x5/preview.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace x5 {
namespace {
const cv::Scalar text_color(225, 230, 235), muted(155, 165, 175), accent(160, 220, 90);
std::string fixed(double value, int precision = 2, bool sign = false) {
    std::ostringstream out;
    if (sign) out << std::showpos;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
void text(cv::Mat& image, const std::string& value, int x, int y,
          double scale = 0.52, cv::Scalar color = text_color, int width = 450) {
    // Shorten the font, not the value, when a configured filename is long.
    int baseline = 0;
    int pixels = cv::getTextSize(value, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline).width;
    if (pixels > width) scale *= static_cast<double>(width) / pixels;
    cv::putText(image, value, {x, y}, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}
std::string rate(const std::optional<double>& value, const char* unit) {
    return value ? fixed(*value, 1) + unit : std::string("--") + unit;
}
std::string iso(int value) { return value == 0 ? "Auto" : std::to_string(value); }
std::string shutter(double value) { return value == 0 ? "Auto" : fixed(value, 6); }
std::string wb(int value) { return value == 0 ? "Auto" : std::to_string(value); }
void row(cv::Mat& image, const std::string& label, const std::string& value, int y) {
    text(image, label, 984, y, 0.48, muted, 160);
    text(image, value, 1150, y, 0.49, text_color, 270);
}
}
cv::Mat render_preview(const cv::Mat& frame, int stream_index,
                       const PreviewContext& context, const PreviewLive& live,
                       int64_t now_ns) {
    cv::Mat image(820, 1440, CV_8UC3, cv::Scalar(23, 23, 23));
    cv::rectangle(image, {960, 0, 480, 820}, cv::Scalar(34, 34, 34), cv::FILLED);
    const auto& config = context.config;
    const auto& stats = live.capture;
    text(image, "X5 LIVE  /  stream " + std::to_string(stream_index), 16, 30, 0.66, accent, 460);
    text(image, rate(live.video_fps, " fps (total)") + "  |  " + fixed(live.elapsed_s, 1) + " s",
         530, 30, 0.54, text_color, 415);

    if (!frame.empty()) {
        double scale = std::min(936.0 / frame.cols, 468.0 / frame.rows);
        cv::Size size(std::max(1, static_cast<int>(frame.cols * scale)),
                      std::max(1, static_cast<int>(frame.rows * scale)));
        cv::Rect target(12 + (936 - size.width) / 2, 52 + (468 - size.height) / 2,
                        size.width, size.height);
        cv::resize(frame, image(target), size, 0, 0, cv::INTER_AREA);
    }

    const bool have_imu = config.imu && stats.latest_imu.has_value();
    std::string imu_title = config.imu ? "IMU  /  latest raw sample" : "IMU  /  disabled in YAML";
    text(image, imu_title, 16, 552, 0.61, accent, 465);
    std::string imu_status = rate(live.imu_hz, " samples/s");
    if (have_imu) imu_status += "  |  age " + fixed(std::max<int64_t>(0, now_ns - stats.latest_imu_received_ns) / 1e6, 1) + " ms";
    else imu_status += "  |  no sample";
    text(image, imu_status, 500, 552, 0.50, muted, 445);
    std::array<double, 6> values{};
    if (have_imu) {
        const auto& imu = *stats.latest_imu;
        values = {imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz};
    }
    const char* labels[] = {"ax", "ay", "az", "gx", "gy", "gz"};
    for (int i = 0; i < 6; ++i) {
        int x = 12 + i % 3 * 312, y = 570 + i / 3 * 88;
        cv::rectangle(image, {x, y, 300, 78}, cv::Scalar(44, 44, 44), cv::FILLED);
        text(image, labels[i], x + 12, y + 26, 0.59, muted, 55);
        text(image, have_imu ? fixed(values[i], 4, true) : "--", x + 78, y + 51, 0.9, text_color, 208);
    }
    text(image, "Device timestamp (raw ticks): " + (have_imu ? std::to_string(stats.latest_imu->timestamp) : "--"),
         16, 774, 0.51, text_color, 700);
    text(image, "Raw IMU units / axes are not calibrated. Host age is callback-arrival age.",
         16, 802, 0.44, muted, 930);

    text(image, "PARAMETERS", 984, 30, 0.68, accent);
    text(image, context.identity["model"].as<std::string>() + "  |  " +
         context.identity["firmware"].as<std::string>(), 984, 58, 0.48, muted);
    text(image, "Serial: " + context.identity["serial"].as<std::string>(), 984, 80, 0.46, muted);
    text(image, "YAML", 1100, 108, 0.45, accent, 110);
    text(image, "Read back", 1235, 108, 0.45, accent, 185);
    const auto exposure = context.readback["exposure"];
    struct Setting { std::string name, requested, applied; };
    const Setting settings[] = {
        {"Mode", config.exposure_mode, exposure["mode"].as<std::string>()},
        {"ISO", config.iso ? std::to_string(*config.iso) : "Auto", iso(exposure["iso"].as<int>())},
        {"Shutter [s]", config.shutter_s ? shutter(*config.shutter_s) : "Auto", shutter(exposure["shutter_s"].as<double>())},
        {"EV", fixed(config.ev), fixed(exposure["ev"].as<double>())},
        {"WB [K]", wb(config.white_balance_kelvin), wb(context.readback["white_balance_kelvin"].as<int>())}
    };
    int y = 136;
    for (const auto& setting : settings) {
        text(image, setting.name, 984, y, 0.47, muted, 110);
        text(image, setting.requested, 1100, y, 0.46, text_color, 125);
        text(image, setting.applied, 1235, y, 0.46, text_color, 185);
        y += 27;
    }
    text(image, "Startup readback; per-frame ISO / WB are not available.", 984, 274, 0.40, muted, 440);

    text(image, "STREAM / RUN", 984, 310, 0.61, accent);
    row(image, "Request / lens", std::to_string(config.resolution) + "x" + std::to_string(config.resolution) + " @ " + std::to_string(config.fps), 340);
    std::string actual = std::to_string(frame.cols) + "x" + std::to_string(frame.rows);
    if (frame.rows > 0 && frame.cols == 2 * frame.rows) actual += " [" + std::to_string(frame.rows) + "/lens]";
    row(image, "Actual image", actual, 366);
    row(image, "Codec", codec_name(context.codec), 392);
    row(image, "Bitrate request", fixed(config.bitrate_bps / 1e6) + " Mb/s", 418);
    row(image, "IMU enabled", config.imu ? "Yes" : "No", 444);
    row(image, "Save data", config.save ? "Yes" : "No", 470);
    row(image, "Duration", config.duration_s ? fixed(config.duration_s, 1) + " s" : "Continuous", 496);
    row(image, "Status interval", fixed(config.status_interval_s, 1) + " s", 522);
    row(image, "Config", config.config_path.filename().string(), 548);
    row(image, "Power", live.power, 574);
    row(image, "Battery", live.battery, 600);
    row(image, "Run folder", config.save ? context.output_directory.filename().string() : "Not saving", 626);
    auto stream = stats.streams.find(stream_index);
    row(image, "Decoded frames", stream != stats.streams.end() ? std::to_string(stream->second.frames) : "--", 666);
    row(image, "IMU samples", std::to_string(stats.imu_samples), 692);
    row(image, "Live exposure", stats.latest_exposure_s ? fixed(*stats.latest_exposure_s * 1000, 3) + " ms" : "--", 718);
    text(image, "Live exposure comes from the camera callback.", 984, 749, 0.42, muted, 440);
    text(image, "Startup video units not decoded: " + std::to_string(
        stream != stats.streams.end() ? stream->second.startup_units_skipped : 0), 984, 773, 0.42, muted, 440);
    text(image, "Stop: q / Esc / close window / Ctrl+C", 984, 796, 0.46, muted, 440);
    std::string timing = "Host timing: SDK -> decoded --  |  SDK -> GUI prep --";
    if (live.frame_received_ns > 0 && live.frame_decoded_ns >= live.frame_received_ns) {
        timing = "Host timing: SDK -> decoded " + fixed((live.frame_decoded_ns - live.frame_received_ns) / 1e6, 1)
            + " ms  |  SDK -> GUI prep " + fixed(std::max<int64_t>(0, steady_time_ns() - live.frame_received_ns) / 1e6, 1) + " ms";
    }
    text(image, timing + " (not camera-to-screen)", 16, 536, 0.42, muted, 930);
    return image;
}
}
