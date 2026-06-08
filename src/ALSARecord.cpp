//
// Created by awalol on 2026/3/29.
//

#include "ALSARecord.h"

#include <alsa/error.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "audio.h"
#include "config.h"
#include "defer.h"
#include "log.h"
#include "resample.h"

constexpr int kMicChannels = 1;
constexpr auto ctlName = "Capture Rate";  // playback 方向则用 "Playback Rate"

ALSARecord::~ALSARecord() {
    if (opened) {
        uninit();
    }
}

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

    const auto ctlCardName = pcmToCtlName();
    if (ctlCardName.empty()) {
        LOGE("pcmToCtlName");
        return 1;
    }
    ret = snd_ctl_open(&ctlHandle, ctlCardName.c_str(), SND_CTL_NONBLOCK);
    if (ret < 0) {
        LOGE("ctl_open(%s): %s", ctlCardName.c_str(), snd_strerror(ret));
        return ret;
    }
    ret = snd_ctl_subscribe_events(ctlHandle, 1);
    if (ret < 0) {
        LOGE("subscribe_events: %s", snd_strerror(ret));
        return ret;
    }

    audioCtlNfds = snd_ctl_poll_descriptors_count(ctlHandle);
    audioPcmNfds = snd_pcm_poll_descriptors_count(audioHandle);
    if (audioPcmNfds <= 0) {
        LOGE("poll_descriptors_count = %d", audioPcmNfds);
        ret = -1;
        return ret;
    }

    auto *pfds = static_cast<struct pollfd *>(calloc(audioPcmNfds + audioCtlNfds, sizeof(struct pollfd)));
    if (pfds == nullptr) {
        ret = -1;
        return ret;
    }
    pollFds = std::unique_ptr<struct pollfd>(pfds);
    defer {
        if (ret != 0) {
            pollFds = nullptr;
        }
    };

    ret = snd_pcm_poll_descriptors(audioHandle, pollFds.get(), audioPcmNfds);
    if (ret < 0) {
        LOGE("snd_pcm_poll_descriptors: %s", snd_strerror(ret));
        return ret;
    }
    ret = snd_ctl_poll_descriptors(ctlHandle, &((pollFds.get())[audioPcmNfds]), audioCtlNfds);
    if (ret < 0) {
        LOGE("snd_ctl_poll_descriptors: %s", snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_start(audioHandle);
    if (ret < 0) {
        LOGE("Failed to start PCM: %s", snd_strerror(ret));
        return ret;
    }

    {
        // 上电先读一次当前状态
        long rate = 0;
        if (getAudioActive(&rate)) {
            onItfChanged(rate > 0, rate);
        }
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

    audioInit([this](int16_t *buffer, size_t frames) -> size_t { return this->read(buffer, frames); });
    opened = true;
    return 0;
}

void ALSARecord::uninit() {
    opened = false;
    if (audioHandle != nullptr) {
        snd_pcm_close(audioHandle);
    }
    if (micHandle != nullptr) {
        snd_pcm_close(micHandle);
    }
    if (ctlHandle != nullptr) {
        snd_ctl_close(ctlHandle);
    }
    audioCleanup();
}

size_t ALSARecord::read(int16_t *buffer, size_t frames) const {
    if (!opened) {
        return 0;
    }
    ssize_t ret = snd_pcm_readi(audioHandle, buffer, frames);
    if (ret < 0) {
        if (ret == -EAGAIN) {
            return 0;
        }
        xrunRecovery(audioHandle, ret);
        return 0;
    }
    return ret;
}

size_t ALSARecord::writeMic(const int16_t *buffer, size_t frames) const {
    if (micHandle == nullptr) {
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

bool ALSARecord::runOnce() {
    const int ret = poll(pollFds.get(), audioPcmNfds + audioCtlNfds, 100);
    if (ret < 0) {
        if (errno == EINTR) {
            return true;
        }

        LOGE("poll : %d", errno);
        return false;
    }
    if (ret == 0) {
        return true;
    }

    {
        unsigned short ctlRevents = 0;
        snd_ctl_poll_descriptors_revents(ctlHandle, &((pollFds.get())[audioPcmNfds]), audioCtlNfds, &ctlRevents);
        if (ctlRevents & (POLLIN | POLLERR)) {
            handleCtlEvents();
        }
    }

    unsigned short revents = 0;
    snd_pcm_poll_descriptors_revents(audioHandle, pollFds.get(), audioPcmNfds, &revents);

    if ((revents & POLLERR) != 0) {
        xrunRecovery(audioHandle, -EPIPE);
        return true;
    }
    if ((revents & POLLIN) != 0) {
        ::audioLoop();
    }

    return true;
}

std::string ALSARecord::findUacCaptureDevice() {
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char *name = nullptr;
        if (snd_card_get_name(card, &name) >= 0 && name != nullptr) {
            std::string cardName(name);
            free(name);
            LOGI("snd card name:%s", cardName.c_str());
            if (cardName.find("UAC2") != std::string::npos || cardName.find("Gadget") != std::string::npos ||
                cardName.find("gadget") != std::string::npos || cardName.find("USB") != std::string::npos) {
                char device[32];
                snprintf(device, sizeof(device), "hw:%d,0", card);
                LOGI("[Audio] Found UAC gadget ALSA device: %s (%s)", device, cardName.c_str());
                return std::string(device);
            }
        }
    }

    LOGI("[Audio] No UAC gadget card found, trying hw:0,0");
    return "hw:0,0";
}

std::string ALSARecord::pcmToCtlName() {
    snd_pcm_info_t *info = nullptr;
    snd_pcm_info_alloca(&info);
    if (const int err = snd_pcm_info(audioHandle, info); err < 0) {
        LOGE("snd_pcm_info_alloca: %s", snd_strerror(err));
        return std::string();
    }

    const int card = snd_pcm_info_get_card(info);
    if (card < 0) {
        LOGE("snd_pcm_info_get_card: %s", snd_strerror(card));
        return std::string();
    }

    return "hw:" + std::to_string(card);
}

void ALSARecord::xrunRecovery(snd_pcm_t *handle, long err) {
    if (err == -EAGAIN) {
        return;
    }

    snd_pcm_abort(handle);
    snd_pcm_prepare(handle);
    snd_pcm_start(handle);
}

void ALSARecord::handleCtlEvents() {
    snd_ctl_event_t *event = nullptr;
    snd_ctl_event_alloca(&event);

    int err = -1;
    long rate = 0;
    /* 非阻塞模式下, 把队列里的事件全部读完 */
    while ((err = snd_ctl_read(ctlHandle, event)) > 0) {
        if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) {
            continue;
        }

        if (const auto *name = snd_ctl_event_elem_get_name(event); name == nullptr || strcmp(name, ctlName) != 0) {
            continue;
        }

        /* 只关心 VALUE 变化; 也可能收到 INFO/ADD/REMOVE 等 */
        if (const auto mask = snd_ctl_event_elem_get_mask(event); (mask & SND_CTL_EVENT_MASK_VALUE) == 0U) {
            continue;
        }

        if (getAudioActive(&rate)) {
            onItfChanged(rate > 0, rate);
        }
    }
    if (err < 0 && err != -EAGAIN) {
        LOGE("snd_ctl_read: %s", snd_strerror(err));
    }
}

void ALSARecord::onItfChanged(bool audioActive, long rate) {
    config.audioActive = audioActive;
    if (audioActive) {
        if (rate != 48000) {
            LOGE("snd_ctl_elem_value_set_rate: %ld", rate);
        }
        LOGD("[AUDIO] host set alt!=0 -> Speaker active, rate=%ld", rate);
    } else {
        LOGD("[AUDIO] host set alt=0  -> Speaker inactive");
    }
}

bool ALSARecord::getAudioActive(long *rate) {
    snd_ctl_elem_id_t *id = nullptr;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_t *val = nullptr;
    snd_ctl_elem_value_alloca(&val);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_name(id, ctlName);
    snd_ctl_elem_value_set_id(val, id);
    if (const auto ret = snd_ctl_elem_read(ctlHandle, val); ret < 0) {
        LOGE("snd_ctl_elem_read:%d", ret);
        return false;
    }

    *rate = snd_ctl_elem_value_get_integer(val, 0);

    return true;
}
