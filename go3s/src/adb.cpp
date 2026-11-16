#include "go3s/adb.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace go3s {
namespace {
std::string run(const std::vector<std::string>& args) {
    int pipefd[2]; if (pipe(pipefd)) throw std::runtime_error("cannot create adb output pipe");
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); throw std::runtime_error("cannot fork adb"); }
    if (!pid) {
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        std::vector<char*> argv; for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str())); argv.push_back(nullptr);
        execvp(argv[0],argv.data()); _exit(127);
    }
    close(pipefd[1]); std::string output; char data[4096]; ssize_t n;
    while ((n=read(pipefd[0],data,sizeof(data))) != 0) { if(n<0) { if(errno==EINTR) continue; break; } output.append(data,static_cast<size_t>(n)); }
    close(pipefd[0]); int status; while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("adb failed: " + output);
    while(!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) output.pop_back();
    return output;
}
std::string prop(const std::string& serial,const char* property) { return run({"adb","-s",serial,"shell","getprop",property}); }
bool valid(const std::string& serial) {
    auto identity = prop(serial,"ro.product.manufacturer") + " " + prop(serial,"ro.product.model") + " " + prop(serial,"ro.product.name");
    std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char c){return std::tolower(c);});
    if (identity.find("insta360") != std::string::npos || identity.find("go3") != std::string::npos || identity.find("go 3") != std::string::npos) return false;
    const auto api = prop(serial,"ro.build.version.sdk");
    const auto abi = prop(serial,"ro.product.cpu.abilist");
    if (api.empty() || !std::all_of(api.begin(),api.end(),[](unsigned char c){return std::isdigit(c);})) return false;
    return std::stoi(api) >= 28 && abi.find("arm64-v8a") != std::string::npos;
}
}
std::string select_phone(const std::string& requested) {
    std::istringstream lines(run({"adb","devices"})); std::string line; std::vector<std::string> eligible;
    while(std::getline(lines,line)) {
        std::istringstream row(line); std::string serial,state; row>>serial>>state;
        if(state!="device" || (!requested.empty() && serial!=requested)) continue;
        if(valid(serial)) eligible.push_back(serial);
    }
    if(eligible.size()!=1) throw std::runtime_error("need exactly one authorized Android API>=28 arm64 phone (camera ADB excluded); use bridge.adb_serial if multiple phones exist");
    return eligible.front();
}
void forward_phone(const std::string& serial,int port) { run({"adb","-s",serial,"forward","--no-rebind","tcp:"+std::to_string(port),"tcp:18765"}); }
void remove_forward(const std::string& serial,int port) { run({"adb","-s",serial,"forward","--remove","tcp:"+std::to_string(port)}); }
int connect_phone(int port) {
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) throw std::runtime_error("cannot open bridge socket");
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(static_cast<uint16_t>(port)); inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))) { auto reason=std::string(std::strerror(errno)); close(fd); throw std::runtime_error("cannot connect phone bridge: "+reason); }
    return fd;
}
}
