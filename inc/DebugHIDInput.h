#ifndef DS5_DONGLE_LINUX_DEBUGHIDINPUT_H
#define DS5_DONGLE_LINUX_DEBUGHIDINPUT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class DebugHIDInput {
private:
    int fd = -1;
    bool failed = false;
    std::string pending;

    static std::string findEventsPath();
    static std::vector<uint8_t> parseReportLine(const std::string& line);

public:
    int init();
    bool available() const { return fd >= 0 && !failed; }
    bool healthy() const { return !failed; }
    bool drain(uint8_t* report, size_t size, const std::function<void(const uint8_t*, size_t)>& micPacket = {});
};

#endif  // DS5_DONGLE_LINUX_DEBUGHIDINPUT_H
