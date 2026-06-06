// gcc audio_active_cb.c -o audio_active_cb -lasound
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>

static int get_audio_active(snd_ctl_t *ctl, const char *name, long *rate)
{
    snd_ctl_elem_id_t    *id;   snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_t *val;  snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_value_set_id(val, id);
    if (snd_ctl_elem_read(ctl, val) < 0) return -1;
    *rate = snd_ctl_elem_value_get_integer(val, 0);
    return 0;
}

/* 你的“回调”:等价于 tud_audio_set_itf_cb 里设置 audioActive 之后的处理 */
static void on_itf_changed(int audioActive, long rate)
{
    if (audioActive)
        printf("[AUDIO] host set alt!=0 -> Speaker active, rate=%ld\n", rate);
    else
        printf("[AUDIO] host set alt=0  -> Speaker inactive\n");
}

int main(int argc, char **argv)
{
    const char *card = (argc > 1) ? argv[1] : "hw:UAC2Gadget";
    const char *ctlname = "Capture Rate";   // playback 方向则用 "Playback Rate"

    snd_ctl_t *ctl;
    if (snd_ctl_open(&ctl, card, 0) < 0) { fprintf(stderr, "ctl_open fail\n"); return 1; }
    snd_ctl_subscribe_events(ctl, 1);

    long rate = 0;
    get_audio_active(ctl, ctlname, &rate);
    on_itf_changed(rate > 0, rate);          // 上电先读一次当前状态

    struct pollfd pfds[8];
    int nfds = snd_ctl_poll_descriptors(ctl, pfds, 8);
    for (;;) {
        if (poll(pfds, nfds, -1) < 0) break;
        unsigned short rev;
        snd_ctl_poll_descriptors_revents(ctl, pfds, nfds, &rev);
        if (!(rev & POLLIN)) continue;

        snd_ctl_event_t *ev; snd_ctl_event_alloca(&ev);
        if (snd_ctl_read(ctl, ev) < 0) continue;
        if (snd_ctl_event_get_type(ev) != SND_CTL_EVENT_ELEM) continue;

        const char *n = snd_ctl_event_elem_get_name(ev);
        if (n && strcmp(n, ctlname) == 0) {  // 等价于 itf==1 的判断
            get_audio_active(ctl, ctlname, &rate);
            on_itf_changed(rate > 0, rate);  // 等价于 config.audioActive = (alt!=0)
        }
    }
    snd_ctl_close(ctl);
    return 0;
}