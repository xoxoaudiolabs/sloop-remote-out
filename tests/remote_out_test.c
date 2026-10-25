/* SPDX-License-Identifier: GPL-3.0-only */
/* (remote-out) REMOTE OUT on the host, through the ui_pages_test harness: with GLO > MIDI 3 > REMOTE = OUT 16, the
 * buttons, keys, knobs (every turn, whatever the page) and MASTER go out on MIDI channel 16, and SLOOP still gets
 * the same knob turns; REMOTE OFF sends nothing.
 *   cc -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o build/host/remote_out_test tests/remote_out_test.c -lm */
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"

static uint32_t sent[256], nsent;

static void drain(void)                     /* what went into the USB MIDI ring since the last look */
{
    while (mo_r != mo_w && nsent < 256u)
        sent[nsent++] = midi_out_q[mo_r++ % MQ];
    mo_r = mo_w;
}

static int has(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t i, want = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
    for (i = 0; i < nsent; i++)
        if (sent[i] == want)
            return 1;
    return 0;
}

static uint32_t count_on_channel(uint32_t ch)
{
    uint32_t i, n = 0;
    for (i = 0; i < nsent; i++)
        n += ((sent[i] >> 8) & 15u) == ch;
    return n;
}

int main(void)
{
    int16_t before;
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    host_tracks_init();
    usb.config = 1;                         /* a host is listening: the ring takes events */
    go_home();
    frames(2);
    drain();
    nsent = 0;

    frames(1);                              /* REMOTE OFF: a knob, a button, a key: nothing on channel 16 (the keys
                                             * still go out on their track's channel, as SLOOP's MIDI OUT does) */
    encs[panel.enc[EN_K2]] = 2;
    press(B_SEQ);
    release(B_SEQ);
    key(3);
    drain();
    check(count_on_channel(15u) == 0u, "REMOTE OFF: nothing sent on channel 16");

    song.g[G_RMCH] = 17 + 15;               /* OUT 16 */
    frames(1);
    drain();
    check(has(0xBFu, 123u, 127u), "OUT 16: MASTER sent first, where it is (all the way up: CC 123, 127)");
    nsent = 0;

    before = song.g[G_BPM];                 /* SELECT on TRACKS is the tempo: SLOOP gets the steps it sends */
    encs[panel.enc[EN_SELECT]] = 3;
    frames(1);
    drain();
    check(has(0xBFu, 116u, 67u), "OUT 16: SELECT +3 sent as CC 116, 67");
    check(song.g[G_BPM] != before, "OUT 16: SLOOP still takes the turn (the tempo changed)");
    nsent = 0;

    encs[panel.enc[EN_K4]] = -2;            /* KNOB 4: sent whatever the page reads */
    frames(1);
    drain();
    check(has(0xBFu, 122u, 62u), "OUT 16: KNOB 4 -2 sent as CC 122, 62");
    nsent = 0;

    press(B_PLAY);
    drain();
    check(has(0xBFu, 112u, 127u), "OUT 16: PLAY down as CC 112, 127");
    release(B_PLAY);
    drain();
    check(has(0xBFu, 112u, 0u), "OUT 16: PLAY up as CC 112, 0");
    nsent = 0;

    fm1_in.notes |= 1u << 7;                /* C4: key 7 */
    frame();
    drain();
    check(has(0x9Fu, 43u, 100u), "OUT 16: key 7 down as note 43, 100");
    fm1_in.notes &= ~(1u << 7);
    frame();
    drain();
    check(has(0x8Fu, 43u, 0u), "OUT 16: key 7 up as note 43 off");
    nsent = 0;

    song.master_q12 = (512u * 512u) >> 8;   /* MASTER half way (main.c: k10 * k10 >> 8): sent once, as 63 */
    frames(3);
    drain();
    check(has(0xBFu, 123u, 63u), "OUT 16: MASTER half way as CC 123, 63");
    check(count_on_channel(15u) == 1u, "OUT 16: MASTER sent once while it stays");
    nsent = 0;
    song.master_q12 = (520u * 520u) >> 8;   /* a wobble of one step (64): not sent */
    frames(2);
    drain();
    check(count_on_channel(15u) == 0u, "OUT 16: MASTER's own jitter (one step) not sent");
    nsent = 0;

    song.g[G_RMCH] = 0;                     /* REMOTE OFF again: SLOOP reads the knobs itself again */
    frames(1);
    before = song.g[G_BPM];
    encs[panel.enc[EN_SELECT]] = 1;
    frames(1);
    drain();
    check(count_on_channel(15u) == 0u && song.g[G_BPM] != before,
          "REMOTE OFF again: nothing on channel 16, SLOOP takes the knob itself");
    printf("remote_out_test: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
