//
// Created by awalol on 2026/3/29.
//

#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

#include "ALSARecord.h"
#include "BTHID.h"
#include "EventLoop.h"
#include "USBGadget.h"
#include "USBHID.h"
#include "log.h"

USBGadget gadget{};
BTHID bt{};
USBHID usb{};
ALSARecord recorder{};

ssize_t btSend(uint8_t* data, size_t size) { return bt.send(data, size); }

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

void forwardUsbFeatureReport(uint8_t reportId, const uint8_t* payload, size_t size) {
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

void audioTask(const std::stop_token& stop_token) {
    while (!stop_token.stop_requested()) {
        recorder.runOnce();
    }
    recorder.uninit();
}

namespace {
struct EventBusContext {
    EventLoop* loop = nullptr;
    int exitCode = 0;
};

void requestEventBusExit(EventBusContext* context, int exitCode, const char* message) {
    if (context->exitCode != 0) {
        return;
    }
    context->exitCode = exitCode;
    if (message != nullptr) {
        std::cerr << message << std::endl;
    }
    context->loop->breakLoop();
}

void handleUsbReadable(evutil_socket_t, short events, void* arg) {
    auto* context = static_cast<EventBusContext*>(arg);
    if ((events & EV_CLOSED) != 0) {
        requestEventBusExit(context, 4, "USB HID file descriptor reported hangup/error; exiting for service restart");
        return;
    }

    std::vector<std::uint8_t> data = usb.recv();
    if (data.empty()) {
        if (!usb.healthy()) {
            requestEventBusExit(context, 3, "USB HID became unhealthy; exiting for service restart");
        }
        return;
    }

    if (data.size() == 47) {
        bt.setStateData(data.data(), data.size());
        return;
    }
    if (data.size() == 63 && data[0] == 0x06) {
        forwardUsbFeatureReport(0x80, data.data(), data.size());
        return;
    }
    if (data[0] == 0x02) {
        bt.setStateData(data.data() + 1, data.size() - 1);
        return;
    }
    if (data[0] == 0x80) {
        data.resize(64);
        forwardUsbFeatureReport(0x80, data.data() + 1, data.size() - 1);
    }
}

void handleBtReadable(evutil_socket_t, short events, void* arg) {
    auto* context = static_cast<EventBusContext*>(arg);
    if ((events & EV_CLOSED) != 0) {
        requestEventBusExit(context, 4, "Bluetooth HID file descriptor reported hangup/error; exiting for service restart");
        return;
    }

    std::vector<std::uint8_t> data = bt.recv();
    if (data.empty()) {
        if (!bt.healthy()) {
            requestEventBusExit(context, 2, "Bluetooth HID became unhealthy; exiting for service restart");
        }
        return;
    }

    if (data.size() < 65) {
        return;
    }
    if (!is_full_bt_input_report(data.data(), data.size())) {
        return;
    }
    bt.handleMicButton(mic_button_pressed_from_bt_report31(data.data(), data.size()));
    static std::array<uint8_t, 16> lastBtControls = {};
    static bool haveLastBtControls = false;
    std::array<uint8_t, 16> btControls = {};
    memcpy(btControls.data(), data.data(), btControls.size());
    if (!haveLastBtControls || btControls != lastBtControls) {
        lastBtControls = btControls;
        haveLastBtControls = true;
    }
    bt.setHeadset(headset_connected_from_status(data[55]));
    memcpy(interrupt_data + 1, data.data() + 2, 63);
}

void handleUsbPeriodic(evutil_socket_t, short events, void* arg) {
    auto* context = static_cast<EventBusContext*>(arg);
    if (!bt.healthy()) {
        requestEventBusExit(context, 2, "Bluetooth HID became unhealthy; exiting for service restart");
        return;
    }
    if (!usb.healthy()) {
        requestEventBusExit(context, 3, "USB HID became unhealthy; exiting for service restart");
        return;
    }

    const ssize_t ret = usb.send(interrupt_data, 64);
    if (ret < 0 && !usb.healthy()) {
        requestEventBusExit(context, 3, "USB HID became unhealthy; exiting for service restart");
    }
}
}  // namespace

int event_bus() {
    EventBusContext context{};
    EventLoop loop;
    context.loop = &loop;
    if (!loop.valid()) {
        std::cerr << "event_base_new failed" << std::endl;
        return 1;
    }

    event* usbEvent = loop.createFdEvent(usb.get_fd(), EV_READ | EV_CLOSED, true, handleUsbReadable, &context);
    event* btEvent = loop.createFdEvent(bt.get_fd(), EV_READ | EV_CLOSED, true, handleBtReadable, &context);
    event* timerEvent = loop.createTimerEvent(true, handleUsbPeriodic, &context);

    if (usbEvent == nullptr || btEvent == nullptr || timerEvent == nullptr) {
        std::cerr << "event_new failed" << std::endl;
        loop.freeEvent(usbEvent);
        loop.freeEvent(btEvent);
        loop.freeEvent(timerEvent);
        return 1;
    }

    const timeval usbSendPeriod{.tv_sec = 0, .tv_usec = 4 * 1000};
    if (!loop.addEvent(usbEvent, nullptr) || !loop.addEvent(btEvent, nullptr) || !loop.addEvent(timerEvent, &usbSendPeriod)) {
        std::cerr << "event_add failed" << std::endl;
        loop.freeEvent(usbEvent);
        loop.freeEvent(btEvent);
        loop.freeEvent(timerEvent);
        return 1;
    }

    const int dispatchResult = loop.dispatch();
    if (dispatchResult == -1 && context.exitCode == 0) {
        std::cerr << "event_base_dispatch failed" << std::endl;
        context.exitCode = 1;
    }

    loop.freeEvent(usbEvent);
    loop.freeEvent(btEvent);
    loop.freeEvent(timerEvent);

    return context.exitCode == 0 ? 0 : context.exitCode;
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

    if (recorder.init() != 0) {
        return -1;
    }

    // Init DualSense

    if (auto ret = bt.sendInitialState(); !ret) {
        LOGE("sendInitialState");
    }

    std::cout << "Get Controller and Host MAC" << std::endl;
    auto report_0x09 = bt.get_feature_report(0x09, 20);
    auto ret = usb.set_get_report(0x09, report_0x09);
    printHex(report_0x09.data(), report_0x09.size());

    std::cout << "Get Controller Version/Data (Firmware Info)" << std::endl;
    auto report_0x20 = bt.get_feature_report(0x20, 64);
    ret = usb.set_get_report(0x20, report_0x20);
    printHex(report_0x20.data(), report_0x20.size());

    std::cout << "Get Hardware Info" << std::endl;
    auto report_0x22 = bt.get_feature_report(0x22, 64);
    ret = usb.set_get_report(0x22, report_0x22);
    printHex(report_0x22.data(), report_0x22.size());

    std::cout << "Get Calibration" << std::endl;
    auto report_0x05 = bt.get_feature_report(0x05, 41);
    ret = usb.set_get_report(0x05, report_0x05);
    printHex(report_0x05.data(), report_0x05.size());

    auto thread2 = std::jthread(audioTask);
    return event_bus();

    return 0;
}
