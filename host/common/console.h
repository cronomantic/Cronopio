#ifndef CRONOPIO_CONSOLE_H
#define CRONOPIO_CONSOLE_H

#include <stdint.h>

#define CRONOPIO_SCREEN_W      320
#define CRONOPIO_SCREEN_H      240
#define CRONOPIO_PALETTE_SIZE  256
#define CRONOPIO_FPS            60
#define CRONOPIO_AUDIO_HZ    22050
#define CRONOPIO_AUDIO_CHANS    16   /* voices, shared between music and SFX */
#define CRONOPIO_SAMPLE_SLOTS   16   /* registered PCM sample banks */
#define CRONOPIO_PAD_COUNT       2

/* Synth waveforms (cron_snd_tone wave arg). */
#define CRON_WAVE_SINE_  0
#define CRON_WAVE_SQR_   1
#define CRON_WAVE_TRI_   2
#define CRON_WAVE_NOISE_ 3
#define CRON_WAVE_PULSE_ 4

/* Envelope stages. */
enum { CRON_ENV_OFF = 0, CRON_ENV_ATTACK, CRON_ENV_DECAY, CRON_ENV_SUSTAIN, CRON_ENV_RELEASE };
#define CRONOPIO_SAVE_BYTES   1024
#define CRONOPIO_IMAGE_SLOTS     8
#define CRONOPIO_TILEMAP_SLOTS   8
#define CRONOPIO_TILE_SIZE       8

/* 3D triangle submission. A vertex is 7 little-endian i32 words in cart
 * memory: x, y (screen px), z (depth), u, v (texcoords Q16.16), w (1/clip-w
 * style depth for perspective), c (gouraud light/index). cron_polys draws
 * count/3 triangles. mode is a bitmask. */
#define CRONOPIO_VERT_WORDS      7
#define CRONOPIO_VERT_BYTES      (CRONOPIO_VERT_WORDS * 4)
#define CRONOPIO_POLY_GOURAUD    (1u << 0)   /* interpolate vertex c via cmap */
#define CRONOPIO_POLY_TEX        (1u << 1)   /* affine texture from image bank */
#define CRONOPIO_POLY_PERSP      (1u << 2)   /* perspective-correct (needs w)  */
#define CRONOPIO_POLY_ZTEST      (1u << 3)   /* depth test/write the z-buffer  */

#define CRONOPIO_FB_BYTES      (CRONOPIO_SCREEN_W * CRONOPIO_SCREEN_H)  /* 76 800 */
#define CRONOPIO_PAL_BYTES     (CRONOPIO_PALETTE_SIZE * 4)              /* 128    */

/* Canonical region names for host-shared regions. The cart declares them via
 * cvm-cc flags (--region=fb:76800:rw --region=pal:128:rw); the host resolves
 * their heap offsets at load time with cvm_image_get_region. */
#define CRONOPIO_FB_REGION   "fb"
#define CRONOPIO_PAL_REGION  "pal"

/* A voice: either a synth waveform or a PCM sample, shaped by an ADSR
 * envelope. mode 0 = synth, 1 = PCM. Inactive when env stage is OFF and the
 * voice isn't gated. */
typedef struct {
    int      active;
    int      mode;        /* 0 synth, 1 pcm */
    int      vol;         /* 0..255 */
    int      pan;         /* -128..127 */

    /* synth */
    int      wave;
    uint32_t freq_mhz;
    uint32_t phase;

    /* pcm: a direct descriptor into cart memory (heap offsets, in samples),
     * so the MOD player can point a voice at an arbitrary sample with a
     * sub-region sustain loop. pcm_pos/step are Q16.16. pcm_looplen==0 means
     * one-shot (stop at pcm_len); >0 loops [pcm_loopstart, +pcm_looplen). */
    uint32_t pcm_off;
    uint32_t pcm_len;
    uint32_t pcm_loopstart;
    uint32_t pcm_looplen;
    uint32_t pcm_pos;
    uint32_t pcm_step;
    int      pcm_unsigned;   /* 1 = sample bytes are unsigned 8-bit (DMX) */

    /* ADSR envelope (level is Q8.8: 0..255<<8). Times in samples. */
    int      env_stage;
    int      env_level;   /* current 0..(255<<8) */
    int      env_attack;  /* samples for 0 -> peak */
    int      env_decay;   /* samples for peak -> sustain */
    int      env_sustain; /* 0..255 sustain level */
    int      env_release; /* samples for level -> 0 */
    int      has_env;     /* 0 = gate (no envelope): full level until stop */
} cron_voice_t;

/* PCM sample bank: 8-bit signed mono in cart memory at heap `offset`,
 * `len` samples, native `rate` Hz. */
typedef struct {
    uint32_t offset;
    uint32_t len;
    uint32_t rate;
    int      u8;          /* 1 = unsigned 8-bit (DMX), 0 = signed 8-bit */
    int      used;
} cron_sample_bank_t;

/* Streaming PCM: a host ring buffer of 16-bit signed *stereo* frames at the
 * output rate. The cart renders music (e.g. a DOOM MUS via its own OPL
 * emulator) and pushes frames; the mixer drains and adds them. */
#define CRONOPIO_STREAM_FRAMES  8192

#define CRONOPIO_MOD_MAXCHAN  8
#define CRONOPIO_MOD_SAMPLES  31

/* Host-side ProTracker MOD player. Parses a .mod blob living in cart memory
 * (RAM or ROM) and drives voices 0..n_channels-1; SFX use the rest. */
typedef struct {
    int      playing;
    int      loop_song;

    /* parsed from the .mod (heap offsets are absolute into the cart heap) */
    int      n_channels;
    int      song_len;
    uint8_t  order[128];
    uint32_t pattern_off;                       /* pattern data */
    uint32_t smp_off[CRONOPIO_MOD_SAMPLES];     /* sample PCM */
    uint32_t smp_len[CRONOPIO_MOD_SAMPLES];
    uint32_t smp_loopstart[CRONOPIO_MOD_SAMPLES];
    uint32_t smp_looplen[CRONOPIO_MOD_SAMPLES];
    uint8_t  smp_vol[CRONOPIO_MOD_SAMPLES];     /* 0..64 */

    /* playback position */
    int      pos;            /* order index */
    int      row;            /* 0..63 */
    int      speed;          /* ticks per row (default 6) */
    int      bpm;            /* default 125 */
    int      tick;           /* current tick within the row */
    int      samples_per_tick;
    int      sample_counter; /* output samples until the next tick */
    int      pat_break;      /* pending row break (-1 none) */
    int      pos_jump;       /* pending order jump (-1 none) */

    /* per-channel running state */
    int      ch_sample[CRONOPIO_MOD_MAXCHAN];
    int      ch_period[CRONOPIO_MOD_MAXCHAN];
    int      ch_vol[CRONOPIO_MOD_MAXCHAN];      /* 0..64 */
    uint8_t  ch_fx[CRONOPIO_MOD_MAXCHAN];
    uint8_t  ch_fxp[CRONOPIO_MOD_MAXCHAN];
} cron_mod_t;

/* Global drawing state — clip rect, camera offset, draw-time palette remap.
 * Every primitive routes pixel writes through these (Pyxel-style). */
typedef struct {
    int     clip_x0, clip_y0, clip_x1, clip_y1;  /* screen-space [x0,x1) [y0,y1) */
    int     cam_x, cam_y;                          /* subtracted from world coords */
    uint8_t pal_map[256];                          /* draw color -> written color  */
} cron_draw_t;

/* An image bank is a thin handle over an 8bpp bitmap living in cart memory
 * (RAM or ROM): offset into the heap + dimensions. used==0 means unbound. */
typedef struct {
    uint32_t offset;
    int      w, h;
    int      used;
} cron_image_bank_t;

/* A tilemap bank is a grid of u16 cells (linear tile index, 0xFFFF = empty)
 * in cart memory, drawn from 8x8 tiles of image bank `img`. */
typedef struct {
    uint32_t offset;
    int      w, h;        /* in cells */
    int      img;         /* image-bank slot the tiles come from */
    int      used;
} cron_tilemap_bank_t;

typedef struct {
    /* Heap-relative offsets of the fb/pal regions inside the CronoVM image.
     * Resolved once at cart load time. Zero/invalid means "not declared by
     * this cart" — the host still runs the cart but drawing syscalls
     * become no-ops and the screen stays black. */
    uint32_t fb_offset;
    uint32_t pal_offset;
    int      regions_ok;

    /* graphics */
    cron_draw_t         draw;
    cron_image_bank_t   images[CRONOPIO_IMAGE_SLOTS];
    cron_tilemap_bank_t tilemaps[CRONOPIO_TILEMAP_SLOTS];

    /* Active colormap for the textured-rasteriser accelerators (tcol/tspan):
     * a 256-byte remap in cart memory (e.g. a DOOM light colormap). When
     * cmap_set is 0 the accelerators write source indices unremapped. */
    uint32_t cmap_offset;
    int      cmap_set;

    /* Optional depth buffer: 320x240 int32 in cart memory, written/tested by
     * CRONOPIO_POLY_ZTEST triangles. Nearer = smaller z. */
    uint32_t zbuf_offset;
    int      zbuf_set;

    /* audio */
    cron_voice_t       voices[CRONOPIO_AUDIO_CHANS];
    cron_sample_bank_t samples[CRONOPIO_SAMPLE_SLOTS];
    int                master_vol_q8;            /* 0..256 */
    cron_mod_t         mod;                       /* host-side MOD player */
    void*              synth;                     /* MIDI+SoundFont synth (midisynth.c, opaque cron_synth*) */

    /* Streaming PCM ring (16-bit stereo). head written by the cart thread
     * (cron_stream), tail by the audio thread (mixer); single-producer /
     * single-consumer, races benign. */
    int16_t            stream[CRONOPIO_STREAM_FRAMES * 2];
    int                stream_head;
    int                stream_tail;
    /* Cart heap base, captured at load so the audio thread can read PCM
     * sample bytes (the audio callback only gets the console pointer). */
    const uint8_t     *heap;

    /* input snapshot */
    uint32_t pad_cur[CRONOPIO_PAD_COUNT];
    uint32_t pad_prev[CRONOPIO_PAD_COUNT];
    int32_t  mouse_x, mouse_y;
    uint32_t mouse_buttons;

    /* keyboard scancode bitmap (256 bits) — written by the platform shell. */
    uint8_t  keys[32];

    /* timing */
    uint64_t boot_ms;
    uint32_t frame_count;
    uint32_t prng_state;

    /* Cart-registered per-frame callback. The cart calls cron_set_frame(fn)
     * during its boot run; the syscall handler stores fn's FUNCS index here.
     * The platform shell invokes it via cvm_call once per 1/60 s tick. */
    int32_t  frame_fn_index;

    /* cart exit signal — set by cron_exit syscall or the platform shell. */
    int      cart_exited;
    int32_t  exit_status;

    /* persistence */
    uint8_t  save[CRONOPIO_SAVE_BYTES];
    int      save_dirty;
} cronopio_console_t;

void cronopio_console_init(cronopio_console_t* c);
void cronopio_console_begin_frame(cronopio_console_t* c);
void cronopio_console_end_frame(cronopio_console_t* c);

/* Initialise the palette region of a freshly-loaded cart's heap with the
 * default 32-colour palette. fb_region/pal_region are heap-relative
 * offsets resolved via cvm_image_get_region; both must already be valid.
 * The framebuffer region is left zeroed (loader already zero-fills it). */
void cronopio_console_seed_palette(uint8_t* heap, uint32_t pal_offset);

/* Pack the cart's 8 bpp framebuffer + palette (read from `heap` at the
 * region offsets stored in `c`) into a 32-bit RGBA buffer supplied by the
 * platform shell. `dst` must be SCREEN_W*SCREEN_H pixels. */
void cronopio_console_blit_rgba(const cronopio_console_t* c,
                                const uint8_t* heap, uint32_t* dst);

/* Render `frames` audio frames into a 16-bit signed stereo buffer
 * (length frames*2). */
void cronopio_console_mix(cronopio_console_t* c, int16_t* dst, int frames);

/* APU / input glue used by the syscall layer and the platform shell. */
void     cron_apu_tone   (cronopio_console_t* c, int ch, int wave,
                          uint32_t freq_mhz, int vol, int pan);
void     cron_apu_stop   (cronopio_console_t* c, int ch);
void     cron_apu_master (cronopio_console_t* c, int vol_q8);
void     cron_apu_sample (cronopio_console_t* c, int slot, uint32_t offset,
                          uint32_t len, uint32_t rate, int u8, uint32_t mem_size);
/* Queue `nframes` 16-bit stereo frames from heap+off into the stream ring;
 * returns frames actually queued. cron_apu_stream_free returns ring space. */
int      cron_apu_stream     (cronopio_console_t* c, uint32_t off, int nframes, uint32_t mem_size);
int      cron_apu_stream_free(cronopio_console_t* c);
void     cron_apu_pcm    (cronopio_console_t* c, int v, int slot,
                          uint32_t pitch_q16, int vol, int pan, int loop);
void     cron_apu_env    (cronopio_console_t* c, int v, int attack_ms,
                          int decay_ms, int sustain, int release_ms);
void     cron_apu_note_off(cronopio_console_t* c, int v);

/* MOD player. cron_mod_play parses the .mod blob at heap offset `off`
 * (len bytes) and starts it on voices 0..n_channels-1; returns 0 on success,
 * -1 if the blob isn't a recognised MOD. cron_mod_tick advances one tick and
 * is called by the mixer. */
int      cron_mod_play(cronopio_console_t* c, uint32_t off, uint32_t len, int loop);
void     cron_mod_stop(cronopio_console_t* c);
void     cron_mod_tick(cronopio_console_t* c);

/* MIDI + SoundFont synth (midisynth.c). The handle is an opaque cron_synth*
 * stored in cronopio_console_t::synth. _load_default and _load_mem touch tsf
 * directly and must run off the audio thread (init / cart thread); the _send/
 * _select/_free/_reset/_volume calls are lock-free producers feeding an SPSC
 * ring the mixer drains in cron_synth_render. */
void*    cron_synth_create(void);
void     cron_synth_destroy(void* synth);
int      cron_synth_load_default(void* synth, const char* path);   /* slot 0 (BIOS); 0/-1 */
int      cron_synth_load_mem(void* synth, const void* sf2, int len);/* cart: -> slot>=1 or -1 */
void     cron_synth_free_slot(void* synth, int slot);
void     cron_synth_select(void* synth, int slot);                 /* 0 = default bank */
void     cron_synth_send(void* synth, int status, int d1, int d2); /* one MIDI message */
void     cron_synth_reset(void* synth);                            /* all notes off */
void     cron_synth_volume(void* synth, int vol);                  /* music master 0..255 */
void     cron_synth_render(void* synth, int16_t* out, int frames); /* audio thread */

void     cron_input_set_pad     (cronopio_console_t* c, int player, uint32_t mask);
uint32_t cron_input_pad         (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_pressed (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_released(const cronopio_console_t* c, int player);

/* GPU primitives. All take the console (for fb offset + draw state: clip,
 * camera, palette remap) and the heap base. Coordinates are in world space;
 * the camera offset and clip rect are applied per pixel. cls is the one
 * exception: it clears the whole framebuffer, ignoring the draw state. */
void cron_gpu_reset_state(cronopio_console_t* c);   /* clip=full, cam=0, pal=identity */

void cron_gpu_cls  (cronopio_console_t* c, uint8_t* heap, int color);
void cron_gpu_pset (cronopio_console_t* c, uint8_t* heap, int x, int y, int color);
int  cron_gpu_pget (cronopio_console_t* c, uint8_t* heap, int x, int y);
void cron_gpu_rect (cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_rectb(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_line (cronopio_console_t* c, uint8_t* heap, int x0, int y0, int x1, int y1, int color);
void cron_gpu_circ (cronopio_console_t* c, uint8_t* heap, int x, int y, int r, int color);
void cron_gpu_circb(cronopio_console_t* c, uint8_t* heap, int x, int y, int r, int color);
void cron_gpu_elli (cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_ellib(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_tri  (cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color);
void cron_gpu_trib (cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color);
void cron_gpu_fill (cronopio_console_t* c, uint8_t* heap, int x, int y, int color);
void cron_gpu_text (cronopio_console_t* c, uint8_t* heap, const char* s, int len, int x, int y, int color);
void cron_gpu_blit_raw(cronopio_console_t* c, uint8_t* heap,
                       const uint8_t* src, int sw, int sh, int dx, int dy);

/* Draw-state setters (clamped/normalised here). */
void cron_gpu_clip  (cronopio_console_t* c, int x, int y, int w, int h);
void cron_gpu_clip_reset(cronopio_console_t* c);
void cron_gpu_camera(cronopio_console_t* c, int x, int y);
void cron_gpu_pal   (cronopio_console_t* c, int c0, int c1);
void cron_gpu_pal_reset(cronopio_console_t* c);

/* Image / tilemap banks (mem_size is the cart's addressable byte count, used
 * to bounds-check the registered region). Returns 0 on success, -1 if the
 * slot is out of range or the region escapes cart memory. */
int  cron_gpu_image  (cronopio_console_t* c, int slot, uint32_t offset, int w, int h, uint32_t mem_size);
int  cron_gpu_tilemap(cronopio_console_t* c, int slot, uint32_t offset, int w, int h, int img, uint32_t mem_size);

/* Blit a (sx,sy,w,h) region of image bank `img` to (dx,dy). colkey: a source
 * index to treat as transparent, or -1 for fully opaque. Negative w/h flip
 * horizontally/vertically (Pyxel convention). */
void cron_gpu_blt (cronopio_console_t* c, uint8_t* heap, int img,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey);
/* Blit a pixel region (sx,sy,w,h) of tilemap `tm` to (dx,dy). colkey as blt. */
void cron_gpu_bltm(cronopio_console_t* c, uint8_t* heap, int tm,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey);

/* Rotozoom blit: like blt, but the sprite is scaled (scale_q16, Q16.16,
 * 0x10000 = 1.0) and rotated (rotate_deg, clockwise) around its centre,
 * which is placed at (dx + w/2, dy + h/2). Nearest-neighbour sampling. */
void cron_gpu_blt_ex(cronopio_console_t* c, uint8_t* heap, int img,
                     int dx, int dy, int sx, int sy, int w, int h,
                     int colkey, int rotate_deg, int scale_q16);

/* --- Textured-rasteriser accelerators (the perf escape hatch for
 * software 3D — DOOM's R_DrawColumn / R_DrawSpan in native C). They honour
 * the clip rect but ignore camera and the draw palette: the active colormap
 * (cron_gpu_cmap) is the only remap, matching DOOM's light-diminishing. --- */

/* Set the active 256-byte colormap (heap offset); set=0 selects identity. */
void cron_gpu_cmap(cronopio_console_t* c, uint32_t offset, int set);

/* Vertical textured column at screen x, rows [y0,y1]. Source is a column of
 * (mask+1) bytes (mask = height-1, power of two); the texture coordinate is
 * frac (Q16.16) advancing by step per row. fb = cmap[src[(frac>>16)&mask]]. */
void cron_gpu_tcol(cronopio_console_t* c, uint8_t* heap, int x, int y0, int y1,
                   uint32_t src_off, int mask, int32_t frac, int32_t step);

/* Masked vertical column: like cron_gpu_tcol but the source is addressed
 * LINEARLY (no power-of-two wrap) — fb = cmap[src[frac>>16]]. For DOOM-style
 * masked posts (sprites, weapon) where each opaque run indexes its own post
 * data and the caller guarantees the index stays in range. The syscall bounds
 * the source by the actual index span, not a (here meaningless) mask. */
void cron_gpu_tcolm(cronopio_console_t* c, uint8_t* heap, int x, int y0, int y1,
                    uint32_t src_off, int32_t frac, int32_t step);

/* Horizontal textured span at screen y, cols [x0,x1], over a 64x64 source.
 * (u,v) Q16.16 advance by (du,dv); index = ((v>>16)&63)*64 + ((u>>16)&63). */
void cron_gpu_tspan(cronopio_console_t* c, uint8_t* heap, int y, int x0, int x1,
                    uint32_t src_off, int32_t u, int32_t v, int32_t du, int32_t dv);

/* --- 3D triangle submission (PSX / 486-Pentium-style software rasteriser).
 * The cart transforms+projects in fixed point, then submits batches of
 * screen-space triangles. Honours the clip rect (viewport) and the active
 * cmap (gouraud/light); ignores camera and the draw palette. --- */

/* Bind/clear the depth buffer (320x240 int32 in cart memory; set=0 = none). */
void cron_gpu_zbuf  (cronopio_console_t* c, uint32_t offset, int set);
/* Fill the bound depth buffer with `far` (typically INT32_MAX). */
void cron_gpu_zclear(cronopio_console_t* c, uint8_t* heap, int32_t far);

/* Draw count/3 triangles from a vertex array at verts_off (CRONOPIO_VERT_BYTES
 * each). mode is a CRONOPIO_POLY_* bitmask. arg = flat colour index, or the
 * image-bank slot when CRONOPIO_POLY_TEX is set. colkey: transparent texel
 * index for textured draws, or -1. */
void cron_gpu_polys (cronopio_console_t* c, uint8_t* heap, int mode,
                     uint32_t verts_off, int count, int arg, int colkey);

#endif
