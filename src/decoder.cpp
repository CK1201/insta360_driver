#include "x5/decoder.hpp"
#include "x5/detail/sps_patch.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace x5 {
namespace {
void checked(int result, const char* operation) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}
size_t start_code(const uint8_t* data, size_t size, size_t pos) {
    if (pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) return 3;
    if (pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) return 4;
    return 0;
}
bool h264_keyframe(const uint8_t* data, size_t size) {
    bool keyframe = false, picture = false;
    for (size_t pos = 0; pos < size;) {
        size_t prefix = start_code(data, size, pos);
        if (!prefix) { ++pos; continue; }
        size_t payload = pos + prefix;
        size_t end = payload;
        while (end < size && !start_code(data, size, end)) ++end;
        if (payload < end) {
            unsigned type = data[payload] & 31;
            if (type == 1 || type == 5) {
                picture = true;
                auto header = sps_patch::stripEmulation(data + payload + 1, std::min<size_t>(end - payload - 1, 32));
                sps_patch::BitReader bits(header);
                bits.ue(); // first_mb_in_slice
                auto slice_type = bits.ue();
                if (!bits.ok() || slice_type > 9) throw std::runtime_error("invalid H264 slice header in X5 access unit");
                if (slice_type % 5 == 1) throw std::runtime_error("X5 low-latency preview requires I/P-only H264 (B-frame found)");
                keyframe = keyframe || type == 5;
            }
        }
        pos = end;
    }
    if (!picture) throw std::runtime_error("X5 complete H264 access unit has no picture slice");
    return keyframe;
}
}
struct VideoDecoder::Impl {
    AVCodecContext* context = nullptr;
    AVCodecParserContext* parser = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;
    std::function<void(PreviewFrame)> on_frame;
    bool finished = false;
    bool initial_keyframe = false;
    uint64_t skipped_startup_units = 0;
    uint64_t patched_sps = 0;
    bool complete_h264;

    Impl(Codec codec, std::function<void(PreviewFrame)> callback, DecodeMode mode)
        : on_frame(std::move(callback)), complete_h264(codec == Codec::h264 && mode == DecodeMode::x5_complete_h264) {
        try {
            auto id = codec == Codec::h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
            const AVCodec* implementation = avcodec_find_decoder(id);
            if (!implementation) throw std::runtime_error("FFmpeg decoder unavailable");
            if (!complete_h264) parser = av_parser_init(id);
            context = avcodec_alloc_context3(implementation);
            frame = av_frame_alloc();
            packet = av_packet_alloc();
            if ((!complete_h264 && !parser) || !context || !frame || !packet) throw std::bad_alloc();
            context->thread_count = 1;
            context->pkt_timebase = {1, 1000000000};
            if (complete_h264) context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            checked(avcodec_open2(context, implementation, nullptr), "avcodec_open2");
        } catch (...) { release(); throw; }
    }
    ~Impl() { release(); }
    void release() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
        if (parser) av_parser_close(parser);
    }
    void receive() {
        while (true) {
            int result = avcodec_receive_frame(context, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            checked(result, "avcodec_receive_frame");
            auto format = static_cast<AVPixelFormat>(frame->format);
            bool jpeg_range = false;
            // YUVJ aliases have identical plane layouts to YUV. Normalize the
            // alias so swscale can reuse its context, then set range explicitly.
            switch (format) {
                case AV_PIX_FMT_YUVJ420P: format = AV_PIX_FMT_YUV420P; jpeg_range = true; break;
                case AV_PIX_FMT_YUVJ422P: format = AV_PIX_FMT_YUV422P; jpeg_range = true; break;
                case AV_PIX_FMT_YUVJ444P: format = AV_PIX_FMT_YUV444P; jpeg_range = true; break;
                case AV_PIX_FMT_YUVJ440P: format = AV_PIX_FMT_YUV440P; jpeg_range = true; break;
                default: break;
            }
            scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                format, frame->width, frame->height,
                AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!scaler) throw std::runtime_error("sws_getCachedContext failed");
            int color_space = SWS_CS_DEFAULT;
            if (frame->colorspace == AVCOL_SPC_BT709) color_space = SWS_CS_ITU709;
            else if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL)
                color_space = SWS_CS_BT2020;
            const int* coefficients = sws_getCoefficients(color_space);
            checked(sws_setColorspaceDetails(scaler, coefficients,
                jpeg_range || frame->color_range == AVCOL_RANGE_JPEG,
                coefficients, 1, 0, 1 << 16, 1 << 16), "sws_setColorspaceDetails");
            cv::Mat bgr(frame->height, frame->width, CV_8UC3);
            uint8_t* planes[] = {bgr.data, nullptr, nullptr, nullptr};
            int strides[] = {static_cast<int>(bgr.step), 0, 0, 0};
            int rows = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
            if (rows != frame->height) throw std::runtime_error("incomplete decoded image conversion");
            on_frame({std::move(bgr), frame->pts == AV_NOPTS_VALUE ? 0 : frame->pts, steady_time_ns()});
            av_frame_unref(frame);
        }
    }
    void send(const uint8_t* data, int size, bool keyframe, int64_t received_ns) {
        if (!initial_keyframe) {
            // A newly opened X5 can first deliver a cached P slice before its
            // SPS/PPS/IDR. Raw bytes are already saved by the capture pipeline.
            if (!keyframe) { ++skipped_startup_units; return; }
            initial_keyframe = true;
        }
        checked(av_new_packet(packet, size), "av_new_packet");
        std::memcpy(packet->data, data, size);
        packet->pts = received_ns > 0 ? received_ns : AV_NOPTS_VALUE;
        int result = avcodec_send_packet(context, packet);
        if (result == AVERROR(EAGAIN)) { receive(); result = avcodec_send_packet(context, packet); }
        av_packet_unref(packet);
        checked(result, "avcodec_send_packet");
        receive();
    }
    void feed(const uint8_t* bytes, size_t length, int64_t received_ns) {
        if (finished) throw std::runtime_error("decoder already flushed");
        if (length > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::runtime_error("video packet too large");
        if (!length) return;
        if (complete_h264) {
            bool keyframe = h264_keyframe(bytes, length);
            auto patched = sps_patch::patchAnnexb(bytes, length);
            if (!patched.empty()) {
                ++patched_sps;
                send(patched.data(), static_cast<int>(patched.size()), keyframe, received_ns);
            } else send(bytes, static_cast<int>(length), keyframe, received_ns);
            return;
        }
        std::vector<uint8_t> padded(length + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(padded.data(), bytes, length);
        const uint8_t* input = padded.data();
        int remaining = static_cast<int>(length);
        while (remaining > 0) {
            uint8_t* output = nullptr;
            int output_size = 0;
            int consumed = av_parser_parse2(parser, context, &output, &output_size,
                input, remaining, received_ns > 0 ? received_ns : AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            checked(consumed, "av_parser_parse2");
            if (output_size) send(output, output_size, parser->key_frame == 1, parser->pts);
            if (consumed == 0 && output_size == 0) throw std::runtime_error("video parser made no progress");
            input += consumed;
            remaining -= consumed;
        }
    }
    void finish() {
        if (finished) return;
        if (parser) {
            uint8_t* output = nullptr;
            int size = 0;
            checked(av_parser_parse2(parser, context, &output, &size, nullptr, 0,
                AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0), "av_parser_parse2 flush");
            if (size) send(output, size, parser->key_frame == 1, parser->pts);
        }
        if (!initial_keyframe) throw std::runtime_error("video stream ended before an initial keyframe");
        checked(avcodec_send_packet(context, nullptr), "avcodec_send_packet flush");
        receive();
        finished = true;
    }
};
VideoDecoder::VideoDecoder(Codec c, std::function<void(PreviewFrame)> f, DecodeMode mode) : impl_(std::make_unique<Impl>(c, std::move(f), mode)) {}
VideoDecoder::~VideoDecoder() = default;
void VideoDecoder::feed(const uint8_t* data, size_t size, int64_t received_ns) { impl_->feed(data, size, received_ns); }
void VideoDecoder::finish() { impl_->finish(); }
uint64_t VideoDecoder::startup_units_skipped() const { return impl_->skipped_startup_units; }
uint64_t VideoDecoder::sps_patches() const { return impl_->patched_sps; }
}
