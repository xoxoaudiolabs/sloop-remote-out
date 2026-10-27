/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP for the Studio: the harness tests/ui_pages_test.c builds (the real UI on a framebuffer, the audio between
 * frames), with SLOOP's own saving in place of the test's doubles: storage.c on a flash image kept in a file
 * (flash.bin, the FM-1's 1 MiB SPI NOR: the same offsets, the same objects), so its settings, the four project
 * slots, the working project, the song's sections, user presets, the FM6 patch bank and the four sample slots are
 * kept as the FM-1 keeps them; and its editor protocol (editor.c, the SysEx the web editor and the FM-1 Companion's
 * library speak) through the USB SysEx plumbing (usb.c), so a backup, a restore, a project or a sample goes in and
 * out exactly as on an FM-1.
 *
 * Lines 21-90 of tests/ui_pages_test.c (its harness: the framebuffer, the panel, the LEDs, the UI's sources) are
 * repeated here, unchanged but for the doubles it gives saving, which are left out. */
#define FELUCCA_ARRANGER 1
#define FELUCCA_FLASH 1                 /* storage.c, project.c and upreset.c with their flash paths */
#define FELUCCA_OTA 1                   /* usb.c's SysEx frames (sx_frame, ota_wire_send): the editor's */
#define FELUCCA_ID "FM-1_900"
#define FLASH_SIZE 0x100000u
static unsigned char *flash_img;        /* the FM-1's SPI NOR (sloop_studio_flash_open) */
#define SMP_USER_XIP(k) ((const uint8_t *)flash_img + SMP_USER_OFF((uint32_t)(k)))   /* the slots, as played from XIP */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>
static uint16_t screen[240*240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ uint32_t i,j; assert(x+w<=240 && y+h<=240); for(j=0;j<h;j++) for(i=0;i<w;i++) screen[(y+j)*240+x+i]=p[j*w+i]; }
#include "../firmware/src/gfx.c"
static void lcd_fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint16_t c)
{ uint32_t i,j; assert(x+w<=240&&y+h<=240); for(j=0;j<h;j++)for(i=0;i<w;i++)screen[(y+j)*240+x+i]=swap16(c); }
static int32_t encs[7];
static uint32_t fm1_ticks(void) { return fm1_ms * 1000u * 24u; }
#define FM1_TICKS_PER_US 24u
static int32_t enc_late[7];
static int32_t fm1_enc_take(uint32_t e) { int32_t s = encs[e]; encs[e] = enc_late[e]; enc_late[e] = 0; return s; }
static uint8_t fm1_led[16], fm1_led_dim[16], fm1_led_bg[16];
static volatile uint16_t fm1_led_bg_ns;
#define FM1_NCOL 16u
static const int8_t FM1_KEYMAP[5][16];
static void fm1_led_key(uint32_t id, int on) { (void)id; (void)on; }
static uint32_t edges_btn, notes_seen;
static uint32_t fm1_input_edges(int x) { uint32_t e = edges_btn; (void)x; edges_btn = 0; return e; }
static uint32_t fm1_input_note_edges(void) { uint32_t e = fm1_in.notes & ~notes_seen; notes_seen = fm1_in.notes; return e; }
static uint32_t wdt_tick_ms;
static void fm1_wdt_feed(void) { fm1_ms += wdt_tick_ms; }
static int32_t fm1_adc_read(int c) { (void)c; return -1; }
static struct { uint32_t magic, stage, page, home, ui_frames; } felucca_dbg;
#define FELUCCA_ICONS 1
#define SCOPE_N 512u
static int16_t scope_buf[SCOPE_N], scope_bufr[SCOPE_N];
static uint32_t scope_w;
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
#include "../firmware/src/ui_song.c"
#include "../firmware/src/ui_studio.c"
#include "../firmware/src/icons.c"
#include "../firmware/src/ui_draw.c"
#include "../firmware/src/ui_vis.c"
#include "../firmware/src/ui_layers.c"
#include "../firmware/src/ui_menu.c"
#include "../firmware/src/ui_input.c"
#include "../firmware/src/splash.c"

/* ---- the flash: the FM-1's 1 MiB SPI NOR, in RAM, kept in a file (a write: the sectors it changed) ---- */

static uint8_t flash_ok;
static int flash_fd = -1;
static uint32_t flash_dirty[FLASH_SIZE / 0x1000u / 32u];   /* sectors changed since the last write to the file */
static uint32_t fl_jedec_ram(void) { return 0x856014u; }    /* the part SLOOP looks for (persist_boot) */
#define FL_FAR(f) f
static void fl_plain_window_init(void) {}
static void fl_inval(uint32_t off, uint32_t n) { (void)off, (void)n; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static void flash_mark(uint32_t off, uint32_t n)
{
    uint32_t s;
    for (s = off / 0x1000u; s <= (off + n - 1u) / 0x1000u && s < FLASH_SIZE / 0x1000u; s++)
        flash_dirty[s / 32u] |= 1u << (s % 32u);
}
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off >= FLASH_SIZE || n > FLASH_SIZE - off)
        return -8;
    memcpy(dst, flash_img + off, n);
    return 0;
}
static int fl_erase4k_quiet(uint32_t off, uint32_t *took)
{
    if (took)
        *took = 0;
    off &= ~0xFFFu;
    if (off >= FLASH_SIZE)
        return -8;
    memset(flash_img + off, 0xFF, 0x1000u);
    flash_mark(off, 0x1000u);
    return 0;
}
static int st_erase(uint32_t off) { uint32_t took; return fl_erase4k_quiet(off, &took); }
static int fl_write(uint32_t off, const void *src, uint32_t n)      /* NOR: a program only clears bits */
{
    const uint8_t *s = src;
    uint32_t i;
    if (off >= FLASH_SIZE || n > FLASH_SIZE - off)
        return -8;
    for (i = 0; i < n; i++)
        flash_img[off + i] &= s[i];
    flash_mark(off, n);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n) { return fl_write(off, src, n); }
#define ST_XIP_PTR(off) ((const uint8_t *)flash_img + (off))
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"

/* ---- the editor's SysEx (usb.c's frames in, its SysEx ring out) ---- */

static uint32_t ota_now_ms(void) { return fm1_ms; }
static void sloop_studio_sysex_out(void);
static void ota_idle(void) { sloop_studio_sysex_out(); }   /* the reply ring full: drained to the FIFO, then on */
#include "../firmware/src/editor.c"
