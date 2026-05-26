//
// Created by awalol on 2026/3/30.
//

#ifndef DS5_DONGLE_LINUX_ALSARECORD_H
#define DS5_DONGLE_LINUX_ALSARECORD_H
#include <atomic>
#include <string>
#include <alsa/asoundlib.h>

#include "BTHID.h"
#include "opus.h"
#include "resample.h"

class ALSARecord {
private:
    bool opened = false;
    snd_pcm_t *handle = nullptr;
    snd_pcm_t *playbackHandle = nullptr;
    int micPlaybackChannels = 1;
    WDL_Resampler resampler;
    OpusEncoder *opus = nullptr;
    OpusDecoder *micOpus = nullptr;
    BTHID& bt;
    std::atomic_bool waveOutActive = false;
    std::atomic<uint8_t> waveOutRoute = 0x13;
    void haptics_proc(int16_t* data,ssize_t frames);
    void mic_keepalive_proc();
    void speaker_proc(int16_t* data,ssize_t frames);
    void waveout_proc();
    std::string find_uac_capture_device();
public:
    int init();
    // return: read frames
    ssize_t read(int16_t* buffer, snd_pcm_uframes_t frames) const;
    ssize_t writeMic(const int16_t* buffer, snd_pcm_uframes_t frames) const;
    void mic_add_packet(const uint8_t* data, size_t size);
    void setWaveOut(bool enabled, uint8_t route);
    void audio_loop();
    ALSARecord(BTHID& bt) : bt(bt) {}
};



#endif //DS5_DONGLE_LINUX_ALSARECORD_H
