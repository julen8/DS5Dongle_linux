// gcc alsa_poll_capture_with_ctl.c -o demo -lasound
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <alsa/asoundlib.h>

#define PCM_DEVICE      "default"
#define SAMPLE_RATE     16000
#define CHANNELS        1
#define FORMAT          SND_PCM_FORMAT_S16_LE
#define PERIOD_FRAMES   1024
#define CTL_ELEM_NAME   "Capture Rate"     // 想监听的 control 名字

/* ---------- 处理 control 事件 ---------- */
static void handle_ctl_events(snd_ctl_t *ctl)
{
    snd_ctl_event_t *event;
    snd_ctl_event_alloca(&event);

    int err;
    /* 非阻塞模式下, 把队列里的事件全部读完 */
    while ((err = snd_ctl_read(ctl, event)) > 0) {
        if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM)
            continue;

        const char *name = snd_ctl_event_elem_get_name(event);
        unsigned int mask = snd_ctl_event_elem_get_mask(event);

        if (strcmp(name, CTL_ELEM_NAME) != 0)
            continue;

        /* 只关心 VALUE 变化; 也可能收到 INFO/ADD/REMOVE 等 */
        if (!(mask & SND_CTL_EVENT_MASK_VALUE))
            continue;

        /* 从事件里取出 elem id, 再 read 当前值 */
        snd_ctl_elem_id_t    *id;
        snd_ctl_elem_value_t *val;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_value_alloca(&val);
        snd_ctl_event_elem_get_id(event, id);
        snd_ctl_elem_value_set_id(val, id);

        if (snd_ctl_elem_read(ctl, val) == 0) {
            unsigned int rate = snd_ctl_elem_value_get_integer(val, 0);
            printf(">>> [CTL] %s changed -> %u Hz\n", name, rate);
            // TODO: 这里可以触发 PCM 重新配置 (drop -> set_params -> start)
        }
    }
    if (err < 0 && err != -EAGAIN)
        fprintf(stderr, "snd_ctl_read: %s\n", snd_strerror(err));
}

/* ---------- 根据 PCM 句柄推导出 "hw:N" 字符串 ---------- */
static int pcm_to_ctl_name(snd_pcm_t *pcm, char *out, size_t outsz)
{
    snd_pcm_info_t *info;
    snd_pcm_info_alloca(&info);
    int err = snd_pcm_info(pcm, info);
    if (err < 0) return err;
    int card = snd_pcm_info_get_card(info);
    if (card < 0) return -ENODEV;
    snprintf(out, outsz, "hw:%d", card);
    return 0;
}

int main(void)
{
    snd_pcm_t *pcm = NULL;
    snd_ctl_t *ctl = NULL;
    int err;

    /* === 1. 打开并配置 PCM (同上一份示例) === */
    if ((err = snd_pcm_open(&pcm, PCM_DEVICE,
                            SND_PCM_STREAM_CAPTURE,
                            SND_PCM_NONBLOCK)) < 0) {
        fprintf(stderr, "pcm_open: %s\n", snd_strerror(err)); return 1;
    }
    if ((err = snd_pcm_set_params(pcm, FORMAT,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  CHANNELS, SAMPLE_RATE,
                                  1, 100 * 1000)) < 0) {
        fprintf(stderr, "set_params: %s\n", snd_strerror(err)); return 1;
    }

    /* === 2. 打开同一张卡的 control 并订阅事件 === */
    char ctl_name[16];
    if ((err = pcm_to_ctl_name(pcm, ctl_name, sizeof(ctl_name))) < 0) {
        fprintf(stderr, "pcm_to_ctl_name: %s\n", snd_strerror(err)); return 1;
    }
    if ((err = snd_ctl_open(&ctl, ctl_name, SND_CTL_NONBLOCK)) < 0) {
        fprintf(stderr, "ctl_open(%s): %s\n", ctl_name, snd_strerror(err)); return 1;
    }
    if ((err = snd_ctl_subscribe_events(ctl, 1)) < 0) {
        fprintf(stderr, "subscribe_events: %s\n", snd_strerror(err)); return 1;
    }

    /* === 3. 计算两边各需要多少 fd, 拼一个总数组 === */
    int n_pcm = snd_pcm_poll_descriptors_count(pcm);
    int n_ctl = snd_ctl_poll_descriptors_count(ctl);
    int nfds  = n_pcm + n_ctl;
    struct pollfd *pfds = calloc(nfds, sizeof(*pfds));

    snd_pcm_poll_descriptors(pcm, &pfds[0],     n_pcm);
    snd_ctl_poll_descriptors(ctl, &pfds[n_pcm], n_ctl);

    snd_pcm_start(pcm);

    /* === 4. 主循环 === */
    short buf[PERIOD_FRAMES * CHANNELS];
    for (;;) {
        int ret = poll(pfds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }
        if (ret == 0) continue;

        /* ---- 4a. PCM 部分 ---- */
        unsigned short pcm_revents = 0;
        snd_pcm_poll_descriptors_revents(pcm, &pfds[0], n_pcm, &pcm_revents);

        if (pcm_revents & POLLERR) {
            if (snd_pcm_state(pcm) == SND_PCM_STATE_XRUN)
                snd_pcm_prepare(pcm), snd_pcm_start(pcm);
        } else if (pcm_revents & POLLIN) {
            snd_pcm_sframes_t fr = snd_pcm_readi(pcm, buf, PERIOD_FRAMES);
            if (fr == -EPIPE) {
                snd_pcm_prepare(pcm); snd_pcm_start(pcm);
            } else if (fr > 0) {
                // TODO: 处理 buf, fr 帧
            }
        }

        /* ---- 4b. CTL 部分 ---- */
        unsigned short ctl_revents = 0;
        snd_ctl_poll_descriptors_revents(ctl, &pfds[n_pcm], n_ctl, &ctl_revents);
        if (ctl_revents & (POLLIN | POLLERR))
            handle_ctl_events(ctl);
    }

    free(pfds);
    snd_pcm_close(pcm);
    snd_ctl_close(ctl);
    return 0;
}
