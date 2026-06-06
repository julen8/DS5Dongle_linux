//
// Created by awalol on 2026/3/30.
//

#include "BTHID.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ostream>
#include <queue>
#include <string>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>

#include "Utils.h"

#define BUFFER_LENGTH           64

namespace {
bool is_transient_errno(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}

bool mic_experiment_enabled() {
    const char* value = std::getenv("DS5_ENABLE_BT_MIC");
    return value && std::string(value) != "0";
}

bool mic_button_mute_enabled() {
    const char* value = std::getenv("DS5_BT_MIC_BUTTON_MUTE");
    return !value || std::string(value) != "0";
}

bool use_legacy_audio_packet() {
    const char* mode = std::getenv("DS5_AUDIO_PACKET_MODE");
    return mode && std::string(mode) == "legacy";
}

std::string audio_state_mode() {
    const char* mode = std::getenv("DS5_AUDIO_STATE_MODE");
    if (mode) {
        return std::string(mode);
    }
    return mic_experiment_enabled() ? std::string("pico-mic") : std::string("linux");
}

uint8_t clamp_byte(int value, int minValue, int maxValue) {
    return static_cast<uint8_t>(std::clamp(value, minValue, maxValue));
}

void set_mic_mute_fields(std::array<uint8_t, 63>& stateData, bool muted) {
    stateData[1] |= 0x03;
    stateData[8] = muted ? 0x01 : 0x00;
    if (muted) {
        stateData[9] |= 0x10;
    } else {
        stateData[9] &= static_cast<uint8_t>(~0x10);
    }
}
}

uint32_t crc32_output(const uint8_t *data, std::size_t size) {
    uint32_t crc = ~0xEADA2D49; // 0xA2 seed

    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; i++)
            crc = ((crc >> 1) ^ (0xEDB88320 & -(crc & 1)));
    }

    return ~crc;
}

uint32_t crc32_feature(const uint8_t *data, std::size_t size) {
    // https://github.com/rafaelvaloto/Dualsense-Multiplatform/blob/main/Source/Private/GCore/Utils/CR32.cpp
    uint32_t crc = ~0x2060efc3; // 0x53 seed

    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; i++)
            crc = ((crc >> 1) ^ (0xEDB88320 & -(crc & 1)));
    }

    return ~crc;
}

inline void fill_output_report_checksum(uint8_t *data, size_t len) {
    uint32_t crc = crc32_output(data, len - 4);
    data[len - 4] = (crc >> 0) & 0xFF;
    data[len - 3] = (crc >> 8) & 0xFF;
    data[len - 2] = (crc >> 16) & 0xFF;
    data[len - 1] = (crc >> 24) & 0xFF;
}

inline void fill_feature_report_checksum(uint8_t *data, const size_t len) {
    uint32_t crc = crc32_feature(data,len - 4);
    data[len - 4] = (crc >> 0) & 0xFF;
    data[len - 3] = (crc >> 8) & 0xFF;
    data[len - 2] = (crc >> 16) & 0xFF;
    data[len - 1] = (crc >> 24) & 0xFF;
}

int BTHID::init() {
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/hidraw")) {
        const std::string name = entry.path().filename().string();
        std::string ueventPath = entry.path().string() + "/device/uevent";
        std::ifstream uevent(ueventPath);
        std::string content((std::istreambuf_iterator<char>(uevent)), std::istreambuf_iterator<char>());
        if (content.find("HID_ID=0005:0000054C:") == std::string::npos) {
            continue;
        }

        std::string devPath = "/dev/" + name;
        fd = open(devPath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
            failed.store(false);
            std::cout << "BTHID device opened: " << devPath << std::endl;
            return 0;
        }
    }

    fd = -1;
    std::cerr << "Failed to open Sony Bluetooth HID device: " << strerror(errno) << std::endl;
    return -1;
}

// Auto fill crc32 at last 4 bytes
ssize_t BTHID::send(uint8_t *data, size_t size) const {
    if (fd < 0) {
        return 0;
    }
    fill_output_report_checksum(data, size);
    const ssize_t ret = write(fd, data, size);
    if (ret < 0 && !is_transient_errno(errno)) {
        failed.store(true);
        std::cerr << "BT send failed: " << strerror(errno) << std::endl;
    }
    return ret;
}

std::vector<std::uint8_t> BTHID::recv() const {
    std::vector<std::uint8_t> data(128);
    const long ret = read(fd, data.data(), data.size());
    if (ret < 0) {
        if (!is_transient_errno(errno)) {
            failed.store(true);
            std::cerr << "BT recv failed: " << strerror(errno) << std::endl;
        }
        data.clear();
        return data;
    }
    if (ret == 0) {
        failed.store(true);
        std::cerr << "BT recv reached EOF" << std::endl;
        data.clear();
        return data;
    }
    data.resize(std::ranges::max(0, (int)ret));
    return data;
}

void BTHID::setStateData(const uint8_t* data, size_t size) {
    const size_t stateSize = std::min(size, stateData.size());
    std::lock_guard lock(outputMutex);
    memcpy(stateData.data(), data, stateSize);
    applyUsbOutputReport(data, size);
    normalizeAudioStateLocked();

    sendStateReportLocked();
}

ssize_t BTHID::sendStateReportLocked() {
    uint8_t outputData[78] = {};
    outputData[0] = 0x31;
    outputData[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    outputData[2] = 0x10;
    memcpy(outputData + 3, stateData.data(), stateData.size());
    return send(outputData, sizeof(outputData));
}

void BTHID::normalizeAudioStateLocked() {
    const bool headsetConnected = headset.load(std::memory_order_relaxed);
    const uint8_t routeOverride = audioRouteOverride.load(std::memory_order_relaxed);
    const bool routeHeadset = routeOverride == 0x16 || (headsetConnected && routeOverride != 0x13);
    const bool muted = micMuted.load(std::memory_order_relaxed);
    const uint8_t hpVol = headsetVolume.load(std::memory_order_relaxed);
    const uint8_t spVol = speakerVolume.load(std::memory_order_relaxed);
    const uint8_t micVol = micVolume.load(std::memory_order_relaxed);
    const std::string mode = audio_state_mode();
    stateData[0] |= 0xe0;
    stateData[1] |= 0x83;
    if (mode == "pico" || mode == "pico-mic") {
        stateData[1] |= 0xf7;
        stateData[4] = hpVol;
        stateData[5] = routeHeadset ? 0x00 : std::max<uint8_t>(spVol, 0x7f);
        stateData[6] = std::max<uint8_t>(micVol, static_cast<uint8_t>(0x7c));
        stateData[7] = 0x09;
        set_mic_mute_fields(stateData, muted);
        stateData[37] = 0x0a;
        return;
    }

    if (mode == "gamepad-mic") {
        stateData[4] = std::min<uint8_t>(hpVol, 0x7f);
        stateData[5] = routeHeadset ? 0x00 : 0x7c;
        stateData[6] = micVol;
        stateData[7] = routeHeadset ? 0x00 : 0x31;
        set_mic_mute_fields(stateData, muted);
        stateData[37] = routeHeadset ? 0x00 : 0x02;
        return;
    }

    stateData[4] = std::min<uint8_t>(hpVol, 0x7f);
    stateData[5] = routeHeadset ? 0x00 : spVol;
    stateData[6] = micVol;
    stateData[7] = routeHeadset ? 0x00 : 0x30;
    set_mic_mute_fields(stateData, muted);
    stateData[37] = routeHeadset ? 0x00 : 0x02;
}

void BTHID::setHeadset(bool connected) {
    const bool previous = headset.exchange(connected, std::memory_order_relaxed);
    if (previous == connected) {
        return;
    }

    std::lock_guard lock(outputMutex);
    normalizeAudioStateLocked();

    sendStateReportLocked();

    std::cout << "BT audio route state=" << (connected ? "headset" : "speaker")
              << std::dec << std::endl;
}

void BTHID::setMicMuted(bool muted) {
    const bool previous = micMuted.exchange(muted, std::memory_order_relaxed);
    if (previous == muted) {
        return;
    }

    sendAudioControlState();
}

void BTHID::handleMicButton(bool pressed) {
    if (mic_experiment_enabled() && !mic_button_mute_enabled()) {
        lastMicButton.store(pressed, std::memory_order_relaxed);
        if (isMicMuted()) {
            setMicMuted(false);
        }
        return;
    }

    const bool previous = lastMicButton.exchange(pressed, std::memory_order_relaxed);
    if (pressed && !previous) {
        setMicMuted(!isMicMuted());
    }
}

void BTHID::sendAudioControlState() {
    std::lock_guard lock(outputMutex);
    normalizeAudioStateLocked();
    sendStateReportLocked();
    std::cout << "BT mic state=" << (micMuted.load(std::memory_order_relaxed) ? "muted" : "unmuted")
              << " micVolume=" << static_cast<int>(micVolume.load(std::memory_order_relaxed))
              << std::dec << std::endl;
}

ssize_t BTHID::sendBluetoothControlFeature(uint8_t state) const {
    uint8_t report[48] = {};
    report[0] = 0x08;
    report[1] = state;
    return send_feature_report(report, sizeof(report));
}

void BTHID::enableBluetoothMicExperiment() {
    if (!mic_experiment_enabled()) {
        return;
    }

    const ssize_t btRet = sendBluetoothControlFeature(0x01);

    micMuted.store(false, std::memory_order_relaxed);
    sendAudioControlState();
    std::cout << "BT mic experiment enabled feature08=" << btRet << std::dec << std::endl;
}

void BTHID::applyUsbAudioFeatureReport(const uint8_t* data, size_t size) {
    if (!data || size < 2) {
        return;
    }

    const uint8_t target = data[0];
    const uint8_t value = data[1];
    bool changed = false;

    if (target == 0x01) {
        speakerVolume.store(value, std::memory_order_relaxed);
        changed = true;
    } else if (target == 0x03) {
        micVolume.store(clamp_byte(value, 0, 0x40), std::memory_order_relaxed);
        changed = true;
    }

    if (changed) {
        sendAudioControlState();
    }
}

void BTHID::applyUsbOutputReport(const uint8_t* data, size_t size) {
    if (!data || size < 8) {
        return;
    }

    if (data[0] & (1 << 4)) {
        headsetVolume.store(data[4], std::memory_order_relaxed);
    }
    if (data[0] & (1 << 5)) {
        speakerVolume.store(data[5], std::memory_order_relaxed);
    }
    if (data[0] & (1 << 6)) {
        micVolume.store(data[6], std::memory_order_relaxed);
    }
}

void BTHID::setAudioRouteOverride(uint8_t route) {
    audioRouteOverride.store(route, std::memory_order_relaxed);
}

ssize_t BTHID::sendInitialState() {
    uint8_t report32[142] = {};
    ssize_t ret = 0;
    {
        std::lock_guard lock(outputMutex);
        normalizeAudioStateLocked();

        report32[0] = 0x32;
        report32[1] = 0x10;
        reportSeqCounter = 1;
        report32[2] = 0x10 | (1 << 7);
        report32[3] = static_cast<uint8_t>(stateData.size());
        memcpy(report32 + 4, stateData.data(), stateData.size());
        ret = send(report32, sizeof(report32));
    }
    if (mic_experiment_enabled()) {
        enableBluetoothMicExperiment();
    }
    return ret;
}

ssize_t BTHID::sendHaptics(const uint8_t *data) {
    uint8_t pkt[206] = {};
    std::lock_guard lock(outputMutex);
    pkt[0] = 0x33;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | (1 << 7);
    pkt[3] = 7;
    pkt[4] = 0b11111110;
    pkt[5] = BUFFER_LENGTH;
    pkt[6] = BUFFER_LENGTH;
    pkt[7] = BUFFER_LENGTH;
    pkt[8] = BUFFER_LENGTH;
    pkt[9] = BUFFER_LENGTH; // buffer length
    pkt[10] = packetCounter++;
    pkt[11] = 0x12 | (1 << 7);
    pkt[12] = 64;
    memcpy(pkt + 13, data, 64);
    return send(pkt, sizeof(pkt));
}

ssize_t BTHID::sendSpeaker(const uint8_t *data) {
    static uint8_t pkt[270] = {};
    std::lock_guard lock(outputMutex);
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x34;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 7;
    pkt[4] = 0b11111110;
    pkt[5] = BUFFER_LENGTH;
    pkt[6] = BUFFER_LENGTH;
    pkt[7] = BUFFER_LENGTH;
    pkt[8] = BUFFER_LENGTH;
    pkt[9] = BUFFER_LENGTH; // buffer length
    pkt[10] = packetCounter++;
    pkt[11] = 0x16 | 0 << 6 | 1 << 7; // Speaker: 0x13 Headset: 0x16
    pkt[12] = 200;
    memcpy(pkt + 13, data, 200);

    return send(pkt, sizeof(pkt));
}

ssize_t BTHID::sendCombine(const uint8_t *haptics, const uint8_t *speaker) {
    const uint8_t overrideRoute = audioRouteOverride.load(std::memory_order_relaxed);
    const uint8_t route = overrideRoute ? overrideRoute : (headset.load() ? 0x16 : 0x13);
    return sendCombineWithRoute(haptics, speaker, route);
}

ssize_t BTHID::sendCombineWithRoute(const uint8_t *haptics, const uint8_t *speaker, uint8_t route) {
    static uint8_t pkt[398] = {};
    std::lock_guard lock(outputMutex);
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x36;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 7;
    normalizeAudioStateLocked();
    const bool legacyPacket = use_legacy_audio_packet();
    pkt[4] = legacyPacket ? 0xff : 0xfe;
    const uint8_t bufferLength = legacyPacket ? 48 : BUFFER_LENGTH;
    pkt[5] = bufferLength;
    pkt[6] = bufferLength;
    pkt[7] = bufferLength;
    pkt[8] = bufferLength;
    pkt[9] = bufferLength; // audio buffer length
    pkt[10] = packetCounter++;
    route = route ? route : (headset.load() ? 0x16 : 0x13);
    if (legacyPacket) {
        pkt[11] = 0x12 | 1 << 7;
        pkt[12] = 64;
        memcpy(pkt + 13, haptics, 64);
        pkt[77] = route | 1 << 7;
        pkt[78] = 200;
        memcpy(pkt + 79, speaker, 200);
    } else {
        pkt[11] = 0x10 | 1 << 7;
        pkt[12] = stateData.size();
        memcpy(pkt + 13, stateData.data(), stateData.size());
        pkt[76] = 0x12 | 1 << 7;
        pkt[77] = 64;
        memcpy(pkt + 78, haptics, 64);
        pkt[142] = route | 1 << 7;
        pkt[143] = 200;
        memcpy(pkt + 144, speaker, 200);
    }
    // std::cout << "sendCombine" << std::endl;
    // Utils::print_hex(pkt, sizeof(pkt));
    const ssize_t ret = send(pkt, sizeof(pkt));

    return ret;
}

ssize_t BTHID::send_feature_report(uint8_t *data, const size_t size) const {
    fill_feature_report_checksum(data, size);
    const auto res = ioctl(fd, HIDIOCSFEATURE(size), data);
    if (res < 0) {
        if (!is_transient_errno(errno)) {
            failed.store(true);
        }
        perror("send_feature_report");
    }
    return res;
}

std::vector<uint8_t> BTHID::get_feature_report(const uint8_t reportId, const size_t maxLength) const {
    std::vector<uint8_t> buf(maxLength);
    buf[0] = reportId;
    const auto res = ioctl(fd, HIDIOCGFEATURE(maxLength), buf.data());
    if (res < 0) {
        if (!is_transient_errno(errno)) {
            failed.store(true);
        }
        perror("get_feature_report");
    }
    return buf;
}
