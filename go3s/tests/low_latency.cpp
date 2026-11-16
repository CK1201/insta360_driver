#include "go3s/decoder.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavutil/md5.h>
}
namespace fs = std::filesystem;
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
struct Packet { std::vector<uint8_t> bytes; int64_t receipt; };
struct Result {
    std::vector<std::array<uint8_t,16>> hashes;
    std::vector<int64_t> receipts;
    std::vector<int> holds;
    uint64_t skips=0,patches=0;
    size_t before_flush=0;
};
std::vector<Packet> load(const fs::path& directory) {
    std::ifstream csv(directory/"video_packets.csv"),video(directory/"video.h264",std::ios::binary);
    require(csv.good()&&video.good(),"capture fixture missing");
    std::string line;std::getline(csv,line);std::vector<Packet> packets;
    while(std::getline(csv,line)) {
        std::istringstream in(line);std::string part;std::vector<std::string> row;
        while(std::getline(in,part,','))row.push_back(part);
        require(row.size()==8&&row[1]=="0","fixture packet layout");
        Packet p; p.bytes.resize(std::stoull(row[4]));p.receipt=std::stoll(row[7]);
        video.seekg(std::stoull(row[3]));video.read(reinterpret_cast<char*>(p.bytes.data()),p.bytes.size());
        require(video.good(),"fixture packet exceeds file");packets.push_back(std::move(p));
    }
    return packets;
}
Result replay(const std::vector<Packet>& packets,go3s::DecodeMode mode) {
    Result r;size_t feed=0;bool flush=false;
    go3s::Decoder* current=nullptr;
    go3s::Decoder decoder("h264",[&](go3s::Frame frame) {
        if(!flush)r.holds.push_back(static_cast<int>(feed-r.hashes.size()-current->startup_skipped()));
        std::array<uint8_t,16> hash;
        av_md5_sum(hash.data(),frame.bgr.data,frame.bgr.total()*frame.bgr.elemSize());
        r.hashes.push_back(hash);r.receipts.push_back(frame.host_receive_ns);
    },mode);current=&decoder;
    for(const auto& p:packets) {
        const auto original=p.bytes;
        decoder.feed(p.bytes.data(),p.bytes.size(),p.receipt);
        require(p.bytes==original,"decoder changed original packet bytes");++feed;
    }
    r.before_flush=r.hashes.size();flush=true;decoder.finish();r.skips=decoder.startup_skipped();r.patches=decoder.sps_patches();return r;
}
int main(int argc,char**argv) {
    try {
        require(argc==3,"usage: low_latency CAPTURE B_FRAME_CAPTURE");
        auto packets=load(argv[1]);require(packets.size()>2,"fixture too short");
        const auto baseline=replay(packets,go3s::DecodeMode::byte_stream);
        const auto fast=replay(packets,go3s::DecodeMode::complete_h264_low_delay);
        require(fast.hashes==baseline.hashes,"SPS fast path changed decoded BGR pixels");
        require(fast.receipts==baseline.receipts,"fast path changed frame receipt pairing");
        require(fast.before_flush==fast.hashes.size(),"fast decoder held frames until flush");
        require(fast.patches>0,"fixture did not exercise nonzero SPS reorder patch");
        for(int held:fast.holds)require(held==0,"fast decoder still waits for later frames");
        std::vector<Packet> old_prefix{packets[1]};old_prefix.insert(old_prefix.end(),packets.begin(),packets.end());
        auto prefixed=replay(old_prefix,go3s::DecodeMode::complete_h264_low_delay);
        require(prefixed.skips==1&&prefixed.hashes==fast.hashes&&prefixed.receipts==fast.receipts,"old P startup gate or accounting broken");
        bool rejected=false;
        try { replay(load(argv[2]),go3s::DecodeMode::complete_h264_low_delay); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("B-frame")!=std::string::npos; }
        require(rejected,"B-frame input was not explicitly rejected");
        nlohmann::json result={{"frames",fast.hashes.size()},{"generic_frames_at_flush",baseline.hashes.size()-baseline.before_flush},
            {"fast_frames_at_flush",fast.hashes.size()-fast.before_flush},{"fast_hold_frames",0},{"pixels_identical",true},
            {"frame_receipts_identical",true},{"original_input_bytes_unchanged",true},{"sps_patches",fast.patches}};
        std::cout<<result.dump(2)<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
