#include "go3s/capture.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
// Offline fixture entry point: intentionally absent from the production CLI.
int main(int argc,char** argv) {
    try {
        if(argc!=5&&argc!=6) throw std::runtime_error("fixture args: port output duration imu [complete]");
        go3s::Config c; c.preview=false; c.save=true; c.output_directory=argv[2]; c.duration_s=std::stod(argv[3]); c.imu=std::string(argv[4])=="true"; c.status_interval_s=.1;
        int fd=socket(AF_INET,SOCK_STREAM,0); sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(static_cast<uint16_t>(std::stoi(argv[1]))); inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
        if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))) throw std::runtime_error("fixture connect failed");
        const auto mode=argc==6 ? go3s::DecodeMode::complete_h264_low_delay : go3s::DecodeMode::byte_stream;
        auto result=go3s::capture(fd,c,{},mode); close(fd); std::cout<<result.summary.dump()<<'\n'; return result.complete?0:1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
