#include "DebugHIDInput.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>

namespace {
bool read_text(const std::filesystem::path& path, std::string& out) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool transient_errno(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

bool is_mic_input_report(const std::vector<uint8_t>& data) {
    if (data.size() >= 78 && data[0] == 0x31) {
        return (data[1] & 0x02) != 0;
    }
    if (data.size() >= 79 && data[0] == 0xa1 && data[1] == 0x31) {
        return (data[2] & 0x02) != 0;
    }
    return false;
}

bool is_full_input_report(const std::vector<uint8_t>& data) {
    if (data.size() >= 78 && data[0] == 0x31) {
        return (data[1] & 0x0f) == 0x01;
    }
    if (data.size() >= 79 && data[0] == 0xa1 && data[1] == 0x31) {
        return (data[2] & 0x0f) == 0x01;
    }
    return false;
}

size_t mic_payload_offset(const std::vector<uint8_t>& data) {
    if (data.size() >= 79 && data[0] == 0xa1 && data[1] == 0x31) {
        return 4;
    }
    return 3;
}
}

std::string DebugHIDInput::findEventsPath() {
    constexpr const char* debugRoot = "/sys/kernel/debug/hid";
    mount("debugfs", "/sys/kernel/debug", "debugfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr);

    if (!std::filesystem::exists(debugRoot)) {
        return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(debugRoot)) {
        const std::string name = entry.path().filename().string();
        if (name.find("0005:054C:0CE6") == std::string::npos) {
            continue;
        }
        const auto events = entry.path() / "events";
        if (std::filesystem::exists(events)) {
            return events.string();
        }
    }
    return {};
}

std::vector<uint8_t> DebugHIDInput::parseReportLine(const std::string& line) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
        return {};
    }

    std::vector<uint8_t> data;
    std::istringstream stream(line.substr(equals + 1));
    std::string byteText;
    while (stream >> byteText) {
        if (byteText.size() > 2) {
            continue;
        }
        char* end = nullptr;
        const long value = strtol(byteText.c_str(), &end, 16);
        if (end != byteText.c_str() && *end == '\0' && value >= 0 && value <= 0xff) {
            data.push_back(static_cast<uint8_t>(value));
        }
    }
    return data;
}

int DebugHIDInput::init() {
    const std::string path = findEventsPath();
    if (path.empty()) {
        std::cerr << "DualSense debug HID events not found" << std::endl;
        return -1;
    }

    fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open DualSense debug HID events: " << strerror(errno) << std::endl;
        return -1;
    }

    failed = false;
    std::cout << "DualSense debug HID events opened: " << path << std::endl;
    return 0;
}

bool DebugHIDInput::drain(
    uint8_t* report,
    size_t size,
    const std::function<void(const uint8_t*, size_t)>& micPacket
) {
    if (fd < 0) {
        return false;
    }

    char buffer[4096];
    bool changed = false;
    while (true) {
        const ssize_t ret = read(fd, buffer, sizeof(buffer));
        if (ret > 0) {
            pending.append(buffer, ret);
            size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (line.find("report ") == std::string::npos ||
                    line.find("31 ") == std::string::npos) {
                    continue;
                }

                const auto data = parseReportLine(line);
                if (data.size() >= 65 && data[0] == 0x31 && size >= 64) {
                    if (is_mic_input_report(data)) {
                        if (micPacket) {
                            const size_t offset = mic_payload_offset(data);
                            micPacket(data.data() + offset, data.size() - offset);
                        }
                        continue;
                    }
                    if (!is_full_input_report(data)) {
                        continue;
                    }
                    report[0] = 0x01;
                    memcpy(report + 1, data.data() + 2, 63);
                    changed = true;
                }
            }
            continue;
        }

        if (ret < 0 && transient_errno(errno)) {
            break;
        }

        if (ret < 0) {
            failed = true;
            std::cerr << "DualSense debug HID read failed: " << strerror(errno) << std::endl;
        } else {
            failed = true;
            std::cerr << "DualSense debug HID reached EOF" << std::endl;
        }
        break;
    }
    return changed;
}
