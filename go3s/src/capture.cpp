#include "go3s/capture.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
namespace go3s {
bool EventQueue::push(Event e) {
    std::lock_guard<std::mutex> lock(mutex_); const size_t size=e.bytes();
    if(closed_ || size > capacity_ - bytes_) return false;
    bytes_+=size; high_water_=std::max(high_water_,bytes_); events_.push_back(std::move(e)); ready_.notify_one(); return true;
}
bool EventQueue::pop(Event& e) {
    std::unique_lock<std::mutex> lock(mutex_); ready_.wait(lock,[&]{return closed_ || !events_.empty();});
    if(events_.empty()) return false;
    bytes_-=events_.front().bytes(); e=std::move(events_.front()); events_.pop_front(); return true;
}
void EventQueue::close() { std::lock_guard<std::mutex> lock(mutex_); closed_=true; ready_.notify_all(); }
size_t EventQueue::high_water() const { std::lock_guard<std::mutex> lock(mutex_); return high_water_; }
namespace {
std::string csv(const std::string& text) { std::string r="\""; for(char c:text) { if(c=='\"') r+='\"'; r+=c; } return r+'\"'; }
std::string num(double x) { std::ostringstream s; s<<std::setprecision(8)<<x; return s.str(); }
int64_t integer(const Json& h,const char* key) {
    if(!h.contains(key) || !h[key].is_number_integer()) throw std::runtime_error(std::string("missing integer field: ")+key);
    return h[key].get<int64_t>();
}
void write_text(const std::filesystem::path& p,const std::string& text) {
    std::ofstream f; f.exceptions(std::ios::badbit|std::ios::failbit); f.open(p); f<<text; f.close();
}
struct Files {
    bool enabled=false, writable=true;
    bool video_writable=true, gyro_writable=true;
    std::filesystem::path dir;
    std::ofstream video,gyro,video_index,gyro_index,imu,exposure,status,timing;
    uint64_t video_offset=0, gyro_offset=0;
    std::string codec;
    void open(std::ofstream& f,const char* name,const char* header) {
        f.exceptions(std::ios::badbit|std::ios::failbit); f.open(dir/name,std::ios::binary); if(header) f<<header<<'\n';
    }
    explicit Files(const Config& c) : enabled(c.save) {
        if(!enabled) return;
        dir=c.output_directory/ ("go3s_"+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())+"_"+std::to_string(getpid()));
        if(!std::filesystem::create_directories(dir)) throw std::runtime_error("session directory already exists: "+dir.string());
        if(!c.source.empty()) std::filesystem::copy_file(c.source,dir/"config_source.yaml");
        else write_text(dir/"config_source.yaml",c.effective_yaml());
        write_text(dir/"config_effective.yaml",c.effective_yaml());
        open(gyro,"gyro.bin",nullptr);
        open(video_index,"video_packets.csv","seq,stream_index,codec,byte_offset,byte_length,camera_timestamp_raw,phone_receive_ns,host_receive_ns");
        open(gyro_index,"gyro_packets.csv","seq,byte_offset,byte_length,camera_timestamp_raw,phone_receive_ns,host_receive_ns,sample_count");
        open(imu,"imu.csv","seq,sample_index,device_timestamp_raw,ax_raw,ay_raw,az_raw,gx_raw,gy_raw,gz_raw,camera_timestamp_raw,phone_receive_ns,host_receive_ns");
        open(exposure,"exposure.csv","seq,camera_timestamp_raw,phone_receive_ns,host_receive_ns,exposure_time_s");
        open(status,"status.csv","seq,phone_receive_ns,host_receive_ns,raw_json");
        open(timing,"frame_timing.csv","frame_index,host_receive_ns,host_decode_ns,host_decode_delay_ms,width,height,timestamp_ns");
    }
    void raw(const Event& e) {
        if(!enabled) return;
        const auto type=e.header.at("type").get<std::string>();
        if(type=="video") {
            if(!video_writable) return;
            try {
            const auto codec_now=e.header.at("codec").get<std::string>();
            if(codec_now!="h264" && codec_now!="h265") throw std::runtime_error("unknown video codec");
            if(codec.empty()) { codec=codec_now; open(video,codec=="h264"?"video.h264":"video.h265",nullptr); }
            if(codec!=codec_now) throw std::runtime_error("video codec changed mid-session");
            video.write(reinterpret_cast<const char*>(e.payload.data()),static_cast<std::streamsize>(e.payload.size())); video.flush();
            video_index<<e.header.at("seq")<<','<<e.header.at("stream_index")<<','<<codec<<','<<video_offset<<','<<e.payload.size()<<','<<integer(e.header,"camera_timestamp")<<','<<integer(e.header,"phone_receive_ns")<<','<<e.host_receive_ns<<'\n'; video_index.flush();
            video_offset+=e.payload.size();
            } catch(...) { video_writable=false; throw; }
        } else if(type=="gyro") {
            if(!gyro_writable) return;
            try {
            gyro.write(reinterpret_cast<const char*>(e.payload.data()),static_cast<std::streamsize>(e.payload.size())); gyro.flush();
            gyro_index<<e.header.at("seq")<<','<<gyro_offset<<','<<e.payload.size()<<','<<integer(e.header,"camera_timestamp")<<','<<integer(e.header,"phone_receive_ns")<<','<<e.host_receive_ns<<','<<e.header.at("samples").size()<<'\n'; gyro_index.flush(); gyro_offset+=e.payload.size();
            } catch(...) { gyro_writable=false; throw; }
        }
    }
};
struct State {
    std::mutex mutex;
    std::string error;
    std::function<void(const std::string&)> on_error;
    Json session, latest_status, latest_imu;
    cv::Mat latest;
    uint64_t video_packets=0,gyro_packets=0,imu_samples=0,exposures=0,frames=0,accepted=0,processed=0,startup_skipped=0,sps_patched=0;
    int64_t last_video=0,last_imu=0,last_frame=0;
    double exposure_s=0,decode_ms=0,current_frame_rate=-1,current_imu_rate=-1;
    int decoded_width=0,decoded_height=0;
    std::string codec;
    void fail(const std::string& message) {
        { std::lock_guard<std::mutex> lock(mutex); if(!error.empty()) return; error=message; }
        if(on_error) on_error(message);
    }
    bool failed() { std::lock_guard<std::mutex> lock(mutex); return !error.empty(); }
};
void show(const Config& c,State& s,int64_t start) {
    cv::Mat picture; Json session,status,imu; uint64_t frames,samples; double exposure,delay,fps,imu_rate;
    { std::lock_guard<std::mutex> lock(s.mutex); picture=s.latest.clone(); session=s.session; status=s.latest_status; imu=s.latest_imu; frames=s.frames; samples=s.imu_samples; exposure=s.exposure_s; delay=s.decode_ms; fps=s.current_frame_rate; imu_rate=s.current_imu_rate; }
    const bool has_picture=!picture.empty();
    if(!has_picture) picture=cv::Mat(360,640,CV_8UC3,cv::Scalar(20,20,20));
    const double elapsed=static_cast<double>(steady_ns()-start)/1e9;
    auto compact=[](double value) { std::ostringstream text; text<<std::fixed<<std::setprecision(4)<<value; return text.str(); };
    auto field=[](const Json& object,const char* key) {
        if(!object.contains(key) || object[key].is_null()) return std::string("unknown");
        if(object[key].is_number_float()) return num(object[key].get<double>());
        return object[key].is_string()?object[key].get<std::string>():object[key].dump();
    };
    std::string readback="waiting";
    if(session.contains("readback")) {
        const auto& r=session["readback"]; const auto e=r.value("exposure",Json::object());
        readback="mode "+field(e,"mode")+" ISO "+field(e,"iso")+" shutter_s "+field(e,"shutter_s")+" EV "+field(e,"ev")+" WB "+field(r,"white_balance_kelvin");
        if(e.contains("sdk_exposure_mode")) readback+=" SDK "+field(e,"sdk_exposure_mode");
    }
    const bool have_imu=imu.is_array() && imu.size()==7;
    const auto preview=status.contains("preview")?status["preview"]:(session.contains("preview")?session["preview"]:Json::object());
    std::vector<std::string> lines={"GO 3S | single lens | q: stop", (has_picture?"Decoded "+std::to_string(picture.cols)+"x"+std::to_string(picture.rows):"Decoded size: waiting")+" | FPS current "+(fps>=0?compact(fps):"waiting")+" mean "+compact(frames/std::max(0.001,elapsed))+" | goal 30",
        "IMU/s current "+(imu_rate>=0?compact(imu_rate):"waiting")+" mean "+compact(samples/std::max(0.001,elapsed))+" | host decode delay ms "+compact(delay),
        "Requested exposure: "+c.exposure_mode+" ISO "+(c.iso?std::to_string(*c.iso):"auto")+" shutter_s "+(c.shutter_s?num(*c.shutter_s):"auto")+" EV "+num(c.ev)+" WB "+std::to_string(c.white_balance_kelvin),
        "Readback: "+readback,
        "Live exposure seconds: "+(exposure>0?num(exposure):"unavailable"),
        "Camera IMU raw timestamp: "+(have_imu?imu[0].dump():"waiting"),
        "Accel raw: ax "+(have_imu?compact(imu[1].get<double>())+"  ay "+compact(imu[2].get<double>())+"  az "+compact(imu[3].get<double>()):"waiting"),
        "Gyro raw:  gx "+(have_imu?compact(imu[4].get<double>())+"  gy "+compact(imu[5].get<double>())+"  gz "+compact(imu[6].get<double>()):"waiting"),
        "SDK preview: "+field(preview,"codec")+" "+field(preview,"width")+"x"+field(preview,"height")+" fps "+field(preview,"fps"),
        "Battery: "+field(status,"battery_level")+" / "+field(status,"battery_scale")+" power "+field(status,"power")};
    if(status.contains("storage") && status["storage"].is_array() && !status["storage"].empty()) {
        size_t index=0;
        for(const auto& storage:status["storage"]) lines.push_back("Storage "+std::to_string(index++)+": state "+field(storage,"state")+" free "+field(storage,"free")+" total "+field(storage,"total")+" bytes");
    } else lines.push_back("Storage: unknown");
    int width=std::max(picture.cols,1000); cv::Mat view(picture.rows+static_cast<int>(lines.size())*24+10,width,CV_8UC3,cv::Scalar(20,20,20)); picture.copyTo(view(cv::Rect(0,0,picture.cols,picture.rows)));
    for(size_t i=0;i<lines.size();++i) cv::putText(view,lines[i],{8,picture.rows+22+static_cast<int>(i)*24},cv::FONT_HERSHEY_SIMPLEX,0.48,{220,235,220},1,cv::LINE_AA);
    cv::imshow("GO3S bridge",view);
}
}
CaptureResult capture(int fd,const Config& config,const std::function<bool()>& interrupted,DecodeMode mode,
                      const CaptureOptions& options) {
    validate_config(config); Files files(config); State s; s.on_error=options.on_error; EventQueue queue;
    const auto sensor_delay_ns=config.sensor_delay_ns();
    CaptureResult result; result.directory=files.dir;
    const int64_t start=steady_ns(); std::atomic<int64_t> stop_deadline{0};
    std::atomic<bool> reader_done{false},saw_end{false}; std::string end_reason;
    Event hello; Sequence sequence; bool handshake_canceled=false;
    try {
        if(!receive_event(fd,hello,[&]{handshake_canceled=interrupted&&interrupted(); return steady_ns()-start>10'000'000'000LL || handshake_canceled;})) throw std::runtime_error("EOF before hello");
        sequence.accept(hello.header);
        if(hello.header["type"]!="hello" || hello.header.at("sdk_version")!="2.2.6" || hello.header.at("sdk_internal_loss_observable")!=false) throw std::runtime_error("incompatible bridge hello");
        if(files.enabled) write_text(files.dir/"phone.yaml",hello.header.dump(2)+"\n");
        send_message(fd,config.start_request());
    } catch(const std::exception& e) {
        result.complete=options.allow_empty_stop && handshake_canceled;
        if(!result.complete) result.error=e.what();
        result.summary={{"complete",result.complete},{"error",result.error},{"canceled_before_hello",result.complete},
            {"sdk_internal_loss_observable",false},{"sdk_internal_loss_verification","unverified"}};
        if(files.enabled) write_text(files.dir/"summary.yaml",result.summary.dump(2)+"\n");
        return result;
    }
    std::thread worker([&] {
        std::unique_ptr<Decoder> decoder; bool decode_enabled=true;
        auto frame=[&](Frame f) {
            f.timestamp_ns=f.host_receive_ns ? f.host_receive_ns-sensor_delay_ns : 0;
            uint64_t index;
            { std::lock_guard<std::mutex> lock(s.mutex); index=s.frames++; s.last_frame=f.host_decode_ns; s.decode_ms=static_cast<double>(f.host_decode_ns-f.host_receive_ns)/1e6; s.decoded_width=f.bgr.cols; s.decoded_height=f.bgr.rows; if(config.preview) s.latest=f.bgr; }
            if(files.enabled && files.writable) {
                files.timing<<index<<','<<f.host_receive_ns<<','<<f.host_decode_ns<<','<<std::setprecision(17)<<static_cast<double>(f.host_decode_ns-f.host_receive_ns)/1e6<<','<<f.bgr.cols<<','<<f.bgr.rows<<','<<f.timestamp_ns<<'\n'; files.timing.flush();
            }
            if(options.on_frame) options.on_frame(f);
        };
        Event e;
        while(queue.pop(e)) {
            // Persistence comes first; decoder failure must not abandon queued raw data.
            try { files.raw(e); } catch(const std::exception& ex) { s.fail(std::string("raw output failure: ")+ex.what()); }
            try {
                const auto& h=e.header; const auto type=h.at("type").get<std::string>();
                if(type=="session") {
                    for(const auto* key:{"identity","requested","readback","preview"}) if(!h.contains(key) || !h[key].is_object()) throw std::runtime_error(std::string("invalid session ")+key);
                    { std::lock_guard<std::mutex> lock(s.mutex); if(!s.session.is_null()) throw std::runtime_error("duplicate session event"); s.session=h; }
                    if(files.enabled && files.writable) write_text(files.dir/"session.yaml",h.dump(2)+"\n");
                } else if(type=="video") {
                    if(h.at("stream_index")!=0) throw std::runtime_error("GO3S requires single stream_index 0");
                    const auto codec=h.at("codec").get<std::string>(); integer(h,"camera_timestamp"); integer(h,"phone_receive_ns");
                    { std::lock_guard<std::mutex> lock(s.mutex); if(s.session.is_null()) throw std::runtime_error("video before session"); if(!s.codec.empty() && s.codec!=codec) throw std::runtime_error("video codec changed"); s.codec=codec; ++s.video_packets; s.last_video=e.host_receive_ns; }
                    if(decode_enabled) {
                        try { if(!decoder) decoder=std::make_unique<Decoder>(codec,frame,mode); decoder->feed(e.payload.data(),e.payload.size(),e.host_receive_ns); }
                        catch(const std::exception& ex) { if(decoder) { std::lock_guard<std::mutex> lock(s.mutex); s.startup_skipped=decoder->startup_skipped(); s.sps_patched=decoder->sps_patches(); } decoder.reset(); decode_enabled=false; s.fail(std::string("video decode failure: ")+ex.what()); }
                    }
                } else if(type=="gyro") {
                    integer(h,"camera_timestamp"); integer(h,"phone_receive_ns");
                    if(h.contains("parse_error")) s.fail("camera gyro parse failure: "+h.at("parse_error").dump());
                    const auto& samples=h.at("samples"); if(!samples.is_array()) throw std::runtime_error("gyro samples must be an array");
                    size_t i=0;
                    for(const auto& sample:samples) {
                        if(!sample.is_array() || sample.size()!=7 || !sample[0].is_number_integer()) throw std::runtime_error("invalid camera 6-axis sample");
                        for(size_t j=1;j<7;++j) if(!sample[j].is_number() || !std::isfinite(sample[j].get<double>())) throw std::runtime_error("invalid IMU numeric value");
                        if(files.enabled && files.writable) { files.imu<<h.at("seq")<<','<<i; for(const auto& value:sample) files.imu<<','<<value.dump(); files.imu<<','<<h.at("camera_timestamp")<<','<<h.at("phone_receive_ns")<<','<<e.host_receive_ns<<'\n'; }
                        { std::lock_guard<std::mutex> lock(s.mutex); s.latest_imu=sample; ++s.imu_samples; s.last_imu=e.host_receive_ns; }
                        ++i;
                    }
                    if(files.enabled && files.writable) files.imu.flush();
                    { std::lock_guard<std::mutex> lock(s.mutex); ++s.gyro_packets; }
                } else if(type=="exposure") {
                    const double value=h.at("exposure_time_s").get<double>();
                    if(!std::isfinite(value) || value<=0) throw std::runtime_error("invalid exposure time");
                    if(files.enabled && files.writable) { files.exposure<<h.at("seq")<<','<<integer(h,"camera_timestamp")<<','<<integer(h,"phone_receive_ns")<<','<<e.host_receive_ns<<','<<std::setprecision(17)<<value<<'\n'; files.exposure.flush(); }
                    { std::lock_guard<std::mutex> lock(s.mutex); ++s.exposures; s.exposure_s=value; }
                } else if(type=="status") {
                    integer(h,"phone_receive_ns");
                    if(files.enabled && files.writable) { files.status<<h.at("seq")<<','<<h.at("phone_receive_ns")<<','<<e.host_receive_ns<<','<<csv(h.dump())<<'\n'; files.status.flush(); }
                    { std::lock_guard<std::mutex> lock(s.mutex); s.latest_status=h; }
                    if(h.at("connected")!=true) s.fail("camera disconnected");
                } else if(type=="error") { s.fail("phone bridge error: "+h.at("message").get<std::string>()); }
                else if(type=="end") { if(h.at("reason")!="stop") s.fail("phone ended session: "+h.at("reason").get<std::string>()); }
                else throw std::runtime_error("unexpected phone event type: "+type);
            } catch(const std::ios_base::failure& ex) { files.writable=false; s.fail(std::string("output failure: ")+ex.what()); }
            catch(const std::exception& ex) { s.fail(std::string("event processing failure: ")+ex.what()); }
            { std::lock_guard<std::mutex> lock(s.mutex); ++s.processed; }
        }
        if(decoder) {
            try { decoder->finish(); } catch(const std::exception& ex) { s.fail(std::string("video decoder flush failure: ")+ex.what()); }
            std::lock_guard<std::mutex> lock(s.mutex); s.startup_skipped=decoder->startup_skipped(); s.sps_patched=decoder->sps_patches();
        }
    });
    std::thread reader([&] {
        bool accepting=true;
        try {
            Event e;
            while(receive_event(fd,e,[&]{return stop_deadline.load()!=0 && steady_ns()>stop_deadline.load();})) {
                sequence.accept(e.header); const bool ended=e.header.at("type")=="end";
                if(ended) end_reason=e.header.at("reason").get<std::string>();
                if(accepting) {
                    if(!queue.push(std::move(e))) { accepting=false; s.fail("host 64 MiB queue overflow; rejected event was not accepted"); }
                    else { std::lock_guard<std::mutex> lock(s.mutex); ++s.accepted; }
                }
                if(ended) { saw_end=true; break; }
            }
            if(!saw_end) s.fail("EOF without end: session incomplete");
        } catch(const std::exception& ex) { s.fail(std::string("bridge receive failure: ")+ex.what()); }
        queue.close(); reader_done=true;
    });
    bool stop_sent=false,window_created=false,warned=false;
    int64_t last_status=start,next_status=start+static_cast<int64_t>(config.status_interval_s*1e9);
    uint64_t last_frames=0,last_samples=0;
    try {
        if(config.preview) { cv::namedWindow("GO3S bridge",cv::WINDOW_NORMAL); cv::resizeWindow("GO3S bridge",1200,820); window_created=true; }
        while(!reader_done) {
            auto now=steady_ns(); bool stop=interrupted&&interrupted();
            if(config.duration_s>0 && static_cast<double>(now-start)/1e9>=config.duration_s) stop=true;
            std::string timeout_error;
            { std::lock_guard<std::mutex> lock(s.mutex);
              if(s.error.empty()) {
                  if(now-(s.last_video?s.last_video:start)>10'000'000'000LL) timeout_error="video timeout: no callback for 10 seconds";
                  else if(config.imu && now-(s.last_imu?s.last_imu:start)>10'000'000'000LL) timeout_error="camera IMU timeout: no sample for 10 seconds";
                  else if(now-(s.last_frame?s.last_frame:start)>10'000'000'000LL) timeout_error="decode timeout: no decoded frame for 10 seconds";
              }
              if(!warned && !s.exposures && now-start>10'000'000'000LL) { std::cerr<<"Warning: camera exposure callback unavailable\n"; warned=true; }
            }
            if(!timeout_error.empty()) s.fail(timeout_error);
            if(config.preview && window_created) {
                show(config,s,start); int key=cv::waitKey(1);
                if(key=='q'||key==27) stop=true;
                // Some HighGUI backends remove their last window immediately.
                try { if(cv::getWindowProperty("GO3S bridge",cv::WND_PROP_VISIBLE)<1) { stop=true; window_created=false; } }
                catch(const cv::Exception&) { stop=true; window_created=false; }
            }
            if(options.monitor_stdin) {
                pollfd input{STDIN_FILENO,POLLIN,0}; if(poll(&input,1,0)>0 && (input.revents&POLLIN)) { char c; if(read(STDIN_FILENO,&c,1)==1 && c=='q') stop=true; }
            }
            if((stop||s.failed()) && !stop_sent && !saw_end) {
                stop_sent=true; stop_deadline=steady_ns()+5'000'000'000LL;
                try { send_message(fd,{{"v",1},{"type","stop"}}); } catch(const std::exception& ex) { s.fail(ex.what()); shutdown(fd,SHUT_RDWR); }
            }
            if(now>=next_status) {
                std::lock_guard<std::mutex> lock(s.mutex);
                const double interval=static_cast<double>(now-last_status)/1e9;
                s.current_frame_rate=static_cast<double>(s.frames-last_frames)/interval;
                s.current_imu_rate=static_cast<double>(s.imu_samples-last_samples)/interval;
                auto status_field=[&](const char* key) { return s.latest_status.contains(key) && !s.latest_status[key].is_null() ? s.latest_status[key].dump() : std::string("unknown"); };
                std::cerr<<"GO3S fps="<<num(s.current_frame_rate)<<" imu_samples_per_s="<<num(s.current_imu_rate)
                    <<" size="<<(s.decoded_width?std::to_string(s.decoded_width)+"x"+std::to_string(s.decoded_height):"unknown")
                    <<" exposure_s="<<(s.exposure_s>0?num(s.exposure_s):"unknown")
                    <<" battery="<<status_field("battery_level")<<'/'<<status_field("battery_scale")
                    <<" power="<<status_field("power")<<" storage="<<status_field("storage")
                    <<" video_packets="<<s.video_packets<<" frames="<<s.frames<<" imu_samples="<<s.imu_samples<<'\n';
                last_status=now; last_frames=s.frames; last_samples=s.imu_samples;
                next_status=now+static_cast<int64_t>(config.status_interval_s*1e9);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch(const std::exception& ex) {
        s.fail(std::string("preview/control failure: ")+ex.what());
        if(!stop_sent) { stop_deadline=steady_ns()+5'000'000'000LL; try { send_message(fd,{{"v",1},{"type","stop"}}); } catch(...) { shutdown(fd,SHUT_RDWR); } }
    }
    reader.join(); worker.join(); if(window_created) cv::destroyWindow("GO3S bridge");
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const bool empty_stop=options.allow_empty_stop && interrupted && interrupted() && !s.video_packets && !s.frames;
        if(s.error.empty() && !empty_stop && s.session.is_null()) s.error="missing session metadata";
        if(s.error.empty() && !empty_stop && !s.video_packets) s.error="no video callbacks received";
        if(s.error.empty() && !empty_stop && !s.frames) s.error="no decoded video frames";
        if(s.error.empty() && !empty_stop && config.imu && !s.imu_samples) s.error="camera IMU enabled but no 6-axis samples received";
        if(s.error.empty() && !saw_end) s.error="missing end event";
        result.error=s.error; result.complete=result.error.empty() && saw_end && end_reason=="stop";
        result.summary={{"complete",result.complete},{"error",result.error},{"end_received",saw_end.load()},{"end_reason",end_reason},
            {"video_packets",s.video_packets},{"gyro_packets",s.gyro_packets},{"imu_samples",s.imu_samples},{"exposure_callbacks",s.exposures},{"decoded_frames",s.frames},
            {"startup_units_skipped_for_preview",s.startup_skipped},{"preview_sps_patches",s.sps_patched},
            {"preview_decode_mode",s.codec=="h264" && mode==DecodeMode::complete_h264_low_delay ? "complete_h264_low_delay" : "byte_stream"},
            {"accepted_events",s.accepted},{"processed_events",s.processed},
            {"host_queue_capacity_bytes",64u*1024u*1024u},{"host_queue_high_water_bytes",queue.high_water()},
            {"video_bytes_saved",files.video_offset},{"gyro_bytes_saved",files.gyro_offset},{"sdk_internal_loss_observable",false},{"sdk_internal_loss_verified",false},
            {"codec",s.codec},{"latest_phone_status",s.latest_status},{"elapsed_host_s",static_cast<double>(steady_ns()-start)/1e9},
            {"decoded_width",s.decoded_width},{"decoded_height",s.decoded_height},
            {"sdk_internal_loss_verification","unverified"},{"acceptance_boundary","Android app SDK callbacks; SDK internal dispatch loss is unobservable"},
            {"clock_domains",{{"camera_timestamp_raw","SDK callback raw timestamp; no unit conversion"},{"device_timestamp_raw","OneGyroField.originTsMs; raw value"},{"phone_receive_ns","Android elapsedRealtimeNanos at app callback"},{"host_receive_ns","host steady_clock at complete wire message"},{"host_decode_ns","host steady_clock at decoded frame"},{"timestamp_ns","host_receive_ns minus configured sensor_delay; 0 if receipt unknown; uncalibrated host-monotonic estimate"}}},
            {"sensor_delay_s",config.sensor_delay_s},
            {"exposure_warning",s.exposures?Json(nullptr):Json("no camera exposure callbacks received")},{"hardware_verified",false}};
    }
    if(files.enabled) {
        try { write_text(files.dir/"summary.yaml",result.summary.dump(2)+"\n"); }
        catch(const std::exception& ex) { result.complete=false; result.error+="; summary output failure: "+std::string(ex.what()); result.summary["complete"]=false; result.summary["error"]=result.error; }
    }
    if(!warned && result.summary["exposure_callbacks"]==0) std::cerr<<"Warning: no camera exposure callbacks received\n";
    return result;
}
}
