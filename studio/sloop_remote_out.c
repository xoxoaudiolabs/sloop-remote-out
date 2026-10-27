/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on the FM-1 Companion Studio (a Raspberry Pi 3 B+ with a 3.5" 480x320 screen): SLOOP's own sound engine,
 * sequencer and screens, built from its sources through the harness its tests use (tests/ui_pages_test.c),
 * running natively on the Pi. Played from a real FM-1 whose SLOOP sends its panel over MIDI (the sloop-remote
 * fork: GLO > MIDI 3 > REMOTE = OUT n), which the Companion passes on through a FIFO. SLOOP's own saving and its
 * editor protocol run as on an FM-1 (sloop_harness.c).
 *
 *   sloop-remote-out --fb /dev/fb0 --midi /tmp/fm1-sloop-midi [--ch 16] [--state FILE] [--clock-out FIFO]...
 *                    [--sysex-out FIFO]
 *
 *   screen  SLOOP's 240x240 picture at 4/3 (320x320) in the middle of the framebuffer, x 80..399; the margins
 *           are the Companion's. SIGUSR1 hides it (the Companion's "< HOME": SLOOP keeps running and playing,
 *           drawing nothing), SIGUSR2 shows it again, whole
 *   sound   44.1 kHz stereo into PipeWire (pw-cat), the Studio's speaker or jack
 *   panel   on MIDI channel --ch: CC 102 + label (127 down, 0 up) the 14 buttons in SLOOP's B_* order, CC 116 + role
 *           (64 + steps) the 7 knobs in EN_* order, notes 36..62 the 27 keys. Anything else is left alone
 *   state   everything SLOOP keeps in an FM-1's flash, kept as it keeps it, in flash.bin (a 1 MiB image of the
 *           FM-1's SPI NOR, the same offsets and objects) in --state's folder (default
 *           ~/.local/share/sloop-remote-out/): its settings, the working project, the four project slots, the
 *           song's sections, user presets, the FM6 patch bank, the four sample slots. Changed sectors are written
 *           to the file a moment after SLOOP writes them, and everything at the stop. The first time (no flash.bin
 *           yet), the working project an older version kept in --state (project.bin) and its SYNC / CLK OUT
 *           (settings.bin) are taken over; on the Studio CLK OUT starts ON (SLOOP leads the other apps' clock)
 *   editor  SysEx in the FIFO goes to SLOOP's editor protocol (editor.c: the web editor's, and the FM-1 Companion
 *           library's backups, restores, projects and samples), its replies to --sysex-out (a FIFO; with no reader
 *           there, a reply is dropped)
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
#include "sloop_harness.c"

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

/* ---- the flash: flash.bin, the FM-1's SPI NOR (sloop_harness.c's image) ---- */

static const char *state_path;
static char flash_path[512];

static int flash_open(void)             /* -> 1: a new image (nothing kept yet), 0: the file's, -1: failed */
{
    const char *slash = strrchr(state_path, '/');
    struct stat st;
    int fresh;
    snprintf(flash_path, sizeof flash_path, "%.*s/flash.bin", slash ? (int)(slash - state_path) : 1,
             slash ? state_path : ".");
    flash_img = malloc(FLASH_SIZE);
    flash_fd = open(flash_path, O_RDWR | O_CREAT, 0644);
    if (!flash_img || flash_fd < 0 || fstat(flash_fd, &st) < 0)
        return -1;
    fresh = st.st_size != (off_t)FLASH_SIZE || pread(flash_fd, flash_img, FLASH_SIZE, 0) != (ssize_t)FLASH_SIZE;
    if (fresh) {                        /* an erased part: all ones */
        memset(flash_img, 0xFF, FLASH_SIZE);
        if (pwrite(flash_fd, flash_img, FLASH_SIZE, 0) != (ssize_t)FLASH_SIZE || fsync(flash_fd) < 0)
            return -1;
    }
    return fresh;
}

static void flash_sync(void)            /* the sectors SLOOP changed, into the file (storage.c's A/B copies keep the
                                         * last good one should a write be cut short, as on the FM-1) */
{
    uint32_t w, any = 0;
    for (w = 0; w < sizeof flash_dirty / sizeof flash_dirty[0]; w++) {
        while (flash_dirty[w]) {
            uint32_t bit = (uint32_t)__builtin_ctz(flash_dirty[w]), off = (w * 32u + bit) * 0x1000u;
            flash_dirty[w] &= flash_dirty[w] - 1u;
            if (flash_fd >= 0 && pwrite(flash_fd, flash_img + off, 0x1000u, (off_t)off) == 0x1000)
                any = 1;
        }
    }
    if (any)
        fdatasync(flash_fd);
}

static void take_over_old_files(void)   /* (once, into a new flash.bin) what an older version kept beside it */
{
    project_t p;
    uint32_t v[3] = {0, 0, 1}, k;
    char path[600];
    const char *slash = strrchr(state_path, '/');
    FILE *f = fopen(state_path, "rb");
    if (f) {                            /* the working project (project.bin) */
        if (fread(&p, sizeof p, 1, f) == 1 && proj_ok(&p)) {
            proj_apply(&p, 1);
            song.sel = (uint8_t)(p.sel < NTRK ? p.sel : 0u);
            for (k = 0; k < NPART; k++)
                trk[k].engine = trk[k].eng_req;
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%.*s/settings.bin", slash ? (int)(slash - state_path) : 1, slash ? state_path : ".");
    f = fopen(path, "rb");
    if (f) {                            /* SYNC and CLK OUT (settings.bin: "SETS", sync, clk out) */
        uint32_t r[3];
        if (fread(r, sizeof r, 1, f) == 1 && r[0] == 0x53455453u && r[1] <= 2u && r[2] <= 1u)
            v[1] = r[1], v[2] = r[2];
        fclose(f);
    }
    lights_sync = (uint8_t)v[1];        /* the Studio's defaults otherwise: its own tempo, its clock sent */
    midi_set[9] = (uint8_t)v[2];
    song.g[G_SYNC] = (int16_t)lights_sync;
    song.g[G_CLKOUT] = (int16_t)midi_set[9];
    settings_save();
}

/* ---- the editor's replies: usb.c's SysEx ring, as bytes, to --sysex-out ---- */

static const char *sx_path;
static int sx_fd = -1;
static uint64_t sx_tried;

static uint64_t ms_now(void);

static void sloop_studio_sysex_out(void)
{
    uint8_t buf[3 * SXQ];
    uint32_t n = 0;
    while (so_r != so_w) {
        uint32_t pkt = sx_out_q[so_r % SXQ], cin = pkt & 15u, k = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        uint32_t j;
        so_r++;
        for (j = 0; j < k; j++)
            buf[n++] = (uint8_t)(pkt >> (8u + 8u * j));
    }
    if (!n || !sx_path)
        return;
    if (sx_fd < 0 && ms_now() - sx_tried >= 200u) {
        sx_tried = ms_now();
        sx_fd = open(sx_path, O_WRONLY | O_NONBLOCK);  /* (ENXIO: nobody listening: the reply is dropped) */
    }
    if (sx_fd >= 0 && write(sx_fd, buf, n) < 0 && errno != EAGAIN) {
        close(sx_fd);
        sx_fd = -1;
    }
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
static uint8_t in_sysex;

static void midi_bytes(const uint8_t *p, ssize_t n)
{
    ssize_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b >= 0xF8u) {
            clock_in(b);                    /* real-time: the clock, not the panel's */
            continue;
        }
        if (b == 0xF0u || in_sysex) {       /* SysEx: SLOOP's editor protocol, through usb.c as from USB */
            in_sysex = b != 0xF7u && (b == 0xF0u || !(b & 0x80u));
            sysex_byte(b);
            if (in_sysex || b == 0xF7u)
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
        sloop_studio_sysex_out();
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

/* power-on, as felucca_init (main.c) starts SLOOP: the default sounds, the drum track, empty patterns, then the
 * working project as it was left, and the FM-1's settings */
static void studio_init(void)
{
    uint32_t i;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    fm6_init();
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        track_defaults(t);
        if (i < NPART) {
            set_engine_of(t, TRK_DEF[i][0]);
            apply_preset_to(t, TRK_DEF[i][1]);
            t->engine = t->eng_req;
        }
        track_defaults_steps(t);
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.sel = 0;
    song.master_q12 = 2048;
    autosave_resume();
    song.g[G_SYNC] = (int16_t)lights_sync;
    song.g[G_MIDI] = (int16_t)lights_mout;
    song.g[G_ROUTE] = (int16_t)lights_min;
    song.g[G_CH1] = midi_set[0], song.g[G_CH2] = midi_set[1], song.g[G_CH3] = midi_set[2];
    song.g[G_DRCH] = midi_set[3], song.g[G_CHOTH] = midi_set[4], song.g[G_BEND] = midi_set[5];
    song.g[G_FXCH] = midi_set[6];
    song.g[G_DRMAP] = midi_set[7];
    song.g[G_CLKOUT] = midi_set[9];
    song.g[G_RMCH] = midi_set[8];
    song.g[G_FXFADE] = fxfade;
    layers_init();
}

int main(int argc, char **argv)
{
    const char *fb_path = "/dev/fb0", *midi_path = "/tmp/fm1-sloop-midi";
    static char state_default[512];
    uint64_t next_sync;
    int i, fresh;
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fb"))
            fb_path = argv[i + 1];
        else if (!strcmp(argv[i], "--midi"))
            midi_path = argv[i + 1];
        else if (!strcmp(argv[i], "--ch") && atoi(argv[i + 1]) >= 1 && atoi(argv[i + 1]) <= 16)
            remote_ch = (uint32_t)atoi(argv[i + 1]) - 1u;
        else if (!strcmp(argv[i], "--state"))
            state_path = argv[i + 1];
        else if (!strcmp(argv[i], "--sysex-out"))
            sx_path = argv[i + 1];
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

    fresh = flash_open();
    if (fresh < 0) {
        fprintf(stderr, "sloop-remote-out: %s: %s\n", flash_path, strerror(errno));
        return 1;
    }
    /* as SLOOP starts on the FM-1 (main.c fm1_main): the flash's settings, then the project */
    panel = PANEL_DEFAULT;
    persist_boot();
    settings_init();
    studio_init();
    if (fresh)
        take_over_old_files();
    usb.config = 1;                         /* "connected": SLOOP's USB MIDI and SysEx out queue (drained here) */
    sloop_splash();
    blit();
    go_home();
    ui.force = 1;

    open_sound();
    t0 = ms_now();
    next_sync = t0 + 1000u;
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
        ed_service();                       /* the editor's SysEx (main.c's loop) */
        sloop_studio_sysex_out();
        autosave_tick();                    /* the working project into the flash, when quiet */
        sections_flush();                   /* live sections / the recorded song, when quiet */
        if (ms_now() >= next_sync) {
            flash_sync();
            next_sync = ms_now() + 1000u;
        }
        if (!pcm)                           /* no PipeWire: keep time without it */
            usleep(16000);
    }
    persist_flush_now();                    /* what's only in RAM so far (as before an update) */
    flash_sync();
    if (pcm)
        pclose(pcm);
    return 0;
}
