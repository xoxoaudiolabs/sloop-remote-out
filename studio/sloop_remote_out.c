/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on the FM-1 Companion Studio (a Raspberry Pi 3 B+ with a 3.5" 480x320 screen): SLOOP's own sound engine,
 * sequencer and screens, built from its sources through the harness its tests use (tests/ui_pages_test.c),
 * running natively on the Pi. Played from a real FM-1 whose SLOOP sends its panel over MIDI (the sloop-remote
 * fork: GLO > MIDI 3 > REMOTE = OUT n), which the Companion passes on through a FIFO.
 *
 *   sloop-remote-out --fb /dev/fb0 --midi /tmp/fm1-sloop-midi [--ch 16] [--state FILE] [--clock-out FIFO]...
 *
 *   screen  SLOOP's 240x240 picture at 4/3 (320x320) in the middle of the framebuffer, x 80..399; the margins
 *           are the Companion's. SIGUSR1 hides it (the Companion's "< HOME": SLOOP keeps running and playing,
 *           drawing nothing), SIGUSR2 shows it again, whole
 *   sound   44.1 kHz stereo into PipeWire (pw-cat), the Studio's speaker or jack
 *   panel   on MIDI channel --ch: CC 102 + label (127 down, 0 up) the 14 buttons in SLOOP's B_* order, CC 116 + role
 *           (64 + steps) the 7 knobs in EN_* order, notes 36..62 the 27 keys. Anything else is left alone
 *   state   the working project (SLOOP's own format, project.c: the tracks, their sounds and steps, the song's
 *           settings) in --state (default ~/.local/share/sloop-remote-out/project.bin): loaded at the start, written
 *           a few seconds after something changed and at the stop. The project slots, user presets and sample
 *           slots are still the harness's RAM: gone at the stop. Two of SLOOP's device settings, kept next to it
 *           in settings.bin: GLO > SYSTEM > SYNC and GLO > MIDI 3 > CLK OUT (on the Studio, CLK OUT starts ON:
 *           SLOOP leads the other apps' clock unless set otherwise)
 *   clock   MIDI clock and START / CONTINUE / STOP in the FIFO too (the clock only while its sender plays), as SLOOP takes them on USB (GLO > SYSTEM >
 *           SYNC = USB follows them); and what SLOOP sends as clock master on USB (GLO > MIDI 3 > CLK OUT, while
 *           SYNC is INT) written to each --clock-out FIFO (another app's --midi): the two cabled together, as two
 *           FM-1s would be. A FIFO with no reader yet is tried again every half second; a pulse it can't take is
 *           dropped, never waited for
 *   stop    SIGTERM or SIGINT: the project written, the sound ended cleanly */
#define _GNU_SOURCE                     /* (F_SETPIPE_SZ) */
#include <errno.h>                      /* (before the harness: hostsim.c defines __attribute__ away) */
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define UI_PAGES_HARNESS_ONLY 1
#include "ui_pages_test.c"
#define PROJ_HOST 1                     /* project.c's formats, capture and apply: no flash */
#include "project.c"

#define FB_W 480u
#define VIEW_X 80u                      /* SLOOP's picture: 320x320 from here, the whole height */
#define VIEW 320u
#define FRAME_BLOCKS 22u                /* audio blocks (CTL frames each) between UI passes: ~16 ms, as the harness */
#define REMOTE_OUT_CC 116u
#define REMOTE_OUT_NOTE 36u

static volatile sig_atomic_t stop, hidden, show_whole;
static void on_signal(int s) { (void)s; stop = 1; }
static void on_hide(int s) { (void)s; hidden = 1; }
static void on_show(int s) { (void)s; hidden = 0; show_whole = 1; }

/* ---- the working project, kept in a file ---- */

static const char *state_path;
static project_t saved;                 /* what the file holds */

static void state_load(void)
{
    project_t p;
    FILE *f = fopen(state_path, "rb");
    uint32_t k;
    if (!f)
        return;
    if (fread(&p, sizeof p, 1, f) == 1 && proj_ok(&p)) {   /* (as project.c autosave_resume) */
        proj_apply(&p, 1);
        song.sel = (uint8_t)(p.sel < NTRK ? p.sel : 0u);
        for (k = 0; k < NPART; k++)
            trk[k].engine = trk[k].eng_req;
        saved = p;
    }
    fclose(f);
}

static void state_save(void)            /* when it changed: a new file put in place (never half a file) */
{
    project_t p;
    char tmp[512];
    FILE *f;
    proj_capture(&p);
    if (!memcmp(&p, &saved, sizeof p))
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(&p, sizeof p, 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0) {
        if (rename(tmp, state_path) == 0)
            saved = p;
        return;
    }
    fclose(f);
}

/* ---- SYNC and CLK OUT (on the FM-1, kept with its settings, project.c), kept in a file of their own ---- */

#define SET_MAGIC 0x53455453u           /* "SETS" */
static char set_path[512];
static int16_t set_saved[2] = {-1, -1};

static void clock_settings_load(void)
{
    uint32_t v[3] = {0, 0, 0};
    const char *slash = strrchr(state_path, '/');
    FILE *f;
    snprintf(set_path, sizeof set_path, "%.*s/settings.bin", slash ? (int)(slash - state_path) : 1,
             slash ? state_path : ".");
    song.g[G_SYNC] = 0;                 /* the Studio's defaults: its own tempo, the clock sent to the others */
    song.g[G_CLKOUT] = 1;
    f = fopen(set_path, "rb");
    if (f) {
        if (fread(v, sizeof v, 1, f) == 1 && v[0] == SET_MAGIC && v[1] <= 2u && v[2] <= 1u) {
            song.g[G_SYNC] = (int16_t)v[1];
            song.g[G_CLKOUT] = (int16_t)v[2];
        }
        fclose(f);
    }
    set_saved[0] = song.g[G_SYNC];
    set_saved[1] = song.g[G_CLKOUT];
}

static void clock_settings_save(void)         /* when one changed */
{
    uint32_t v[3] = {SET_MAGIC, (uint32_t)song.g[G_SYNC], (uint32_t)song.g[G_CLKOUT]};
    char tmp[600];
    FILE *f;
    if (song.g[G_SYNC] == set_saved[0] && song.g[G_CLKOUT] == set_saved[1])
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp", set_path);
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(v, sizeof v, 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0) {
        if (rename(tmp, set_path) == 0)
            set_saved[0] = song.g[G_SYNC], set_saved[1] = song.g[G_CLKOUT];
        return;
    }
    fclose(f);
}

static uint64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* ---- the panel from MIDI (the FM-1's REMOTE OUT) ---- */

static uint32_t remote_ch = 15u;        /* 0..15 (--ch 1..16) */

static void panel_message(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t kind = st & 0xF0u;
    if ((st & 0x0Fu) != remote_ch)
        return;
    if (kind == 0xB0u && d1 >= REMOTE_CC0 && d1 < REMOTE_CC0 + NB) {
        uint32_t b = 1u << panel.btn[d1 - REMOTE_CC0];
        if (d2 >= 64u) {
            if (!(fm1_in.buttons & b))
                edges_btn |= b;             /* (fm1_input_edges: a press is an edge, as the matrix scan makes it) */
            fm1_in.buttons |= b;
        } else {
            fm1_in.buttons &= ~b;
        }
    } else if (kind == 0xB0u && d1 >= REMOTE_OUT_CC && d1 < REMOTE_OUT_CC + NE) {
        encs[panel.enc[d1 - REMOTE_OUT_CC]] += ((int32_t)d2 - 64) * panel.dir[d1 - REMOTE_OUT_CC];
    } else if ((kind == 0x90u || kind == 0x80u) && d1 >= REMOTE_OUT_NOTE && d1 < REMOTE_OUT_NOTE + 27u) {
        uint32_t k = 1u << (d1 - REMOTE_OUT_NOTE);
        if (kind == 0x90u && d2)
            fm1_in.notes |= k;
        else
            fm1_in.notes &= ~k;
    }
}

/* ---- MIDI clock: in, as SLOOP's USB MIDI in; out, to the other apps' FIFOs ---- */

#define NCLK 4
static struct {
    const char *path;
    int fd;
    uint64_t tried;                     /* the last open tried (ms) */
} clk_out[NCLK];
static int nclk;

static void clock_in(uint8_t b)         /* a realtime byte, as usb.c queues one from the USB host (cable 0: USB) */
{
    static uint8_t running;             /* the other app plays: between its START (or CONTINUE) and its STOP */
    if (b == 0xFAu || b == 0xFBu)
        running = 1;
    else if (b == 0xFCu)
        running = 0;
    else if (b != 0xF8u || !running)
        return;                         /* (its clock while it's stopped: not followed, so an app playing on its
                                         * own keeps its time until the other's PLAY, never caught mid-bar) */
    if (mi_w - mi_r < MQ) {
        midi_in_q[mi_w % MQ] = 0xFu | (uint32_t)b << 8;
        mi_w++;
    }
}

static void clock_send(uint8_t b, uint64_t now)
{
    int k;
    for (k = 0; k < nclk; k++) {
        if (clk_out[k].fd < 0) {
            if (now - clk_out[k].tried < 500u)
                continue;
            clk_out[k].tried = now;
            clk_out[k].fd = open(clk_out[k].path, O_WRONLY | O_NONBLOCK);   /* (ENXIO: no reader yet) */
            if (clk_out[k].fd < 0)
                continue;
        }
        if (write(clk_out[k].fd, &b, 1) < 0 && errno != EAGAIN) {          /* (EPIPE: that app stopped) */
            close(clk_out[k].fd);
            clk_out[k].fd = -1;
            clk_out[k].tried = now;
        }
    }
}

static void clock_out_drain(uint64_t now)   /* SLOOP's USB MIDI out (usb.c's ring): its clock and transport, sent */
{
    while (mo_r != mo_w) {
        uint32_t pkt = midi_out_q[mo_r % MQ], st = (pkt >> 8) & 0xFFu;
        mo_r++;
        if ((pkt & 15u) == 0xFu && st >= 0xF8u)
            clock_send((uint8_t)st, now);
    }
}

static uint8_t mbuf[3];
static uint32_t mlen, mneed;

static void midi_bytes(const uint8_t *p, ssize_t n)
{
    ssize_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b >= 0xF8u) {
            clock_in(b);                    /* real-time: the clock, not the panel's */
            continue;
        }
        if (b & 0x80u) {
            mbuf[0] = b, mlen = 1;
            mneed = (b & 0xF0u) == 0xC0u || (b & 0xF0u) == 0xD0u ? 2u : b < 0xF0u ? 3u : 0u;
            continue;
        }
        if (!mneed || mlen >= mneed)
            continue;                       /* (SysEx, or data with no status: not the panel's) */
        mbuf[mlen++] = b;
        if (mlen == mneed) {
            panel_message(mbuf[0], mbuf[1], mneed > 2u ? mbuf[2] : 0u);
            mlen = 1;                       /* running status, should a sender use it */
        }
    }
}

/* ---- the screen: SLOOP's 240x240 big-endian RGB565, nearest-neighbour to 320x320, only the rows that changed ---- */

static uint16_t shown[240 * 240];
static int fb = -1, first = 1;

static void blit(void)
{
    static uint16_t row[VIEW];
    static uint32_t sx[VIEW];
    uint32_t y, x, src_was = 0xFFFFFFFFu, changed = 0;
    if (first)
        for (x = 0; x < VIEW; x++)
            sx[x] = x * 240u / VIEW;
    for (y = 0; y < VIEW; y++) {
        uint32_t sy = y * 240u / VIEW;
        if (sy != src_was) {
            src_was = sy;
            changed = first || memcmp(&screen[sy * 240u], &shown[sy * 240u], 240u * 2u);
            if (changed)
                for (x = 0; x < VIEW; x++)
                    row[x] = swap16(screen[sy * 240u + sx[x]]);
        }
        if (changed && pwrite(fb, row, sizeof row, (off_t)((y * FB_W + VIEW_X) * 2u)) < 0)
            return;
    }
    memcpy(shown, screen, sizeof shown);
    first = 0;
}

/* ---- the sound ---- */

static FILE *pcm;

static void open_sound(void)
{
    pcm = popen("pw-cat --playback --raw --rate 44100 --channels 2 --format s16 --latency 512 "
                "--media-role Music --properties "
                "'{\"node.name\":\"sloop-remote-out\",\"node.description\":\"SLOOP\"}' -", "w");
    if (pcm) {
        fcntl(fileno(pcm), F_SETPIPE_SZ, 4096); /* a small pipe: the write blocks after ~23 ms, so sound keeps time */
        setvbuf(pcm, NULL, _IONBF, 0);      /* each block written as it's made: the write paces every block, so
                                             * MIDI (the clock) is read and timed a block (0.7 ms) at a time */
    }
}

static int midi = -1;
static uint64_t t0;

static void render(void)                /* FRAME_BLOCKS of audio to PipeWire, MIDI read before each block */
{
    static int32_t o[CTL * 2];
    static int16_t out[CTL * 2];
    uint32_t q, i;
    for (q = 0; q < FRAME_BLOCKS; q++) {
        uint8_t buf[256];
        ssize_t n;
        uint64_t now = ms_now();
        while (midi >= 0 && (n = read(midi, buf, sizeof buf)) > 0)
            midi_bytes(buf, n);
        fm1_ms = (uint32_t)(now - t0);
        mix_block(o, CTL);
        clock_out_drain(now);
        for (i = 1; i < CTL; i += 2u) {     /* the scope, as audio.c fills it (the visualiser) */
            scope_bufr[scope_w & (SCOPE_N - 1u)] = vis_tap[2u * i + 1u];
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = vis_tap[2u * i];
        }
        for (i = 0; i < CTL * 2u; i++)
            out[i] = (int16_t)(o[i] > 32767 ? 32767 : o[i] < -32768 ? -32768 : o[i]);
        if (pcm && fwrite(out, sizeof out[0], CTL * 2u, pcm) != CTL * 2u) {
            pclose(pcm);
            pcm = NULL;
        }
    }
}

int main(int argc, char **argv)
{
    const char *fb_path = "/dev/fb0", *midi_path = "/tmp/fm1-sloop-midi";
    static char state_default[512];
    uint64_t next_save;
    int i;
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fb"))
            fb_path = argv[i + 1];
        else if (!strcmp(argv[i], "--midi"))
            midi_path = argv[i + 1];
        else if (!strcmp(argv[i], "--ch") && atoi(argv[i + 1]) >= 1 && atoi(argv[i + 1]) <= 16)
            remote_ch = (uint32_t)atoi(argv[i + 1]) - 1u;
        else if (!strcmp(argv[i], "--state"))
            state_path = argv[i + 1];
        else if (!strcmp(argv[i], "--clock-out") && nclk < NCLK) {
            clk_out[nclk].path = argv[i + 1];
            clk_out[nclk++].fd = -1;
        }
    }
    if (!state_path) {
        snprintf(state_default, sizeof state_default, "%s/.local/share/sloop-remote-out/project.bin",
                 getenv("HOME") ? getenv("HOME") : ".");
        state_path = state_default;
    }
    signal(SIGUSR1, on_hide);
    signal(SIGUSR2, on_show);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    fb = open(fb_path, O_WRONLY);
    if (fb < 0) {
        fprintf(stderr, "sloop-remote-out: %s: %s\n", fb_path, strerror(errno));
        return 1;
    }
    if (mkfifo(midi_path, 0600) < 0 && errno != EEXIST)
        fprintf(stderr, "sloop-remote-out: %s: %s\n", midi_path, strerror(errno));
    midi = open(midi_path, O_RDONLY | O_NONBLOCK);  /* (read-write would keep it open with no writer; this end
                                                     * just reads nothing until the Companion writes) */

    /* as the harness starts SLOOP (ui_pages_test.c main) */
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    host_tracks_init();
    for (i = 0; i < (int)NPART; i++) {
        set_engine_of(&trk[i], TRK_DEF[i][0]);
        apply_preset_to(&trk[i], TRK_DEF[i][1]);
        trk[i].engine = trk[i].eng_req;
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    usb.config = 1;                         /* "connected": SLOOP's USB MIDI out queues (clock_out_drain takes it) */
    state_load();                           /* as it was left */
    clock_settings_load();
    sloop_splash();
    blit();
    go_home();
    ui.force = 1;

    open_sound();
    t0 = ms_now();
    next_save = t0 + 5000u;
    while (!stop) {
        render();
        fm1_ms = (uint32_t)(ms_now() - t0);
        ui_input();
        ui_leds();
        if (show_whole) {                   /* shown again: everything redrawn, all of it written */
            show_whole = 0;
            ui.force = 1;
            first = 1;
        }
        if (!hidden) {                      /* (hidden: the Companion has the screen) */
            ui_draw();
            blit();
        }
        if (ms_now() >= next_save) {
            state_save();
            clock_settings_save();
            next_save = ms_now() + 5000u;
        }
        if (!pcm)                           /* no PipeWire: keep time without it */
            usleep(16000);
    }
    state_save();
    clock_settings_save();
    if (pcm)
        pclose(pcm);
    return 0;
}
