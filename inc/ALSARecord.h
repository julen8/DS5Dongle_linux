//
// Created by awalol on 2026/3/30.
//

#pragma once

#include <alsa/asoundlib.h>

#include <memory>
#include <string>

class ALSARecord {
public:
    ALSARecord() = default;
    ~ALSARecord();

    int init();
    void uninit();
    // return: read frames
    size_t read(int16_t *buffer, size_t frames) const;
    size_t writeMic(const int16_t *buffer, size_t frames) const;
    bool runOnce();

private:
    static std::string findUacCaptureDevice();
    std::string pcmToCtlName();
    static void xrunRecovery(snd_pcm_t *handle, long err);
    void handleCtlEvents();
    static void onItfChanged(bool audioActive, long rate);
    bool getAudioActive(long *rate);

    bool opened = false;
    snd_pcm_t *audioHandle = nullptr;
    snd_ctl_t *ctlHandle = nullptr;
    snd_pcm_t *micHandle = nullptr;
    int micPlaybackChannels = 1;
    std::unique_ptr<struct pollfd> pollFds = nullptr;
    int audioPcmNfds = 0;
    int audioCtlNfds = 0;
};
