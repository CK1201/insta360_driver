#include "go3s/wire.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
namespace go3s {
int64_t steady_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
namespace {
bool exact(int fd, void* dst, size_t size, bool eof_ok, const std::function<bool()>& expired) {
    auto* out = static_cast<uint8_t*>(dst); size_t done = 0;
    while (done < size) {
        if (expired && expired()) throw std::runtime_error("bridge receive deadline expired");
        pollfd p{fd, POLLIN, 0}; int r = poll(&p, 1, 100);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
        if (!r) continue;
        ssize_t n = recv(fd, out + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error(std::string("recv: ") + std::strerror(errno));
        if (!n) { if (!done && eof_ok) return false; throw std::runtime_error("incomplete bridge message at EOF"); }
        done += static_cast<size_t>(n);
    }
    return true;
}
}
bool receive_event(int fd, Event& e, const std::function<bool()>& expired) {
    uint32_t lengths[2];
    if (!exact(fd, lengths, sizeof(lengths), true, expired)) return false;
    auto h = ntohl(lengths[0]), p = ntohl(lengths[1]);
    if (!h || h > 1024u * 1024u || p > 32u * 1024u * 1024u) throw std::runtime_error("invalid bridge message lengths");
    std::string header(h, '\0'); e.payload.resize(p);
    exact(fd, header.data(), h, false, expired); exact(fd, e.payload.data(), p, false, expired);
    e.host_receive_ns = steady_ns(); e.header = Json::parse(header);
    if (!e.header.is_object() || !e.header.contains("v") || e.header["v"] != 1 ||
        !e.header.contains("type") || !e.header["type"].is_string()) throw std::runtime_error("invalid bridge v1 JSON header");
    return true;
}
void send_message(int fd, const Json& header) {
    auto json = header.dump(); uint32_t lengths[2] = {htonl(static_cast<uint32_t>(json.size())),0};
    std::vector<uint8_t> bytes(sizeof(lengths) + json.size());
    std::memcpy(bytes.data(), lengths, sizeof(lengths)); std::memcpy(bytes.data()+sizeof(lengths), json.data(),json.size());
    size_t done = 0;
    while (done < bytes.size()) {
        auto n = send(fd, bytes.data()+done,bytes.size()-done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error(std::string("send: ")+std::strerror(errno));
        done += static_cast<size_t>(n);
    }
}
void Sequence::accept(const Json& h) {
    if (!h.contains("seq") || !h["seq"].is_number_unsigned()) throw std::runtime_error("phone seq must be uint64");
    const uint64_t seq = h["seq"].get<uint64_t>();
    if (seq != next_) throw std::runtime_error("phone sequence gap: expected " + std::to_string(next_) + ", got " + std::to_string(seq));
    if (next_ == UINT64_MAX) throw std::runtime_error("phone sequence exhausted");
    ++next_;
}
}
