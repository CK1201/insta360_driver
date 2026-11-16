#include "go3s/adb.hpp"
#include "go3s/capture.hpp"
#include <unistd.h>
#include <csignal>
#include <iostream>
#include <stdexcept>
namespace { volatile std::sig_atomic_t stopped=0; void signal_handler(int) { stopped=1; } }
int main(int argc,char** argv) {
    int fd=-1; bool forwarded=false; std::string serial; int port=18765;
    try {
        std::string path="config/go3s.yaml"; bool check=false; std::optional<double> duration;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") { std::cout<<"GO3S Android bridge client 0.4.1\nUsage: insta360_go3s_client [--config FILE] [--check-config] [--duration SECONDS] [--help]\n--check-config validates and prints effective settings without ADB, USB, network or output creation.\nStop with q, window close, Ctrl+C, or --duration. Phone bridge must already be running.\n"; return 0; }
            if(arg=="--check-config") check=true;
            else if(arg=="--config" || arg=="--duration") {
                if(++i>=argc) throw std::runtime_error(arg+" needs a value");
                if(arg=="--config") path=argv[i]; else { size_t used=0; duration=std::stod(argv[i],&used); if(used!=std::string(argv[i]).size()) throw std::runtime_error("invalid duration"); }
            } else throw std::runtime_error("unknown option: "+arg);
        }
        auto config=go3s::load_config(path); if(duration) config.duration_s=*duration; go3s::validate_config(config);
        if(check) { std::cout<<config.effective_yaml(); return 0; }
        std::signal(SIGINT,signal_handler); std::signal(SIGTERM,signal_handler);
        serial=go3s::select_phone(config.adb_serial); port=config.port;
        go3s::forward_phone(serial,port); forwarded=true; fd=go3s::connect_phone(port);
        auto result=go3s::capture(fd,config,[]{return stopped!=0;}); close(fd); fd=-1;
        go3s::remove_forward(serial,port); forwarded=false;
        std::cout<<result.summary.dump(2)<<'\n'; if(!result.directory.empty()) std::cout<<"Saved: "<<result.directory<<'\n';
        if(!result.complete) { std::cerr<<"Capture failed: "<<result.error<<'\n'; return 1; } return 0;
    } catch(const std::exception& e) {
        if(fd>=0) close(fd);
        if(forwarded) { try {go3s::remove_forward(serial,port);} catch(...) {} }
        std::cerr<<"GO3S: "<<e.what()<<'\n'; return 1;
    }
}
