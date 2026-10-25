/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on the FM-1 Companion Studio (a Raspberry Pi 3 B+ with a 3.5" 480x320 screen): SLOOP's own sound engine,
 * sequencer and screens, built from its sources through the harness its tests use (tests/ui_pages_test.c),
 * running natively on the Pi. Played from a real FM-1 whose SLOOP sends its panel over MIDI (the sloop-remote
 * fork: GLO > MIDI 3 > REMOTE = OUT n), which the Companion passes on through a FIFO.
 *
 *   sloop-remote-out --fb /dev/fb0 --midi /tmp/fm1-sloop-midi [--ch 16] [--state FILE]
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
 *           slots are still the harness's RAM: gone at the stop
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

static uint8_t mbuf[3];
static uint32_t mlen, mneed;

static void midi_bytes(const uint8_t *p, ssize_t n)
{
    ssize_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b >= 0xF8u)
            continue;                       /* real-time: not the panel's */
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
        setvbuf(pcm, NULL, _IOFBF, 4096);
    }
}

static void render(void)                /* FRAME_BLOCKS of audio to PipeWire (the write paces the loop) */
{
    static int32_t o[CTL * 2];
    static int16_t out[FRAME_BLOCKS * CTL * 2];
    uint32_t q, i, n = 0;
    for (q = 0; q < FRAME_BLOCKS; q++) {
        mix_block(o, CTL);
        for (i = 1; i < CTL; i += 2u) {     /* the scope, as audio.c fills it (the visualiser) */
            scope_bufr[scope_w & (SCOPE_N - 1u)] = vis_tap[2u * i + 1u];
            scope_buf[scope_w++ & (SCOPE_N - 1u)] = vis_tap[2u * i];
        }
        for (i = 0; i < CTL * 2u; i++)
            out[n++] = (int16_t)(o[i] > 32767 ? 32767 : o[i] < -32768 ? -32768 : o[i]);
    }
    if (pcm && fwrite(out, sizeof out[0], n, pcm) != n) {
        pclose(pcm);
        pcm = NULL;
    }
}

int main(int argc, char **argv)
{
    const char *fb_path = "/dev/fb0", *midi_path = "/tmp/fm1-sloop-midi";
    static char state_default[512];
    uint64_t t0, next_save;
    int midi, i;
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fb"))
            fb_path = argv[i + 1];
        else if (!strcmp(argv[i], "--midi"))
            midi_path = argv[i + 1];
        else if (!strcmp(argv[i], "--ch") && atoi(argv[i + 1]) >= 1 && atoi(argv[i + 1]) <= 16)
            remote_ch = (uint32_t)atoi(argv[i + 1]) - 1u;
        else if (!strcmp(argv[i], "--state"))
            state_path = argv[i + 1];
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
    state_load();                           /* as it was left */
    sloop_splash();
    blit();
    go_home();
    ui.force = 1;

    open_sound();
    t0 = ms_now();
    next_save = t0 + 5000u;
    while (!stop) {
        uint8_t buf[256];
        ssize_t n;
        render();
        while (midi >= 0 && (n = read(midi, buf, sizeof buf)) > 0)
            midi_bytes(buf, n);
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
            next_save = ms_now() + 5000u;
        }
        if (!pcm)                           /* no PipeWire: keep time without it */
            usleep(16000);
    }
    state_save();
    if (pcm)
        pclose(pcm);
    return 0;
}
