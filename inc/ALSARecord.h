//
// Created by awalol on 2026/3/30.
//

#pragma once

#include <alsa/asoundlib.h>

#include <memory>
#include <string>

#include "BTHID.h"

class ALSARecord {
private:
    bool opened = false;
    snd_pcm_t* audioHandle = nullptr;
    snd_pcm_t* micHandle = nullptr;
    int micPlaybackChannels = 1;
    BTHID& bt;
    std::unique_ptr<struct pollfd> audioPollFds = nullptr;
    int audioNfds = 0;
    std::string findUacCaptureDevice();
    static int xrunRecovery(snd_pcm_t* handle, int err);

public:
    int init();
    void uninit();
    // return: read frames
    size_t read(int16_t* buffer, size_t frames) const;
    size_t writeMic(const int16_t* buffer, size_t frames) const;
    bool audioLoop();
    ALSARecord(BTHID& bt) : bt(bt) {}
    ~ALSARecord() = default;
};
