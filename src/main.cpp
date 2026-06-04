//
// Created by awalol on 2026/3/29.
//

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "ALSARecord.h"
#include "BTHID.h"
#include "DebugHIDInput.h"
#include "USBGadget.h"
#include "USBHID.h"
#include "Utils.h"

USBGadget gadget;
BTHID bt;
DebugHIDInput debugInput;
USBHID usb;
ALSARecord recorder(bt);

bool find_bt_report31(const uint8_t* data, size_t size, size_t& offset) {
    if (size >= 78 && data[0] == 0x31) {
        offset = 0;
        return true;
    }
    if (size >= 79 && data[0] == 0xa1 && data[1] == 0x31) {
        offset = 1;
        return true;
    }
    return false;
}

bool is_mic_input_report(const uint8_t* data, size_t size, size_t& payloadOffset) {
    size_t reportOffset = 0;
    if (!find_bt_report31(data, size, reportOffset)) {
        return false;
    }

    payloadOffset = reportOffset + 3;
    return size >= payloadOffset + 71 && (data[reportOffset + 1] & 0x02) != 0;
}

bool is_mic_input_report(const uint8_t* data, size_t size) {
    size_t payloadOffset = 0;
    return is_mic_input_report(data, size, payloadOffset);
}

bool is_mic_input_report(const std::vector<std::uint8_t>& data) { return is_mic_input_report(data.data(), data.size()); }

bool is_full_bt_input_report(const uint8_t* data, size_t size) {
    size_t reportOffset = 0;
    if (!find_bt_report31(data, size, reportOffset)) {
        return false;
    }

    return (data[reportOffset + 1] & 0x01) != 0;
}

uint8_t interrupt_data[64] = {0x01, 0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7, 0x08, 0x00, 0x00, 0x00, 0x52, 0x43, 0x30, 0x41,
                              0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03, 0x03, 0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00,
                              0xfc, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00, 0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b};

void log_usb_input_controls();

bool headset_connected_from_status(uint8_t status) { return (status & 0x01) != 0; }

bool mic_button_pressed_from_usb_report(const uint8_t* data, size_t size) { return size > 10 && (data[10] & 0x04) != 0; }

bool mic_button_pressed_from_bt_report31(const uint8_t* data, size_t size) {
    if (size >= 12 && data[0] == 0x31) {
        return (data[11] & 0x04) != 0;
    }
    if (size >= 13 && data[0] == 0xa1 && data[1] == 0x31) {
        return (data[12] & 0x04) != 0;
    }
    return false;
}

void sync_usb_mic_mute_status() {
    if (bt.isMicMuted()) {
        interrupt_data[54] |= 0x04;
    } else {
        interrupt_data[54] &= static_cast<uint8_t>(~0x04);
    }
}

bool hid_no_out_endpoint_enabled() {
    const char* value = std::getenv("DS5_HID_NO_OUT_ENDPOINT");
    return value && std::string(value) != "0";
}

void log_usb_raw(const std::vector<std::uint8_t>& data) {
    static int count = 0;
    if (count++ >= 80) {
        return;
    }

    std::cout << "USB RAW len=" << data.size() << " data=";
    const size_t limit = std::min(data.size(), static_cast<size_t>(32));
    for (size_t index = 0; index < limit; ++index) {
        printf("%02x ", data[index]);
    }
    std::cout << std::endl;
}

void forward_usb_feature_report(uint8_t reportId, const uint8_t* payload, size_t size) {
    std::vector<uint8_t> report(size + 1);
    report[0] = reportId;
    if (size > 0) {
        memcpy(report.data() + 1, payload, size);
    }
    report.resize(64);
    bt.applyUsbAudioFeatureReport(report.data() + 1, report.size() - 1);
    bt.send_feature_report(report.data(), report.size());
    if (auto ret = bt.get_feature_report(0x81, 64); !ret.empty()) {
        usb.set_get_report(0x81, ret);
    }
}

void apply_short_input_report(const std::vector<std::uint8_t>& data) {
    if (data.size() != 10 || data[0] != 0x01) {
        return;
    }

    memcpy(interrupt_data + 1, data.data() + 1, 6);
    interrupt_data[7] = 0x01;
    memcpy(interrupt_data + 8, data.data() + 7, 3);
    log_usb_input_controls();
}

void log_usb_input_controls() {
    static std::array<uint8_t, 9> lastControls = {};
    static int logCount = 0;
    std::array<uint8_t, 9> controls = {};
    memcpy(controls.data(), interrupt_data + 1, 6);
    memcpy(controls.data() + 6, interrupt_data + 8, 3);
    if (logCount < 80 && controls != lastControls) {
        std::cout << "USB INPUT axes/buttons=";
        for (uint8_t byte : controls) {
            printf("%02x ", byte);
        }
        std::cout << "status=" << std::hex << static_cast<int>(interrupt_data[54]) << std::dec << std::endl;
        lastControls = controls;
        logCount++;
    }
}

void log_bt_report_shape(const std::vector<std::uint8_t>& data) {
    static int logCount = 0;
    if (logCount >= 80) {
        return;
    }

    size_t reportOffset = 0;
    if (!find_bt_report31(data.data(), data.size(), reportOffset) && !(data.size() == 10 && data[0] == 0x01)) {
        return;
    }

    std::cout << "BT REPORT len=" << data.size();
    if (find_bt_report31(data.data(), data.size(), reportOffset)) {
        std::cout << " flags=0x" << std::hex << static_cast<int>(data[reportOffset + 1])
                  << " mic=" << ((data[reportOffset + 1] & 0x02) ? 1 : 0) << " input=" << ((data[reportOffset + 1] & 0x01) ? 1 : 0)
                  << std::dec;
    }
    std::cout << std::endl;
    logCount++;
}

void audio_task(const std::stop_token& stop_token) {
    while (!stop_token.stop_requested()) {
        recorder.audio_loop();
    }
}

int event_bus() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }
    epoll_event events[3];

    epoll_event usb_event{};
    usb_event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    usb_event.data.fd = usb.get_fd();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, usb.get_fd(), &usb_event) != 0) {
        perror("epoll_ctl usb");
        close(epoll_fd);
        return 1;
    }

    epoll_event bt_event{};
    bt_event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    bt_event.data.fd = bt.get_fd();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, bt.get_fd(), &bt_event) != 0) {
        perror("epoll_ctl bt");
        close(epoll_fd);
        return 1;
    }

    constexpr auto kUsbSendPeriod = std::chrono::milliseconds(4);
    auto nextSendTime = std::chrono::steady_clock::now() + kUsbSendPeriod;
    while (true) {
        if (!bt.healthy()) {
            std::cerr << "Bluetooth HID became unhealthy; exiting for service restart" << std::endl;
            close(epoll_fd);
            return 2;
        }
        if (!usb.healthy()) {
            std::cerr << "USB HID became unhealthy; exiting for service restart" << std::endl;
            close(epoll_fd);
            return 3;
        }
        if (debugInput.available() && !debugInput.healthy()) {
            std::cerr << "DualSense debug HID became unhealthy; exiting for service restart" << std::endl;
            close(epoll_fd);
            return 6;
        }

        if (debugInput.available() && debugInput.drain(interrupt_data, sizeof(interrupt_data),
                                                       [](const uint8_t* data, size_t size) { recorder.mic_add_packet(data, size); })) {
            bt.setHeadset(headset_connected_from_status(interrupt_data[54]));
            bt.handleMicButton(mic_button_pressed_from_usb_report(interrupt_data, sizeof(interrupt_data)));
            log_usb_input_controls();
        }

        auto now = std::chrono::steady_clock::now();

        if (now >= nextSendTime) {
            sync_usb_mic_mute_status();
            usb.send(interrupt_data, 64);
            nextSendTime += kUsbSendPeriod;
        }

        int timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(nextSendTime - now).count();
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }

        int num_events = epoll_wait(epoll_fd, events, 3, timeout_ms);
        if (num_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            close(epoll_fd);
            return 1;
        }

        for (int i = 0; i < num_events; i++) {
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (events[i].data.fd == bt.get_fd()) {
                    std::cerr << "Bluetooth HID file descriptor reported hangup/error; exiting for service restart" << std::endl;
                    close(epoll_fd);
                    return 4;
                }
                if (events[i].events & EPOLLERR) {
                    std::cerr << "USB HID file descriptor reported error; exiting for service restart" << std::endl;
                    close(epoll_fd);
                    return 4;
                }
            }

            if (events[i].data.fd == usb.get_fd()) {
                // USB SetReport / Interrupt OUT
                std::vector<std::uint8_t> data = usb.recv();
                if (data.empty()) {
                    continue;
                }
                log_usb_raw(data);
                if (data.size() == 47) {
                    bt.setStateData(data.data(), data.size());

                    continue;
                }
                if (data.size() == 63 && data[0] == 0x06) {
                    forward_usb_feature_report(0x80, data.data(), data.size());
                    continue;
                }
                if (hid_no_out_endpoint_enabled()) {
                    if (data.size() == 47) {
                        bt.setStateData(data.data(), data.size());

                        continue;
                    }
                    if (data.size() == 63) {
                        forward_usb_feature_report(0x80, data.data(), data.size());
                        continue;
                    }
                }
                if (data[0] == 0x02) {
                    bt.setStateData(data.data() + 1, data.size() - 1);

                    continue;
                }
                if (data[0] == 0x80) {
                    data.resize(64);

                    forward_usb_feature_report(0x80, data.data() + 1, data.size() - 1);
                }
            } else if (events[i].data.fd == bt.get_fd()) {
                // 接收蓝牙的状态数据
                std::vector<std::uint8_t> data = bt.recv();
                if (data.empty()) {
                    continue;
                }
                log_bt_report_shape(data);
                static int btRawLogCount = 0;
                if (btRawLogCount < 20) {
                    std::cout << "BT RAW len=" << data.size() << " data=";
                    const size_t limit = std::min(data.size(), static_cast<size_t>(20));
                    for (size_t index = 0; index < limit; ++index) {
                        printf("%02x ", data[index]);
                    }
                    std::cout << std::endl;
                    btRawLogCount++;
                }
                if (data.size() == 10 && data[0] == 0x01) {
                    if (!debugInput.available()) {
                        apply_short_input_report(data);
                    }
                    continue;
                }
                if (data.size() < 65) {
                    continue;
                }
                size_t micPayloadOffset = 0;
                if (is_mic_input_report(data.data(), data.size(), micPayloadOffset)) {
                    recorder.mic_add_packet(data.data() + micPayloadOffset, data.size() - micPayloadOffset);
                }
                if (!is_full_bt_input_report(data.data(), data.size())) {
                    continue;
                }
                bt.handleMicButton(mic_button_pressed_from_bt_report31(data.data(), data.size()));
                static std::array<uint8_t, 16> lastBtControls = {};
                static bool haveLastBtControls = false;
                std::array<uint8_t, 16> btControls = {};
                memcpy(btControls.data(), data.data(), btControls.size());
                if (!haveLastBtControls || btControls != lastBtControls) {
                    std::cout << "BT INPUT controls=";
                    for (uint8_t byte : btControls) {
                        printf("%02x ", byte);
                    }
                    std::cout << std::endl;
                    lastBtControls = btControls;
                    haveLastBtControls = true;
                }
                bt.setHeadset(headset_connected_from_status(data[55]));
                memcpy(interrupt_data + 1, data.data() + 2, 63);
                sync_usb_mic_mute_status();
            }
        }
    }
}

int main() {
    if (gadget.exists()) {
        gadget.destroy();
    }

    if (!gadget.exists()) {
        gadget.destroy();
        if (!gadget.create()) {
            return -1;
        }
    }

    if (usb.init() != 0) {
        return -1;
    }

    if (bt.init() != 0) {
        return -1;
    }

    debugInput.init();

    if (recorder.init() != 0) {
        return -1;
    }

    // Init DualSense

    auto ret = bt.sendInitialState();

    std::cout << "Get Controller and Host MAC" << std::endl;
    auto report_0x09 = bt.get_feature_report(0x09, 20);
    ret = usb.set_get_report(0x09, report_0x09);
    Utils::print_hex(report_0x09);

    std::cout << "Get Controller Version/Data (Firmware Info)" << std::endl;
    auto report_0x20 = bt.get_feature_report(0x20, 64);
    ret = usb.set_get_report(0x20, report_0x20);
    Utils::print_hex(report_0x20);

    std::cout << "Get Hardware Info" << std::endl;
    auto report_0x22 = bt.get_feature_report(0x22, 64);
    ret = usb.set_get_report(0x22, report_0x22);
    Utils::print_hex(report_0x22);

    std::cout << "Get Calibration" << std::endl;
    auto report_0x05 = bt.get_feature_report(0x05, 41);
    ret = usb.set_get_report(0x05, report_0x05);
    Utils::print_hex(report_0x05);

    auto thread2 = std::jthread(audio_task);
    return event_bus();

    return 0;
}
