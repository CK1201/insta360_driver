#pragma once
#include <string>
namespace go3s {
// Validates Android API/ABI and excludes camera identities even for explicit serials.
std::string select_phone(const std::string& requested_serial);
void forward_phone(const std::string& serial, int port);
void remove_forward(const std::string& serial, int port);
int connect_phone(int port);
}
