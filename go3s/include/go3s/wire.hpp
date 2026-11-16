#pragma once
#include "go3s/config.hpp"
#include <cstdint>
#include <functional>
#include <vector>
namespace go3s {
int64_t steady_ns();
struct Event {
    Json header;
    std::vector<uint8_t> payload;
    int64_t host_receive_ns = 0;
    size_t bytes() const { return payload.size() + header.dump().size() + 8; }
};
// Returns false only for clean socket EOF before a new message. Partial EOF throws.
bool receive_event(int fd, Event& event, const std::function<bool()>& expired);
void send_message(int fd, const Json& header);
class Sequence {
public:
    void accept(const Json& header);
private:
    uint64_t next_ = 0;
};
}
