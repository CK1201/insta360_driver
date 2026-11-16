#include "x5/preview.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: preview_smoke OUTPUT_DIRECTORY");
        std::filesystem::path directory = argv[1];
        std::filesystem::create_directories(directory);
        cv::Mat frame(480, 960, CV_8UC3, cv::Scalar(12, 12, 12));
        cv::circle(frame, {240, 240}, 225, cv::Scalar(140, 105, 65), cv::FILLED);
        cv::circle(frame, {720, 240}, 225, cv::Scalar(65, 120, 140), cv::FILLED);
        cv::putText(frame, "Camera fixture", {320, 245}, cv::FONT_HERSHEY_SIMPLEX, 0.9, {230, 230, 230}, 2);
        auto original = frame.clone();
        x5::PreviewContext context;
        context.config.config_path = "/example/config/x5.yaml";
        context.config.exposure_mode = "manual";
        context.config.iso = 400;
        context.config.shutter_s = 0.005;
        context.config.white_balance_kelvin = 5000;
        context.identity["model"] = "X5 (UI test fixture)";
        context.identity["firmware"] = "v1.13.21";
        context.identity["serial"] = "TEST123456";
        context.readback["exposure"]["mode"] = "manual";
        context.readback["exposure"]["iso"] = 400;
        context.readback["exposure"]["shutter_s"] = 0.005;
        context.readback["exposure"]["ev"] = 0;
        context.readback["white_balance_kelvin"] = 5000;
        context.output_directory = "/example/outputs/20261116_140000_123456789";
        x5::PreviewLive live;
        live.capture.latest_imu = x5::ImuSample{12345678, 0.125, -0.25, 0.975, 0.0123, -0.0456, 0.0789};
        live.capture.latest_imu_received_ns = 992000000;
        live.capture.imu_samples = 5500;
        live.capture.latest_exposure_s = 0.005;
        live.capture.streams[0] = {327, 10000000, 327, 960, 480};
        live.video_fps = 29.97;
        live.imu_hz = 500;
        live.elapsed_s = 11.2;
        live.battery = "100 (raw, scale unknown)";
        live.power = "External";
        auto manual = x5::render_preview(frame, 0, context, live, 1000000000);
        if (!cv::imwrite((directory / "manual.png").string(), manual)) throw std::runtime_error("cannot save manual preview");
        if (cv::norm(frame, original, cv::NORM_INF) != 0) throw std::runtime_error("preview modified original camera image");
        context.config.exposure_mode = "auto";
        context.config.iso.reset();
        context.config.shutter_s.reset();
        context.config.white_balance_kelvin = 0;
        context.readback["exposure"]["mode"] = "auto";
        context.readback["exposure"]["iso"] = 0;
        context.readback["exposure"]["shutter_s"] = 0;
        context.readback["white_balance_kelvin"] = 0;
        live.capture.latest_imu.reset();
        live.capture.latest_exposure_s.reset();
        live.video_fps.reset();
        live.imu_hz.reset();
        auto waiting = x5::render_preview(frame, 0, context, live, 1000000000);
        if (!cv::imwrite((directory / "waiting.png").string(), waiting)) throw std::runtime_error("cannot save waiting preview");
        std::cout << "PASS: display-only composition preserves source pixels; manual and waiting render samples saved\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
