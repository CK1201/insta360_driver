#pragma once
#include "x5/capture.hpp"
#include "x5/config.hpp"
#include <functional>

namespace x5 {
using FrameObserver = std::function<void(int, const PreviewFrame&)>;
struct RunResult { int code = 0; std::string error; bool cancelled_before_data = false; };
RunResult run_session(const Config& config, const std::function<bool()>& interrupted = {},
                      const FrameObserver& on_frame = {});
}
