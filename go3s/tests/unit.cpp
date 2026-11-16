#include "go3s/capture.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <limits>
namespace {
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool thrown=false; try {f();} catch(const std::exception&) {thrown=true;} require(thrown,"expected rejection"); }
}
int main() {
    try {
        go3s::Config c; go3s::validate_config(c);
        require(c.sensor_delay_ns()==0,"sensor delay defaults to zero");
        c.sensor_delay_s=0.120;
        require(c.sensor_delay_ns()==120000000,"sensor delay seconds to ns");
        require(go3s::Json::parse(c.effective_yaml())["timing"]["sensor_delay_s"]==0.120,"effective config delay");
        require(!c.start_request().contains("timing"),"host timing must not alter phone protocol");
        for(double value:{-0.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),1e30}) {
            auto invalid=c; invalid.sensor_delay_s=value; rejects([&]{go3s::validate_config(invalid);});
        }
        c=go3s::Config{};
        require(c.exposure_mode=="auto" && c.white_balance_kelvin==0 && c.imu && c.preview && !c.save && c.duration_s==0 && c.status_interval_s==1,"defaults");
        for(const auto& mode:{"manual","iso_priority","shutter_priority"}) {
            c=go3s::Config{}; c.exposure_mode=mode; rejects([&]{go3s::validate_config(c);});
            if(c.exposure_mode!="shutter_priority") c.iso=200;
            if(c.exposure_mode!="iso_priority") c.shutter_s=0.01;
            go3s::validate_config(c); require(go3s::Json::parse(c.start_request().dump())["camera"]["exposure"]["mode"]==mode,"JSON mode");
        }
        c=go3s::Config{}; c.duration_s=-1; rejects([&]{go3s::validate_config(c);});
        c=go3s::Config{}; c.iso=100; rejects([&]{go3s::validate_config(c);});
        for(double value:{0.05,61.0}) { c=go3s::Config{}; c.status_interval_s=value; rejects([&]{go3s::validate_config(c);}); }
        for(double value:{0.1,60.0}) { c=go3s::Config{}; c.status_interval_s=value; go3s::validate_config(c); }
        go3s::Sequence sequence; sequence.accept(go3s::Json::parse(R"({"seq":0})")); sequence.accept(go3s::Json::parse(R"({"seq":1})")); rejects([&]{sequence.accept(go3s::Json::parse(R"({"seq":3})"));});
        go3s::Event e{go3s::Json{{"v",1},{"type","video"}},std::vector<uint8_t>(70),1};
        const auto size=e.bytes(); go3s::EventQueue queue(size*2);
        require(queue.push(e)&&queue.push(e)&&!queue.push(e),"queue overflow"); queue.close();
        require(queue.pop(e)&&queue.pop(e)&&!queue.pop(e),"accepted queue drain"); require(queue.high_water()==size*2,"queue accounting"); require(!queue.push(e),"closed queue push");
        int sockets[2]; require(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0,"socketpair");
        std::thread sender([&]{go3s::send_message(sockets[1],{{"v",1},{"type","stop"}}); close(sockets[1]);});
        require(go3s::receive_event(sockets[0],e,[] {return false;}) && e.header["type"]=="stop" && e.payload.empty(),"wire roundtrip");
        require(!go3s::receive_event(sockets[0],e,[]{return false;}),"clean EOF"); close(sockets[0]); sender.join();
        std::cout<<"Synthetic unit checks passed; no hardware used.\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
