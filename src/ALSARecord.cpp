//
// Created by awalol on 2026/3/29.
//

#include "ALSARecord.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/ioctl.h>
#include <unistd.h>
#include <alsa/error.h>
#include "resample.h"
#include "Utils.h"

namespace {
constexpr int kMicOpusSize = 71;
constexpr int kMicFrames = 480;
constexpr int kMicChannels = 1;
constexpr float kWaveOutFrequency = 1000.0f;
constexpr float kWaveOutLevel = 0.30f;
constexpr auto kMicKeepalivePeriod = std::chrono::microseconds(10666);
constexpr auto kWaveOutPeriod = std::chrono::milliseconds(10);

uint8_t speaker_data[200];
uint8_t zero_haptics[64];

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string(value) != "0";
}

struct MixerValue {
    bool ok = false;
    long value = 0;
    long min = 0;
    long max = 0;
    const char* name = "";
};

MixerValue read_mixer_value(const char* name) {
    MixerValue result;
    result.name = name;

    snd_ctl_t* ctl = nullptr;
    if (snd_ctl_open(&ctl, "hw:0", 0) < 0) {
        return result;
    }

    snd_ctl_elem_id_t* id;
    snd_ctl_elem_info_t* info;
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);

    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_info_set_id(info, id);

    if (snd_ctl_elem_info(ctl, info) >= 0 &&
        snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_INTEGER &&
        snd_ctl_elem_info_get_count(info) > 0) {
        snd_ctl_elem_value_set_id(value, id);
        if (snd_ctl_elem_read(ctl, value) >= 0) {
            result.ok = true;
            result.value = snd_ctl_elem_value_get_integer(value, 0);
            result.min = snd_ctl_elem_info_get_min(info);
            result.max = snd_ctl_elem_info_get_max(info);
        }
    }

    snd_ctl_close(ctl);
    return result;
}

float mixer_value_to_gain(const MixerValue& volume) {
    if (!volume.ok) {
        return 2.0f;
    }

    if (volume.min >= 0 && volume.max <= 2) {
        return std::clamp(static_cast<float>(volume.value), 1.0f, 2.0f);
    }

    if (volume.min >= 0 && volume.max <= 512) {
        return std::clamp(static_cast<float>(volume.value) / 256.0f, 1.0f, 2.0f);
    }

    if (volume.max > volume.min) {
        const float normalized = static_cast<float>(volume.value - volume.min) /
            static_cast<float>(volume.max - volume.min);
        return 1.0f + std::clamp(normalized, 0.0f, 1.0f);
    }

    return 2.0f;
}

MixerValue read_haptics_volume() {
    MixerValue mic_volume = read_mixer_value("PCM Playback Volume");
    if (mic_volume.ok && mic_volume.min >= 0 && mic_volume.max <= 512) {
        return mic_volume;
    }

    MixerValue speaker_volume = read_mixer_value("PCM Capture Volume");
    if (speaker_volume.ok) {
        return speaker_volume;
    }

    return mic_volume;
}
}

int ALSARecord::init() {
    int ret = snd_pcm_open(&handle, "hw:0,0", SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (ret < 0) {
        std::cerr << "Failed to open PCM device: " << snd_strerror(ret) << std::endl;
        return ret;
    }
    ret = snd_pcm_set_params(
        handle,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        4,
        48000,
        1,
        50 * 1000
    );
    if (ret < 0) {
        std::cerr << "Failed to set PCM parameters: " << snd_strerror(ret) << std::endl;
        return ret;
    }

    ret = snd_pcm_prepare(handle);
    if (ret < 0) {
        std::cerr << "Failed to prepare PCM: " << snd_strerror(ret) << std::endl;
        return ret;
    }
    ret = snd_pcm_start(handle);
    if (ret < 0) {
        std::cerr << "Failed to start PCM: " << snd_strerror(ret) << std::endl;
        return ret;
    }

    ret = snd_pcm_open(&playbackHandle, "hw:0,0", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (ret < 0) {
        std::cerr << "Failed to open mic PCM device: " << snd_strerror(ret) << std::endl;
        return ret;
    }
    ret = snd_pcm_set_params(
        playbackHandle,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        kMicChannels,
        48000,
        1,
        20 * 1000
    );
    if (ret < 0) {
        std::cerr << "Failed to set mono mic PCM parameters, retry stereo: "
                  << snd_strerror(ret) << std::endl;
        ret = snd_pcm_set_params(
            playbackHandle,
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            2,
            48000,
            1,
            20 * 1000
        );
        if (ret < 0) {
            std::cerr << "Failed to set mic PCM parameters: " << snd_strerror(ret) << std::endl;
            return ret;
        }
        micPlaybackChannels = 2;
    } else {
        micPlaybackChannels = 1;
    }

    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 24, 6);

    int error = 0;
    opus = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        std::cerr << "Failed to create opus encoder: " << error << std::endl;
        return error;
    }
    opus_encoder_ctl(opus, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(opus, OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(opus, OPUS_SET_VBR(false));
    opus_encoder_ctl(opus, OPUS_SET_COMPLEXITY(0));

    micOpus = opus_decoder_create(48000, kMicChannels, &error);
    if (error != OPUS_OK) {
        std::cerr << "Failed to create mic opus decoder: " << error << std::endl;
        return error;
    }

    opened = true;
    return 0;
}

ssize_t ALSARecord::read(int16_t* buffer, snd_pcm_uframes_t frames) const {
    if (!opened) {
        return 0;
    }
    ssize_t ret = snd_pcm_readi(handle, buffer, frames);
    if (ret < 0) {
        if (ret == -EAGAIN) {
            return 0;
        }
        snd_pcm_abort(handle);
        snd_pcm_prepare(handle);
        snd_pcm_start(handle);
        return 0;
    }
    return ret;
}

ssize_t ALSARecord::writeMic(const int16_t* buffer, snd_pcm_uframes_t frames) const {
    if (!playbackHandle) {
        return 0;
    }

    ssize_t ret = snd_pcm_writei(playbackHandle, buffer, frames);
    if (ret < 0) {
        if (ret != -EAGAIN) {
            snd_pcm_abort(playbackHandle);
            snd_pcm_prepare(playbackHandle);
        }
    }
    return ret;
}

void ALSARecord::mic_add_packet(const uint8_t* data, size_t size) {
    static std::mutex micMutex;
    std::lock_guard lock(micMutex);
    static auto lastLog = std::chrono::steady_clock::now();
    static unsigned packets = 0;
    static unsigned decodedPackets = 0;
    static int peak = 0;

    if (!micOpus || size < kMicOpusSize) {
        return;
    }
    packets++;

    int16_t decoded[kMicFrames * kMicChannels] = {};
    const int decodedFrames = opus_decode(
        micOpus,
        data,
        kMicOpusSize,
        decoded,
        kMicFrames,
        0
    );
    if (decodedFrames <= 0) {
        static unsigned decodeErrors = 0;
        if (decodeErrors++ < 20) {
            std::cerr << "Mic opus decode failed: " << decodedFrames << std::endl;
        }
        return;
    }

    for (int frame = 0; frame < decodedFrames; ++frame) {
        peak = std::max(peak, std::abs((int)decoded[frame]));
    }
    decodedPackets++;

    if (bt.isMicMuted()) {
        memset(decoded, 0, decodedFrames * kMicChannels * sizeof(int16_t));
    }
    if (micPlaybackChannels == 1) {
        writeMic(decoded, decodedFrames);
    } else {
        int16_t stereo[kMicFrames * 2] = {};
        for (int frame = 0; frame < decodedFrames; ++frame) {
            stereo[frame * 2] = decoded[frame];
            stereo[frame * 2 + 1] = decoded[frame];
        }
        writeMic(stereo, decodedFrames);
    }

    auto now = std::chrono::steady_clock::now();
    if (now - lastLog >= std::chrono::seconds(1)) {
        std::cout << std::dec << "MIC packets=" << packets
                  << " decoded=" << decodedPackets
                  << " peak=" << peak << std::endl;
        packets = 0;
        decodedPackets = 0;
        peak = 0;
        lastLog = now;
    }
}

void ALSARecord::audio_loop() {
    if (waveOutActive.load(std::memory_order_relaxed)) {
        waveout_proc();
        mic_keepalive_proc();
        return;
    }

    static int16_t buffer[32 * 4] = {};
    auto frames = read(buffer, 32);
    if (frames > 0) {
        speaker_proc(buffer, frames);
        haptics_proc(buffer, frames);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    mic_keepalive_proc();
}

void ALSARecord::setWaveOut(bool enabled, uint8_t route) {
    waveOutRoute.store(route ? route : 0x13, std::memory_order_relaxed);
    waveOutActive.store(enabled, std::memory_order_relaxed);
}

void ALSARecord::waveout_proc() {
    static auto next_send = std::chrono::steady_clock::now();
    static float phase = 0.0f;
    static auto last_log = std::chrono::steady_clock::now();
    static unsigned packets = 0;
    static unsigned errors = 0;

    auto now = std::chrono::steady_clock::now();
    if (now < next_send) {
        std::this_thread::sleep_until(next_send);
        return;
    }
    if (now - next_send > std::chrono::milliseconds(100)) {
        next_send = now;
    }
    next_send += kWaveOutPeriod;

    float samples[480 * 2];
    const float step = 2.0f * static_cast<float>(M_PI) * kWaveOutFrequency / 48000.0f;
    for (int frame = 0; frame < 480; ++frame) {
        const float value = std::sin(phase) * kWaveOutLevel;
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
        phase += step;
        if (phase >= 2.0f * static_cast<float>(M_PI)) {
            phase -= 2.0f * static_cast<float>(M_PI);
        }
    }

    const int encodedBytes = opus_encode_float(
        opus,
        samples,
        480,
        speaker_data,
        sizeof(speaker_data)
    );
    if (encodedBytes <= 0) {
        errors++;
        return;
    }
    if (encodedBytes < static_cast<int>(sizeof(speaker_data))) {
        memset(speaker_data + encodedBytes, 0, sizeof(speaker_data) - encodedBytes);
    }

    bt.markAudioActive();
    bt.sendCombineWithRoute(zero_haptics, speaker_data, waveOutRoute.load(std::memory_order_relaxed));
    packets++;

    if (now - last_log >= std::chrono::seconds(1)) {
        std::cout << std::dec << "WAVEOUT route=0x" << std::hex
                  << static_cast<int>(waveOutRoute.load(std::memory_order_relaxed))
                  << std::dec << " packets=" << packets
                  << " errors=" << errors << std::endl;
        packets = 0;
        errors = 0;
        last_log = now;
    }
}

void ALSARecord::haptics_proc(int16_t* data, ssize_t frames) {
    static auto last_log = std::chrono::steady_clock::now();
    static int log_peak_l = 0;
    static int log_peak_r = 0;
    static unsigned log_packets = 0;
    static MixerValue haptics_volume = read_haptics_volume();
    static auto last_gain_read = std::chrono::steady_clock::now();

    WDL_ResampleSample* in_buf;
    int nframes = resampler.ResamplePrepare(frames, 2, &in_buf);

    for (int i = 0; i < nframes; i++) {
        log_peak_l = std::max(log_peak_l, std::abs((int)data[i * 4 + 2]));
        log_peak_r = std::max(log_peak_r, std::abs((int)data[i * 4 + 3]));
        in_buf[i * 2] = (WDL_ResampleSample)(data[i * 4 + 2] / 32768.0f);
        in_buf[i * 2 + 1] = (WDL_ResampleSample)(data[i * 4 + 3] / 32768.0f);
    }

    WDL_ResampleSample out_buf[64];
    int out_frames = resampler.ResampleOut(out_buf, nframes, nframes / 4, 2);
    static int8_t haptics_buf[64];
    static int haptics_buf_pos = 0;

    auto now = std::chrono::steady_clock::now();
    if (now - last_gain_read >= std::chrono::seconds(1)) {
        haptics_volume = read_haptics_volume();
        last_gain_read = now;
    }

    for (int i = 0; i < out_frames; i++) {
        const float haptics_gain = mixer_value_to_gain(haptics_volume);
        int val_l = (int)(out_buf[i * 2] * 127.0f * haptics_gain);
        int val_r = (int)(out_buf[i * 2 + 1] * 127.0f * haptics_gain);
        haptics_buf[haptics_buf_pos++] = (int8_t)std::clamp(val_l, -128, 127);
        haptics_buf[haptics_buf_pos++] = (int8_t)std::clamp(val_r, -128, 127);

        if (haptics_buf_pos != 64) {
            continue;
        }
        bt.markAudioActive();
        bt.sendCombine(reinterpret_cast<const uint8_t*>(haptics_buf), speaker_data);
        log_packets++;
        haptics_buf_pos = 0;
    }

    if (now - last_log >= std::chrono::seconds(1)) {
        std::cout << std::dec << "HAPTIC peak=" << log_peak_l << "," << log_peak_r
                  << " gain=" << mixer_value_to_gain(haptics_volume)
                  << " volume=" << haptics_volume.name << ":" << haptics_volume.value
                  << "/" << haptics_volume.min << ".." << haptics_volume.max
                  << " packets=" << log_packets << std::endl;
        log_peak_l = 0;
        log_peak_r = 0;
        log_packets = 0;
        last_log = now;
    }
}

void ALSARecord::mic_keepalive_proc() {
    if (!env_enabled("DS5_ENABLE_BT_MIC") || !opus) {
        return;
    }

    static auto next_send = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next_send) {
        return;
    }
    next_send = now + kMicKeepalivePeriod;

    if (bt.audioActive()) {
        return;
    }

    float silence[480 * 2] = {};
    const int encodedBytes = opus_encode_float(
        opus,
        silence,
        480,
        speaker_data,
        sizeof(speaker_data)
    );
    if (encodedBytes <= 0) {
        static unsigned errors = 0;
        if (errors++ < 20) {
            std::cerr << "Mic keepalive opus encode failed: " << encodedBytes << std::endl;
        }
        return;
    }
    if (encodedBytes < static_cast<int>(sizeof(speaker_data))) {
        memset(speaker_data + encodedBytes, 0, sizeof(speaker_data) - encodedBytes);
    }

    bt.sendCombine(zero_haptics, speaker_data);
}

void ALSARecord::speaker_proc(int16_t* data, ssize_t frames) {
    static WDL_Resampler speaker_resampler;
    static bool speaker_resampler_ready = false;
    static WDL_ResampleSample speaker_buf[512 * 2];
    static int speaker_buf_pos = 0;
    static auto last_log = std::chrono::steady_clock::now();
    static int log_peak_l = 0;
    static int log_peak_r = 0;
    static int log_encoded_bytes = 0;
    static unsigned log_packets = 0;

    if (!speaker_resampler_ready) {
        speaker_resampler.SetMode(true, 0, false);
        speaker_resampler.SetRates(51200, 48000);
        speaker_resampler.SetFeedMode(true);
        speaker_resampler.Prealloc(2, 512, 480);
        speaker_resampler_ready = true;
    }

    for (int i = 0; i < frames; i++) {
        log_peak_l = std::max(log_peak_l, std::abs((int)data[i * 4]));
        log_peak_r = std::max(log_peak_r, std::abs((int)data[i * 4 + 1]));
        speaker_buf[speaker_buf_pos++] = static_cast<WDL_ResampleSample>(data[i * 4] / 32768.0f);
        speaker_buf[speaker_buf_pos++] = static_cast<WDL_ResampleSample>(data[i * 4 + 1] / 32768.0f);

        if (speaker_buf_pos != 512 * 2) {
            continue;
        }

        WDL_ResampleSample* in_buf;
        const int nframes = speaker_resampler.ResamplePrepare(512, 2, &in_buf);
        memcpy(in_buf, speaker_buf, nframes * 2 * sizeof(WDL_ResampleSample));

        WDL_ResampleSample resampled_buf[480 * 2];
        speaker_resampler.ResampleOut(resampled_buf, nframes, 480, 2);
        float out_buf[480 * 2];
        for (int sample = 0; sample < 480 * 2; ++sample) {
            out_buf[sample] = static_cast<float>(resampled_buf[sample]);
        }

        const int encodedBytes = opus_encode_float(
            opus,
            out_buf,
            480,
            speaker_data,
            200
        );
        if (encodedBytes > 0) {
            log_encoded_bytes = std::max(log_encoded_bytes, encodedBytes);
            log_packets++;
        }
        speaker_buf_pos = 0;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(1)) {
        std::cout << std::dec << "SPEAKER peak=" << log_peak_l << "," << log_peak_r
                  << " encodedBytes=" << log_encoded_bytes
                  << " packets=" << log_packets << std::endl;
        log_peak_l = 0;
        log_peak_r = 0;
        log_encoded_bytes = 0;
        log_packets = 0;
        last_log = now;
    }
}
