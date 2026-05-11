//
// Created by awalol on 2026/3/30.
//

#include "USBHID.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <ostream>
#include <string>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {
bool is_transient_errno(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR ||
        error == EPIPE || error == ESHUTDOWN || error == ENODEV;
}

bool is_unsupported_get_report_errno(int error) {
    return error == ENOTTY || error == EINVAL || error == ENOSYS;
}
}

#if __has_include(<linux/usb/g_hid.h>)
#include <linux/usb/g_hid.h>
#else
#define MAX_REPORT_LENGTH 64

struct usb_hidg_report {
    uint8_t report_id;
    uint8_t userspace_req;
    uint16_t length;
    uint8_t data[MAX_REPORT_LENGTH];
    uint8_t padding[4];
};

#define GADGET_HID_READ_GET_REPORT_ID _IOR('g', 0x41, uint8_t)
#define GADGET_HID_WRITE_GET_REPORT _IOW('g', 0x42, struct usb_hidg_report)
#endif

int USBHID::init() {
    for (int index = 0; index < 16; ++index) {
        std::string path = "/dev/hidg" + std::to_string(index);
        fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            failed.store(false);
            std::cout << "Gadget HID device opened: " << path << std::endl;
            return 0;
        }
    }

    std::cerr << "Failed to open any Gadget HID device: " << strerror(errno) << std::endl;
    fd = -1;
    return -1;
}

ssize_t USBHID::send(uint8_t* data, size_t size) const {
    if (fd < 0) {
        return 0;
    }
    const ssize_t ret = write(fd, data, size);
    if (ret < 0 && !is_transient_errno(errno)) {
        std::cerr << "USB send failed: " << strerror(errno) << std::endl;
    }
    return ret;
}

std::vector<std::uint8_t> USBHID::recv() const {
    std::vector<std::uint8_t> data(128);
    const long ret = read(fd,data.data(), data.size());
    if (ret < 0) {
        if (!is_transient_errno(errno)) {
            failed.store(true);
            std::cerr << "USB recv failed: " << strerror(errno) << std::endl;
        }
        data.clear();
        return data;
    }
    data.resize(std::ranges::max(0, (int)ret));
    return data;
}

ssize_t USBHID::set_get_report(uint8_t reportId, const std::vector<uint8_t> &data) const {
    usb_hidg_report report{};
    report.report_id = reportId;
    report.userspace_req = 0;
    report.length = std::min(data.size(), static_cast<size_t>(MAX_REPORT_LENGTH));
    memcpy(report.data, data.data(), report.length);
    const ssize_t ret = ioctl(fd, GADGET_HID_WRITE_GET_REPORT, &report);
    if (ret < 0) {
        if (!is_transient_errno(errno) && !is_unsupported_get_report_errno(errno)) {
            failed.store(true);
        }
        perror("set_get_report");
    }
    return ret;
}
