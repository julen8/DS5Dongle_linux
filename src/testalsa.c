// 编译: gcc alsa_poll_capture.c -o alsa_poll_capture -lasound
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <alsa/asoundlib.h>

#define PCM_DEVICE      "default"   // 也可以用 "hw:0,0" / "plughw:1,0"
#define SAMPLE_RATE     16000
#define CHANNELS        1
#define FORMAT          SND_PCM_FORMAT_S16_LE
#define PERIOD_FRAMES   1024        // 一个周期(period)的帧数, poll 每 period 唤醒一次

static int xrun_recovery(snd_pcm_t *handle, int err)
{
    if (err == -EPIPE) {            // overrun
        fprintf(stderr, "overrun, recovering...\n");
        err = snd_pcm_prepare(handle);
    } else if (err == -ESTRPIPE) {  // suspended
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            usleep(100 * 1000);
        if (err < 0) err = snd_pcm_prepare(handle);
    }
    return err;
}

int main(void)
{
    snd_pcm_t *pcm = NULL;
    int err;

    /* 1. 打开 capture 设备 (非阻塞模式, 配合 poll) */
    err = snd_pcm_open(&pcm, PCM_DEVICE, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open: %s\n", snd_strerror(err));
        return 1;
    }

    /* 2. 简便参数配置 */
    err = snd_pcm_set_params(pcm,
                             FORMAT,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             CHANNELS,
                             SAMPLE_RATE,
                             1,            // soft_resample
                             100 * 1000);  // latency 100ms
    if (err < 0) {
        fprintf(stderr, "snd_pcm_set_params: %s\n", snd_strerror(err));
        goto out;
    }

    /* 3. 获取 ALSA 需要 poll 的 fd 数量 */
    int nfds = snd_pcm_poll_descriptors_count(pcm);
    if (nfds <= 0) {
        fprintf(stderr, "poll_descriptors_count = %d\n", nfds);
        goto out;
    }
    struct pollfd *pfds = calloc(nfds, sizeof(struct pollfd));

    /* 4. 把 ALSA 的 fd 填进 pollfd */
    err = snd_pcm_poll_descriptors(pcm, pfds, nfds);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_poll_descriptors: %s\n", snd_strerror(err));
        free(pfds);
        goto out;
    }

    /* 5. 启动数据流 */
    if ((err = snd_pcm_start(pcm)) < 0) {
        fprintf(stderr, "snd_pcm_start: %s\n", snd_strerror(err));
        free(pfds);
        goto out;
    }

    /* 6. 主循环: poll -> revents -> readi */
    short buffer[PERIOD_FRAMES * CHANNELS];
    int frame_bytes = CHANNELS * snd_pcm_format_width(FORMAT) / 8;

    for (int loop = 0; loop < 500; loop++) {        // 演示读 500 个周期
        int ret = poll(pfds, nfds, 1000);           // 1s 超时
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (ret == 0) {
            fprintf(stderr, "poll timeout\n");
            continue;
        }

        /* 关键: 把 revents 翻译成 ALSA 事件 */
        unsigned short revents = 0;
        snd_pcm_poll_descriptors_revents(pcm, pfds, nfds, &revents);

        if (revents & POLLERR) {
            // 触发 XRUN 等, 走恢复路径
            if (xrun_recovery(pcm, -EPIPE) < 0) break;
            continue;
        }
        if (!(revents & POLLIN))
            continue;   // 还没数据

        /* 数据就绪, 读一个周期 */
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm, buffer, PERIOD_FRAMES);
        if (frames < 0) {
            if (xrun_recovery(pcm, frames) < 0) {
                fprintf(stderr, "readi recover failed: %s\n", snd_strerror(frames));
                break;
            }
            continue;
        }

        // TODO: 在这里处理 buffer, 大小为 frames * frame_bytes
        printf("got %ld frames (%ld bytes)\n", frames, frames * frame_bytes);
    }

    free(pfds);
out:
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    return 0;
}
