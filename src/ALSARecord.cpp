//
// Created by awalol on 2026/3/29.
//

#include "ALSARecord.h"

#include <alsa/error.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "Utils.h"
#include "audio.h"
#include "config.h"
#include "defer.h"
#include "log.h"
#include "resample.h"

constexpr int kMicChannels = 1;

int ALSARecord::init() {
    const auto sndName = findUacCaptureDevice();

    // audio
    int ret = snd_pcm_open(&audioHandle, sndName.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        LOGE("snd_pcm_open err:%s", snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_set_params(audioHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 4, 48000, 1, 50 * 1000);
    if (ret < 0) {
        LOGE("Failed to set PCM parameters: %s", snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_prepare(audioHandle);
    if (ret < 0) {
        LOGE("Failed to prepare PCM: %s", snd_strerror(ret));
        return ret;
    }
    ret = snd_pcm_start(audioHandle);
    if (ret < 0) {
        LOGE("Failed to start PCM: %s", snd_strerror(ret));
        return ret;
    }

    audioNfds = snd_pcm_poll_descriptors_count(audioHandle);
    if (audioNfds <= 0) {
        LOGE("poll_descriptors_count = %d", audioNfds);
        ret = -1;
        return ret;
    }
    struct pollfd* pfds = static_cast<struct pollfd*>(calloc(audioNfds, sizeof(struct pollfd)));
    if (pfds == nullptr) {
        ret = -1;
        return ret;
    }
    audioPollFds = std::unique_ptr<struct pollfd>(pfds);
    defer {
        if (ret != 0) {
            audioPollFds = nullptr;
        }
    };

    ret = snd_pcm_poll_descriptors(audioHandle, audioPollFds.get(), audioNfds);
    if (ret < 0) {
        LOGE("snd_pcm_poll_descriptors: %s", snd_strerror(ret));
        return ret;
    }

    // mic
    ret = snd_pcm_open(&micHandle, sndName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (ret < 0) {
        LOGE("Failed to open mic PCM device: %s", snd_strerror(ret));
        return ret;
    }
    ret = snd_pcm_set_params(micHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, kMicChannels, 48000, 1, 20 * 1000);
    if (ret < 0) {
        LOGE("Failed to set mono mic PCM parameters, retry stereo: %s", snd_strerror(ret));
        ret = snd_pcm_set_params(micHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 20 * 1000);
        if (ret < 0) {
            LOGE("Failed to set mic PCM parameters: %s", snd_strerror(ret));
            return ret;
        }
        micPlaybackChannels = 2;
    } else {
        micPlaybackChannels = 1;
    }

    audioInit([this](int16_t* buffer, size_t frames) -> size_t { return this->read(buffer, frames); });
    opened = true;
    return 0;
}

void ALSARecord::uninit() {
    if (audioHandle != nullptr) {
        snd_pcm_close(audioHandle);
    }
    if (micHandle != nullptr) {
        snd_pcm_close(micHandle);
    }
    audioCleanup();
}

size_t ALSARecord::read(int16_t* buffer, size_t frames) const {
    if (!opened) {
        return 0;
    }
    ssize_t ret = snd_pcm_readi(audioHandle, buffer, frames);
    if (ret < 0) {
        if (ret == -EAGAIN) {
            return 0;
        }
        snd_pcm_abort(audioHandle);
        snd_pcm_prepare(audioHandle);
        snd_pcm_start(audioHandle);
        return 0;
    }
    return ret;
}

size_t ALSARecord::writeMic(const int16_t* buffer, size_t frames) const {
    if (!micHandle) {
        return 0;
    }

    ssize_t ret = snd_pcm_writei(micHandle, buffer, frames);
    if (ret < 0) {
        if (ret != -EAGAIN) {
            snd_pcm_abort(micHandle);
            snd_pcm_prepare(micHandle);
        }
    }
    return ret;
}

int ALSARecord::xrunRecovery(snd_pcm_t* handle, int err) {
    if (err == -EAGAIN) {
        return 0;
    }

    snd_pcm_abort(handle);
    snd_pcm_prepare(handle);
    snd_pcm_start(handle);
    return 0;
}

bool ALSARecord::audioLoop() {
    int ret = poll(audioPollFds.get(), audioNfds, 100);
    if (ret < 0) {
        if (errno == EINTR) {
            return true;
        }

        LOGE("poll : %d", errno);
        return false;
    }
    if (ret == 0) {
        LOGD("poll timeout");
        return true;
    }

    /* 关键: 把 revents 翻译成 ALSA 事件 */
    unsigned short revents = 0;
    snd_pcm_poll_descriptors_revents(audioHandle, audioPollFds.get(), audioNfds, &revents);

    if (revents & POLLERR) {
        // 触发 XRUN 等, 走恢复路径
        return xrunRecovery(audioHandle, -EPIPE) >= 0;
    }
    if ((revents & POLLIN) == 0) {
        return true;  // 还没数据
    }

    /* 数据就绪, 读一个周期 */
    config.audioActive = true;
    audioLoop();
    config.audioActive = false;
    return true;
}

std::string ALSARecord::findUacCaptureDevice() {
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char* name = nullptr;
        if (snd_card_get_name(card, &name) >= 0 && name) {
            std::string card_name(name);
            free(name);
            LOGI("snd card name:%s", card_name.c_str());
            if (card_name.find("UAC2") != std::string::npos || card_name.find("Gadget") != std::string::npos ||
                card_name.find("gadget") != std::string::npos || card_name.find("USB") != std::string::npos) {
                char device[32];
                snprintf(device, sizeof(device), "hw:%d,0", card);
                LOGI("[Audio] Found UAC gadget ALSA device: %s (%s)", device, card_name.c_str());
                return std::string(device);
            }
        }
    }

    LOGI("[Audio] No UAC gadget card found, trying hw:0,0");
    return "hw:0,0";
}
