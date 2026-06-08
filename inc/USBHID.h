//
// Created by awalol on 2026/3/30.
//

#ifndef DS5_DONGLE_LINUX_USBHID_H
#define DS5_DONGLE_LINUX_USBHID_H

#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <vector>

class USBHID {
private:
    int fd = -1;
    mutable std::atomic_bool failed = false;

public:
    int init();
    int get_fd() const { return fd; }
    bool healthy() const { return fd >= 0 && !failed.load(); }
    ssize_t send(uint8_t* data, size_t size) const;
    std::vector<uint8_t> recv() const;
    ssize_t set_get_report(uint8_t reportId, const std::vector<uint8_t>& data) const;
};

#endif  // DS5_DONGLE_LINUX_USBHID_H
