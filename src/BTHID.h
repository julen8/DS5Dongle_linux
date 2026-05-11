//
// Created by awalol on 2026/3/30.
//

#ifndef DS5_DONGLE_LINUX_BTHID_H
#define DS5_DONGLE_LINUX_BTHID_H
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <sys/types.h>


namespace std {
    class stop_token;
}

class BTHID {
private:
    int fd = -1;
    mutable std::atomic_bool failed = false;
    std::atomic_bool headset = false;
    std::atomic_bool micMuted = false;
    std::atomic_bool lastMicButton = false;
    std::atomic<uint8_t> headsetVolume = 0x7f;
    std::atomic<uint8_t> speakerVolume = 0x7f;
    std::atomic<uint8_t> micVolume = 0xff;
    std::atomic<uint8_t> audioRouteOverride = 0;
    std::atomic<uint64_t> audioActiveUntilMs = 0;
    mutable std::mutex outputMutex;
    std::array<uint8_t, 63> stateData = {
        0xfd, 0xf7, 0x00, 0x00,
        0x7f, 0x7f,
        0xff, 0x09, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
        0x07, 0x00, 0x00, 0x02, 0x01,
        0x00,
        0xff, 0xd7, 0x00,
    };
    int reportSeqCounter = 0;
    uint8_t packetCounter = 0;
    void normalizeAudioStateLocked();
    ssize_t sendStateReportLocked();
    ssize_t sendBluetoothControlFeature(uint8_t state) const;
public:
    int init();
    int get_fd() const { return fd; }
    bool healthy() const { return fd >= 0 && !failed.load(); }
    void setHeadset(bool connected);
    void setMicMuted(bool muted);
    bool isMicMuted() const { return micMuted.load(std::memory_order_relaxed); }
    void handleMicButton(bool pressed);
    void sendAudioControlState();
    void enableBluetoothMicExperiment();
    void applyUsbAudioFeatureReport(const uint8_t* data, size_t size);
    void applyUsbOutputReport(const uint8_t* data, size_t size);
    void setAudioRouteOverride(uint8_t route);
    void markAudioActive();
    bool audioActive() const;
    ssize_t sendInitialState();
    ssize_t send(uint8_t* data, size_t size) const;
    std::vector<std::uint8_t> recv() const;
    void setStateData(const uint8_t* data, size_t size);
    ssize_t sendHaptics(const uint8_t* data);
    ssize_t sendSpeaker(const uint8_t* data);
    ssize_t sendCombine(const uint8_t* haptics,const uint8_t* speaker);
    ssize_t sendCombineWithRoute(const uint8_t* haptics, const uint8_t* speaker, uint8_t route);
    ssize_t send_feature_report(uint8_t* data,size_t size) const;
    std::vector<std::uint8_t> get_feature_report(uint8_t reportId, size_t maxLength) const;
};



#endif //DS5_DONGLE_LINUX_BTHID_H
