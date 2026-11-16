# H.264 SPS helper attribution

`include/go3s/detail/sps_patch.hpp` derives from [specialist-ai/insta360_driver](https://github.com/specialist-ai/insta360_driver/blob/22641e241c18857f345fbbf8643592194beefb7f/include/insta360_camera/sps_patch.h), commit `22641e241c18857f345fbbf8643592194beefb7f`, through this workspace’s verified X5 implementation.

Copyright (c) 2026 Hybrid Robotics. The MIT license is preserved in this directory. GO3S uses this helper only on the decoder input copy; raw recorded camera bytes remain unchanged. The helper is not applied to HEVC or generic fragmented input. No upstream SDK files are copied.
