#ifndef CRONOPIO_CONSOLE_H
#define CRONOPIO_CONSOLE_H

#include <stdint.h>
#include <stddef.h>

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
/* Per-cart save blob ("memory card"). Host-allocated, grows on demand: the
 * region starts small and auto-grows when the cart writes more (or pre-reserves
 * via cron_save_reserve), up to CRONOPIO_SAVE_MAX. The host persists
 * save[0..save_len] to <cart>.sav and reloads it on boot (live bytes only). */
#define CRONOPIO_SAVE_DEFAULT (64u * 1024u)         /* initial capacity */
#define CRONOPIO_SAVE_MAX     (64u * 1024u * 1024u) /* hard cap, anti-runaway */
#define CRONOPIO_IMAGE_SLOTS     8
#define CRONOPIO_TILEMAP_SLOTS   8
#define CRONOPIO_PAL_BANK_SLOTS 32       /* per-line palette remap tables — bank 0 is the identity sentinel ("no swap"); banks 1..31 are user-defined */
#define CRONOPIO_BLEND_SLOTS     8       /* 256x256 blend LUTs — slot 0 is the opaque sentinel ("no blend"); slots 1..7 are user-defined */
#define CRONOPIO_TILE_SIZE       8

/* 3D triangle submission. A vertex is 7 little-endian i32 words in cart
 * memory: x, y (screen px), z (depth), u, v (texcoords Q16.16), w (1/clip-w
 * style depth for perspective), c (gouraud light/index). cron_polys draws
 * count/3 triangles. mode is a bitmask. */
#define CRONOPIO_VERT_WORDS      9            /* x,y,z,u,v,w,c,lu,lv */
#define CRONOPIO_VERT_BYTES      (CRONOPIO_VERT_WORDS * 4)

/* World-space vertex for cron_xform_polys (the host-side T&L path). 8 little-
 * endian floats in cart memory: x, y, z (world), u, v (texels), lu, lv
 * (lightmap UVs in lumels), light (Gouraud light row). The host transforms
 * each vertex by the bound MVP (cron_mvp), near-clips per triangle, perspective-
 * divides + maps to the clip-rect viewport, then rasterises through the same
 * inner loop as cron_polys. Same CRONOPIO_POLY_* mode flags. */
#define CRONOPIO_WVERT_WORDS     8            /* x,y,z,u,v,lu,lv,light (all f32) */
#define CRONOPIO_WVERT_BYTES     (CRONOPIO_WVERT_WORDS * 4)
#define CRONOPIO_POLY_GOURAUD    (1u << 0)   /* interpolate vertex c via cmap */
#define CRONOPIO_POLY_TEX        (1u << 1)   /* affine texture from image bank */
#define CRONOPIO_POLY_PERSP      (1u << 2)   /* perspective-correct (needs w)  */
#define CRONOPIO_POLY_ZTEST      (1u << 3)   /* depth test/write the z-buffer  */
#define CRONOPIO_POLY_LIGHTMAP   (1u << 4)   /* per-texel light: out = colormap[lm[lu,lv]*256 + tex] */
#define CRONOPIO_POLY_CLAMP      (1u << 5)   /* clamp texcoords to edge instead of wrapping (alias skins) */
#define CRONOPIO_POLY_TURB       (1u << 6)   /* per-pixel texcoord turbulence (water/lava ripple) */

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

    /* pcm: a direct descriptor into cart memory (heap offsets, in samples).
     * pcm_pos/step are Q16.16. pcm_looplen==0 means one-shot (stop at pcm_len);
     * >0 loops [pcm_loopstart, +pcm_looplen). */
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

    /* Per-line palette remap tables for cron_bltm_raster (cron_raster_t.
     * pal_bank). bank 0 is the identity (no swap); banks 1..31 are 256-byte
     * remap tables in cart memory referenced by heap offset. used==0 means
     * unbound — bltm_raster treats a reference to an unbound bank as
     * identity (silent, no abort). */
    struct {
        uint32_t offset;
        int      used;
    } pal_banks[CRONOPIO_PAL_BANK_SLOTS];

    /* Per-image-bank tile-animation tables. When a tilemap cell's tile
     * index matches an anim's `src_tile`, the bltm family substitutes
     * the current frame's tile index (computed from frame_count and the
     * anim's period_frames). Anim disabled when offset==0 or count==0;
     * tablesize bound by the cart but typically <16 anims per bank. The
     * table itself is an array of cron_tile_anim_t in cart heap. */
    struct {
        uint32_t table_offset;
        int      count;
    } tile_anims[CRONOPIO_IMAGE_SLOTS];

    /* 256x256 blend LUTs (input src x dst -> output palette idx). Slot 0
     * is the opaque sentinel: put_px writes src directly (current
     * behaviour). Slots 1..7 are user-registered 64 KB tables in cart
     * heap. blend_active selects which slot put_px consults; 0 disables
     * blending. Cart sets via cron_gpu_blend_set; persists across draws
     * until changed (camera/clip pattern). */
    struct {
        uint32_t offset;
        int      used;
    } blend_tables[CRONOPIO_BLEND_SLOTS];
    int blend_active;

    /* Active colormap for the textured-rasteriser accelerators (tcol/tspan):
     * a 256-byte remap in cart memory (e.g. a DOOM light colormap). When
     * cmap_set is 0 the accelerators write source indices unremapped. */
    uint32_t cmap_offset;
    int      cmap_set;

    /* Optional depth buffer: 320x240 int32 in cart memory, written/tested by
     * CRONOPIO_POLY_ZTEST triangles. Nearer = smaller z. */
    uint32_t zbuf_offset;
    int      zbuf_set;

    /* CRONOPIO_POLY_LIGHTMAP: per-texel lighting like Quake. `lm` is a small
     * per-surface light grid (8bpp, each byte a colormap ROW) sampled by the
     * triangle's lu/lv; `colormap` is a levels*256 table (e.g. Quake's 64x256
     * host_colormap) indexed [light*256 + texel]. Both in cart memory. */
    uint32_t lm_offset;   int lm_w, lm_h, lm_set;
    uint32_t colormap_offset; int colormap_levels, colormap_set;

    /* CRONOPIO_POLY_TURB: Quake-style per-pixel texcoord turbulence (water /
     * lava ripple). `turb_phase` advances the sine animation (the cart passes
     * a per-frame value, period 128); `turb_amp` is the ripple amplitude in
     * texels. Bound by cron_gpu_turb. */
    int turb_phase, turb_amp, turb_set;

    /* Bound MVP matrix for cron_xform_polys — row-major 4x4 (m[r*4+c]), point
     * transform p' = M * (x,y,z,1). Set via cron_gpu_mvp; mvp_set==0 makes
     * xform_polys a no-op. Re-bound every entity in Quake (one for the world,
     * one per brush ent / alias model). */
    float mvp[16];
    int   mvp_set;

    /* audio */
    cron_voice_t       voices[CRONOPIO_AUDIO_CHANS];
    cron_sample_bank_t samples[CRONOPIO_SAMPLE_SLOTS];
    int                master_vol_q8;            /* 0..256 */
    void*              synth;                     /* MIDI+SoundFont synth (midisynth.c, opaque cron_synth*) */
    void*              ogg;                     /* streaming OGG music (cron_ogg.c, opaque) */

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
    int32_t  mouse_x, mouse_y;             /* absolute pos in cart 320x240 coords */
    uint32_t mouse_buttons;                /* bitmask: 1=L, 2=R, 4=M, 8=X1, 16=X2 */
    /* Relative motion + wheel — host accumulates between cart reads; cron_mouse_delta
     * and cron_mouse_wheel zero them on read (consume-then-reset). Independent of the
     * relative-mode toggle: even in absolute mode, a cart can read deltas if it wants. */
    int32_t  mouse_dx, mouse_dy;           /* accumulated relative motion (cart coords) */
    int32_t  mouse_wheel;                  /* accumulated vertical wheel ticks (+=up) */
    int      cursor_visible;               /* host shows OS cursor when 1 (default) */
    int      mouse_relative;               /* SDL relative-mouse mode when 1 (mouselook) */

    /* timing */
    uint64_t boot_ms;
    uint32_t frame_count;
    uint32_t prng_state;

    /* Cart-registered per-frame callback. The cart calls cron_set_frame(fn)
     * during its boot run; the syscall handler stores fn's FUNCS index here.
     * The platform shell invokes it via cvm_call once per 1/60 s tick. */
    int32_t  frame_fn_index;

    /* Optional present hook: the platform shell sets this so cron_present can
     * flush the framebuffer to screen mid-frame / during a blocking cart entry
     * (e.g. a DOOM loading screen drawn while D_DoomMain runs). NULL = the
     * default callback-driven model (present once per frame after the frame fn). */
    void   (*present_cb)(void* ud);
    void    *present_ud;

    /* cart exit signal — set by cron_exit syscall or the platform shell. */
    int      cart_exited;
    int32_t  exit_status;

    /* persistence — host-owned, malloc'd. save_cap = current capacity (grows),
     * save_len = live bytes (persisted to <cart>.sav, returned by save_read). */
    uint8_t *save;
    uint32_t save_cap;
    uint32_t save_len;
    int      save_dirty;
} cronopio_console_t;

void cronopio_console_init(cronopio_console_t* c);
void cronopio_console_begin_frame(cronopio_console_t* c);
void cronopio_console_end_frame(cronopio_console_t* c);

/* Save region (memory card) management — see the save fields above. */
int  cronopio_save_reserve(cronopio_console_t* c, uint32_t need);
void cronopio_save_free(cronopio_console_t* c);

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
void     cron_apu_pcm_params(cronopio_console_t* c, int v, int vol, int pan);
void     cron_apu_env    (cronopio_console_t* c, int v, int attack_ms,
                          int decay_ms, int sustain, int release_ms);
void     cron_apu_note_off(cronopio_console_t* c, int v);

/* MIDI + SoundFont synth (midisynth.c). The handle is an opaque cron_synth*
 * stored in cronopio_console_t::synth. _load_default and _load_mem touch tsf
 * directly and must run off the audio thread (init / cart thread); the _send/
 * _select/_free/_reset/_volume calls are lock-free producers feeding an SPSC
 * ring the mixer drains in cron_synth_render. */
void*    cron_synth_create(void);
void     cron_synth_destroy(void* synth);
int      cron_synth_load_default_mem(void* synth, const void* sf2, int len); /* slot 0 (BIOS); 0/-1 */
int      cron_synth_load_mem(void* synth, const void* sf2, int len);/* cart: -> slot>=1 or -1 */
void     cron_synth_free_slot(void* synth, int slot);
void     cron_synth_select(void* synth, int slot);                 /* 0 = default bank */
void     cron_synth_send(void* synth, int status, int d1, int d2); /* one MIDI message */
void     cron_synth_reset(void* synth);                            /* all notes off */
void     cron_synth_volume(void* synth, int vol);                  /* music master 0..255 */
void     cron_synth_render(void* synth, int16_t* out, int frames); /* audio thread */

/* The default GM "BIOS" SoundFont, embedded into the binary (bios_sf2.c via
 * C23 #embed) so the executable is self-contained. console_init loads it. */
extern const unsigned char cron_bios_sf2[];
extern const size_t        cron_bios_sf2_len;

void     cron_input_set_pad     (cronopio_console_t* c, int player, uint32_t mask);
uint32_t cron_input_pad         (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_pressed (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_released(const cronopio_console_t* c, int player);

/* Mouse — see [docs/syscalls.md]: the host pushes motion / buttons / wheel,
 * the cart consumes deltas via cron_mouse_delta / cron_mouse_wheel (which
 * reset the accumulators on read), and toggles cursor + relative mode. */
void     cron_input_mouse_motion        (cronopio_console_t* c, int abs_x, int abs_y, int dx, int dy);
void     cron_input_mouse_button        (cronopio_console_t* c, int button, int down);
void     cron_input_mouse_wheel         (cronopio_console_t* c, int ticks);
void     cron_input_set_cursor_visible  (cronopio_console_t* c, int show);
void     cron_input_set_mouse_relative  (cronopio_console_t* c, int enable);
void     cron_input_consume_mouse_delta (cronopio_console_t* c, int32_t* dx, int32_t* dy);
int32_t  cron_input_consume_mouse_wheel (cronopio_console_t* c);

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
/* Blit a pixel region (sx,sy,w,h) of tilemap `tm` to (dx,dy). colkey as blt.
 * Tile cells use the layout:
 *   0xFFFF        = empty cell
 *   bit 15        = HFLIP
 *   bit 14        = VFLIP
 *   bits 13..0    = tile index in the image bank (0..16383)
 * Backwards-compatible with old tilemaps whose cells were all-index (values
 * < 0x4000 land with HFLIP=0, VFLIP=0, index unchanged). */
void cron_gpu_bltm(cronopio_console_t* c, uint8_t* heap, int tm,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey);

/* Tilemap blit with per-scanline parameter overrides — HDMA-style raster
 * effects. The table at heap+table_off is an array of cron_raster_t (see
 * sdk/include/cronopio.h), indexed by destination y. Each entry carries
 * scroll_x/scroll_y deltas (added to the call's sx/sy) and a palette
 * offset (added to the sampled colour index before write). One host syscall
 * walks the whole table, zero per-line VM round-trips. */
void cron_gpu_bltm_raster(cronopio_console_t* c, uint8_t* heap, int tm,
                          int dx, int dy, int sx, int sy, int w, int h,
                          int colkey, uint32_t table_off);

/* Affine ("Mode-7") tilemap blit. The table at heap+table_off is an array
 * of cron_affine_t, indexed by destination y. Each entry carries
 * {u, v, du, dv} in Q16.16 — the texture coord at screen-x=0 and its
 * increment per pixel along x. Texture coords wrap modulo the tilemap
 * size (infinite floor / sky). */
void cron_gpu_bltm_affine(cronopio_console_t* c, uint8_t* heap, int tm,
                          int dx, int dy, int w, int h, int colkey,
                          uint32_t table_off);

/* Register a 256-byte palette remap table as bank `slot` (1..31). Bank 0
 * is reserved as the identity sentinel and cannot be bound. Per-scanline
 * raster tables (cron_raster_t.pal_bank) reference these by index. */
int cron_gpu_palette_bank(cronopio_console_t* c, int slot, uint32_t offset,
                          uint32_t mem_size);

/* Register a tile-animation table for image bank `img_slot`. The table is
 * an array of `count` cron_tile_anim_t entries at heap+table_offset; each
 * entry says "tilemap cells whose tile_idx == src_tile substitute through
 * frames[(frame_count / period) MOD num_frames]". count=0 (or
 * table_offset=0) clears the bank's anim. Returns 0 on success. */
int cron_gpu_tile_anim(cronopio_console_t* c, int img_slot,
                       uint32_t table_offset, int count);

/* Register a 256x256 blend lookup (`out = table[src*256 + dst]`) as
 * slot 1..7. Slot 0 is the opaque sentinel. Returns 0 on success, -1 on
 * invalid slot / OOB offset (the table must fit 64 KB at heap+offset). */
int cron_gpu_blend_table(cronopio_console_t* c, int slot, uint32_t offset,
                         uint32_t mem_size);
/* Set the active blend slot for subsequent draws. 0 disables blending
 * (opaque writes, current behaviour). Persists across draws until
 * changed — same pattern as cron_camera / cron_clip / cron_cmap. */
void cron_gpu_blend_set(cronopio_console_t* c, int slot);

/* Rotozoom blit: like blt, but the sprite is scaled (scale_q16, Q16.16,
 * 0x10000 = 1.0) and rotated (rotate_deg, clockwise) around its centre,
 * which is placed at (dx + w/2, dy + h/2). Nearest-neighbour sampling. */
void cron_gpu_blt_ex(cronopio_console_t* c, uint8_t* heap, int img,
                     int dx, int dy, int sx, int sy, int w, int h,
                     int colkey, int rotate_deg, int scale_q16);

/* Flip-aware blit (no rotate, no scale). flags = bit 0 (HFLIP) | bit 1
 * (VFLIP). The fast path for character sprites that face left/right or
 * flip upside-down. cron_gpu_blt_ex covers the rotation+scale case. */
void cron_gpu_blt_flip(cronopio_console_t* c, uint8_t* heap, int img,
                       int dx, int dy, int sx, int sy, int w, int h,
                       int colkey, int flags);

/* Scale + flip (no rotate). scale_q16 is Q16.16 (0x10000 = 1.0). Anchored
 * at top-left (dx, dy). flags as cron_gpu_blt_flip. Cheaper than blt_ex
 * when only scaling is needed (no trig). */
void cron_gpu_blt_scale(cronopio_console_t* c, uint8_t* heap, int img,
                        int dx, int dy, int sx, int sy, int w, int h,
                        int colkey, int scale_q16, int flags);

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

/* CRONOPIO_POLY_LIGHTMAP bindings. lightmap: a per-surface 8bpp light grid
 * (each byte a colormap row); set=0 disables. colormap: a levels*256 table
 * indexed [light*256 + texel]; set=0 disables (falls back to cmap/raw). */
void cron_gpu_lightmap(cronopio_console_t* c, uint32_t offset, int w, int h, int set);
/* CRONOPIO_POLY_TURB binding: phase advances the ripple (period 128), amp is
 * the texel amplitude. set=0 disables turbulence. */
void cron_gpu_turb(cronopio_console_t* c, int phase, int amp, int set);
void cron_gpu_colormap(cronopio_console_t* c, uint32_t offset, int levels, int set);

/* Draw count/3 triangles from a vertex array at verts_off (CRONOPIO_VERT_BYTES
 * each). mode is a CRONOPIO_POLY_* bitmask. arg = flat colour index, or the
 * image-bank slot when CRONOPIO_POLY_TEX is set. colkey: transparent texel
 * index for textured draws, or -1. */
void cron_gpu_polys (cronopio_console_t* c, uint8_t* heap, int mode,
                     uint32_t verts_off, int count, int arg, int colkey);

/* Bind the model-view-projection matrix used by cron_gpu_xform_polys. mat
 * is 16 little-endian floats in row-major order (p' = M * (x,y,z,1)).
 * set=0 unbinds (xform_polys then no-ops). */
void cron_gpu_mvp(cronopio_console_t* c, const float* mat, int set);

/* Host-side T&L: like cron_gpu_polys but verts are in world space (each
 * CRONOPIO_WVERT_BYTES). The host transforms each vert by the bound MVP,
 * near-clips per source triangle, perspective-divides + maps to the bound
 * clip rect (viewport), then rasterises through the same inner loop as
 * cron_gpu_polys. count is the number of input verts (count/3 source tris;
 * each may expand to 0, 1 or 2 screen-space tris after clipping). */
void cron_gpu_xform_polys(cronopio_console_t* c, uint8_t* heap, int mode,
                          uint32_t verts_off, int count, int arg, int colkey);

#endif
