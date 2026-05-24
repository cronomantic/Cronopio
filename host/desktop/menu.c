/* System-menu overlay. See menu.h. Self-contained: SDL for drawing + input,
 * dirent/stat for the file browser, the shared font8x8 ROM for text. */

#include "menu.h"
#include "font8x8.h"
#include "cvm.h"          /* CVM_SEC_META — cart metadata section */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#define NAME_MAX_      256
#define MAX_ENTRIES    1024
#define GLYPH          8           /* font cell size */
#define TEXT_SCALE     2           /* on-screen glyph = 16px */
#define CW             (GLYPH * TEXT_SCALE)
#define CH             (GLYPH * TEXT_SCALE)
#define ROW_H          (CH + 4)

typedef enum { SCR_MAIN, SCR_BROWSE, SCR_CONTROLS, SCR_JOYSTICK } screen_t;

typedef struct {
    char name[NAME_MAX_];
    int  is_dir;
    char title[64];           /* cart title from CVM_SEC_META, "" if none */
} entry_t;

/* Cart metadata (CVM_SEC_META payload), read without loading the cart. */
typedef struct { char title[64], author[48], controls[192]; } cart_meta_t;

struct menu {
    int       open;
    screen_t  screen;
    int       sel;          /* selected row on the active screen */
    int       scroll;       /* first visible row for scrollable lists */

    SDL_Renderer* ren;
    SDL_Texture*  font;     /* 128x48 glyph atlas (16x6 cells), white on clear */
    int       win_w, win_h;

    host_cfg_t*  cfg;
    menu_host_t  host;
    int          has_cart;

    char      cur_dir[1024];
    entry_t   entries[MAX_ENTRIES];
    int       n_entries;

    int       capturing;    /* waiting for a key/button to bind */
    int       capture_btn;  /* which PAD_* is being rebound */

    char      status[256];
    const char* joy_name;

    int         meta_sel;   /* browse entry whose META is cached below (-1 = none) */
    cart_meta_t meta;       /* author/controls of the focused cart, for the footer */
    long        save_size;  /* size of the focused cart's <cart>.sav, -1 if none */
};

/* Size of a file in bytes, or -1 if it doesn't exist. */
static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* ---- glyph atlas ------------------------------------------------------- */

static SDL_Texture* build_font_atlas(SDL_Renderer* ren) {
    const int cols = 16, rows = 6, aw = cols * GLYPH, ah = rows * GLYPH;
    uint32_t* px = (uint32_t*)calloc((size_t)aw * ah, sizeof(uint32_t));
    if (!px) return NULL;
    for (int gi = 0; gi < 96; ++gi) {
        int cx0 = (gi % cols) * GLYPH, cy0 = (gi / cols) * GLYPH;
        for (int ry = 0; ry < GLYPH; ++ry) {
            uint8_t bits = font8x8[gi][ry];
            for (int rx = 0; rx < GLYPH; ++rx)
                if (bits & (1u << rx))                 /* bit 0 = leftmost col */
                    px[(cy0 + ry) * aw + (cx0 + rx)] = 0xFFFFFFFFu;
        }
    }
    SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, aw, ah);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, aw * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    free(px);
    return t;
}

static void draw_text(menu_t* m, int x, int y, SDL_Color c, const char* s) {
    SDL_SetTextureColorMod(m->font, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(m->font, c.a);
    for (; *s; ++s, x += CW) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7F) ch = '?';
        int gi = ch - 0x20;
        SDL_Rect src = { (gi % 16) * GLYPH, (gi / 16) * GLYPH, GLYPH, GLYPH };
        SDL_Rect dst = { x, y, CW, CH };
        SDL_RenderCopy(m->ren, m->font, &src, &dst);
    }
}

static void fill(menu_t* m, int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(m->ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(m->ren, &r);
}

/* ---- file browser ------------------------------------------------------ */

static int ext_is(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n <= e) return 0;
    const char* s = name + (n - e);
    for (size_t i = 0; i < e; ++i) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Cartridge files are .crom; .bin is still accepted for older/dev artifacts. */
static int has_cart_ext(const char* name) {
    return ext_is(name, ".crom") || ext_is(name, ".bin");
}

static uint32_t rd_u32le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a cart's CVM_SEC_META without loading it: parse the 24-byte header and
 * the section table, then read just the META blob. Returns 1 if a title was
 * found. Format of the blob: "CMTA", u8 version, then {u8 key; u16 len LE;
 * bytes} records (key 1=title, 2=author, 3=controls). */
static int cart_read_meta(const char* path, cart_meta_t* out) {
    memset(out, 0, sizeof *out);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[24];
    if (fread(hdr, 1, 24, f) != 24 || memcmp(hdr, "CVM1", 4) != 0) { fclose(f); return 0; }
    uint32_t count = rd_u32le(hdr + 12), toff = rd_u32le(hdr + 16);
    uint32_t moff = 0, msz = 0;
    if (fseek(f, (long)toff, SEEK_SET) == 0) {
        for (uint32_t i = 0; i < count; ++i) {
            unsigned char e[16];
            if (fread(e, 1, 16, f) != 16) break;
            if (rd_u32le(e) == (uint32_t)CVM_SEC_META) {
                moff = rd_u32le(e + 4); msz = rd_u32le(e + 8); break;
            }
        }
    }
    if (msz == 0 || msz > (1u << 16)) { fclose(f); return 0; }
    unsigned char* blob = (unsigned char*)malloc(msz);
    int ok = blob && fseek(f, (long)moff, SEEK_SET) == 0
                  && fread(blob, 1, msz, f) == msz;
    fclose(f);
    if (!ok) { free(blob); return 0; }

    int got = 0;
    if (msz >= 5 && memcmp(blob, "CMTA", 4) == 0) {
        uint32_t p = 5;                              /* skip magic + version */
        while (p + 3 <= msz) {
            unsigned key = blob[p];
            uint32_t len = (uint32_t)blob[p+1] | ((uint32_t)blob[p+2] << 8);
            p += 3;
            if (p + len > msz) break;
            char*  dst = NULL; size_t cap = 0;
            if      (key == 1) { dst = out->title;    cap = sizeof out->title; }
            else if (key == 2) { dst = out->author;   cap = sizeof out->author; }
            else if (key == 3) { dst = out->controls; cap = sizeof out->controls; }
            if (dst) {
                size_t c = len < cap - 1 ? len : cap - 1;
                memcpy(dst, blob + p, c); dst[c] = '\0';
                if (key == 1) got = 1;
            }
            p += len;
        }
    }
    free(blob);
    return got;
}

static int entry_cmp(const void* a, const void* b) {
    const entry_t* ea = (const entry_t*)a;
    const entry_t* eb = (const entry_t*)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir; /* dirs first */
    return strcmp(ea->name, eb->name);
}

static void path_join(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    if (n && (dir[n-1] == '/' || dir[n-1] == '\\'))
        snprintf(out, cap, "%s%s", dir, name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

/* Replace cur_dir with its parent (handles trailing slash and drive roots). */
static void go_parent(menu_t* m) {
    char* d = m->cur_dir;
    size_t n = strlen(d);
    while (n > 1 && (d[n-1] == '/' || d[n-1] == '\\')) d[--n] = '\0';
    char* slash = strrchr(d, '/');
    char* bslash = strrchr(d, '\\');
    char* cut = slash > bslash ? slash : bslash;
    if (!cut) return;                       /* nothing to ascend to */
    if (cut == d) cut[1] = '\0';            /* keep the root "/" */
    else          *cut = '\0';
    /* "C:" -> "C:/" so the next scan reads the drive root, not the cwd. */
    n = strlen(d);
    if (n == 2 && d[1] == ':') { d[2] = '/'; d[3] = '\0'; }
}

static void scan_dir(menu_t* m) {
    m->n_entries = 0;
    m->sel = 0;
    m->scroll = 0;

    DIR* dp = opendir(m->cur_dir);
    if (!dp) {
        snprintf(m->status, sizeof(m->status), "Cannot open %s", m->cur_dir);
        return;
    }
    /* ".." first, unless we are already at a filesystem root. */
    if (m->n_entries < MAX_ENTRIES) {
        snprintf(m->entries[m->n_entries].name, NAME_MAX_, "..");
        m->entries[m->n_entries].is_dir = 1;
        m->n_entries++;
    }
    struct dirent* de;
    while ((de = readdir(dp)) && m->n_entries < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[1300];
        path_join(full, sizeof(full), m->cur_dir, de->d_name);
        struct stat st;
        int is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        if (!is_dir && !has_cart_ext(de->d_name)) continue;   /* dirs + cart files */
        entry_t* en = &m->entries[m->n_entries];
        snprintf(en->name, NAME_MAX_, "%s", de->d_name);
        en->is_dir = is_dir;
        en->title[0] = '\0';
        if (!is_dir) {
            cart_meta_t mt;
            if (cart_read_meta(full, &mt)) snprintf(en->title, sizeof en->title, "%s", mt.title);
        }
        m->n_entries++;
    }
    closedir(dp);
    /* Sort everything after the ".." sentinel. */
    if (m->n_entries > 1)
        qsort(m->entries + 1, (size_t)(m->n_entries - 1), sizeof(entry_t), entry_cmp);
}

/* ---- screen entry / navigation ----------------------------------------- */

#define MAIN_LOAD   0
#define MAIN_RESET  1
#define MAIN_CTRL   2
#define MAIN_JOY    3
#define MAIN_RESUME 4
#define MAIN_QUIT   5
#define MAIN_COUNT  6

static void enter_main(menu_t* m)     { m->screen = SCR_MAIN;     m->sel = 0; m->scroll = 0; }
static void enter_controls(menu_t* m) { m->screen = SCR_CONTROLS; m->sel = 0; m->scroll = 0; m->capturing = 0; }
static void enter_joystick(menu_t* m) { m->screen = SCR_JOYSTICK; m->sel = 0; m->scroll = 0; m->capturing = 0; }
static void enter_browse(menu_t* m)   { m->screen = SCR_BROWSE; m->capturing = 0; scan_dir(m); }

static int rows_on_screen(const menu_t* m) {
    switch (m->screen) {
        case SCR_BROWSE:    return m->n_entries;
        case SCR_CONTROLS:  return PAD_BTN_COUNT;
        case SCR_JOYSTICK:  return PAD_BTN_COUNT;
        default:            return MAIN_COUNT;
    }
}

static void move_sel(menu_t* m, int dir) {
    int n = rows_on_screen(m);
    if (n <= 0) return;
    m->sel = (m->sel + dir + n) % n;
}

/* ---- activating the selected row --------------------------------------- */

static void activate_main(menu_t* m) {
    switch (m->sel) {
        case MAIN_LOAD:   enter_browse(m); break;
        case MAIN_RESET:
            if (m->has_cart) { m->host.reset_cart(m->host.ud); menu_close(m); }
            else snprintf(m->status, sizeof(m->status), "No cartridge loaded");
            break;
        case MAIN_CTRL:   enter_controls(m); break;
        case MAIN_JOY:    enter_joystick(m); break;
        case MAIN_RESUME:
            if (m->has_cart) menu_close(m);
            else snprintf(m->status, sizeof(m->status), "Load a cartridge first");
            break;
        case MAIN_QUIT:   m->host.quit(m->host.ud); break;
    }
}

static void activate_browse(menu_t* m) {
    if (m->sel < 0 || m->sel >= m->n_entries) return;
    entry_t* e = &m->entries[m->sel];
    if (e->is_dir) {
        if (!strcmp(e->name, "..")) go_parent(m);
        else {
            char nd[1024];
            path_join(nd, sizeof(nd), m->cur_dir, e->name);
            snprintf(m->cur_dir, sizeof(m->cur_dir), "%s", nd);
        }
        scan_dir(m);
        return;
    }
    /* a .bin: load it. */
    char full[1300];
    path_join(full, sizeof(full), m->cur_dir, e->name);
    if (m->host.load_cart(m->host.ud, full) == 0) {
        snprintf(m->cfg->last_dir, sizeof(m->cfg->last_dir), "%s", m->cur_dir);
        menu_close(m);
    } else {
        snprintf(m->status, sizeof(m->status), "Failed to load %s", e->name);
    }
}

/* ---- public API -------------------------------------------------------- */

menu_t* menu_create(SDL_Renderer* ren, int win_w, int win_h,
                    host_cfg_t* cfg, const menu_host_t* host) {
    menu_t* m = (menu_t*)calloc(1, sizeof(menu_t));
    if (!m) return NULL;
    m->ren = ren;
    m->win_w = win_w;
    m->win_h = win_h;
    m->cfg = cfg;
    m->host = *host;
    m->font = build_font_atlas(ren);
    if (!m->font) { free(m); return NULL; }
    return m;
}

void menu_destroy(menu_t* m) {
    if (!m) return;
    if (m->font) SDL_DestroyTexture(m->font);
    free(m);
}

int menu_is_open(const menu_t* m) { return m && m->open; }

void menu_open(menu_t* m, const char* start_dir, const char* joy_name, int has_cart) {
    m->open = 1;
    m->has_cart = has_cart;
    m->joy_name = joy_name;
    m->status[0] = '\0';
    snprintf(m->cur_dir, sizeof(m->cur_dir), "%s",
             (start_dir && *start_dir) ? start_dir : ".");
    /* With no cart yet, drop straight into the browser; otherwise the main menu. */
    if (has_cart) enter_main(m);
    else          enter_browse(m);
}

void menu_close(menu_t* m) { m->open = 0; m->capturing = 0; }

void menu_set_joy_name(menu_t* m, const char* name) { if (m) m->joy_name = name; }

/* Delete the focused cart's <cart>.sav (browser only). */
static void delete_focused_save(menu_t* m) {
    if (m->screen != SCR_BROWSE) return;
    if (m->sel <= 0 || m->sel >= m->n_entries || m->entries[m->sel].is_dir) return;
    char full[1300], sp[1320];
    path_join(full, sizeof full, m->cur_dir, m->entries[m->sel].name);
    snprintf(sp, sizeof sp, "%s.sav", full);
    if (remove(sp) == 0)
        snprintf(m->status, sizeof m->status, "Deleted save: %s", m->entries[m->sel].name);
    else
        snprintf(m->status, sizeof m->status, "No save to delete");
    m->meta_sel = -1;   /* refresh footer save size */
}

int menu_handle_event(menu_t* m, const SDL_Event* ev) {
    if (!m->open) return 0;

    /* Capture mode: the very next key / controller button becomes the binding. */
    if (m->capturing) {
        if (m->screen == SCR_CONTROLS && ev->type == SDL_KEYDOWN) {
            if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                ;                                  /* esc cancels (no rebind) */
            else
                m->cfg->key[m->capture_btn] = ev->key.keysym.scancode;
            m->capturing = 0;
            return 1;
        }
        if (m->screen == SCR_JOYSTICK && ev->type == SDL_CONTROLLERBUTTONDOWN) {
            m->cfg->gbtn[m->capture_btn] = (SDL_GameControllerButton)ev->cbutton.button;
            m->capturing = 0;
            return 1;
        }
        if (m->screen == SCR_JOYSTICK && ev->type == SDL_KEYDOWN &&
            ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            m->capturing = 0;                      /* cancel waiting for a button */
            return 1;
        }
        return 1;       /* swallow everything else while capturing */
    }

    /* Normal navigation — keyboard. */
    if (ev->type == SDL_KEYDOWN) {
        switch (ev->key.keysym.scancode) {
            case SDL_SCANCODE_UP:    move_sel(m, -1); return 1;
            case SDL_SCANCODE_DOWN:  move_sel(m, +1); return 1;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE:
            case SDL_SCANCODE_RIGHT: goto activate;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_LEFT:  goto back;
            case SDL_SCANCODE_DELETE: delete_focused_save(m); return 1;
            default: return 1;
        }
    }
    /* Normal navigation — game controller. */
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (ev->cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:   move_sel(m, -1); return 1;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_sel(m, +1); return 1;
            case SDL_CONTROLLER_BUTTON_A:         goto activate;
            case SDL_CONTROLLER_BUTTON_B:         goto back;
            default: return 1;
        }
    }
    return 1;   /* menu is modal: consume all input while open */

activate:
    m->status[0] = '\0';
    switch (m->screen) {
        case SCR_MAIN:   activate_main(m);   break;
        case SCR_BROWSE: activate_browse(m); break;
        case SCR_CONTROLS:
            m->capturing = 1; m->capture_btn = m->sel; break;
        case SCR_JOYSTICK:
            m->capturing = 1; m->capture_btn = m->sel; break;
    }
    return 1;

back:
    if (m->screen == SCR_MAIN) {
        if (m->has_cart) menu_close(m);    /* esc on main = resume (if a cart runs) */
    } else {
        enter_main(m);
    }
    return 1;
}

/* ---- rendering --------------------------------------------------------- */

static const SDL_Color COL_DIM   = {  0,   0,   0, 200 };
static const SDL_Color COL_PANEL = { 18,  20,  34, 255 };
static const SDL_Color COL_BAR   = { 52,  84, 160, 255 };
static const SDL_Color COL_TITLE = {255, 220, 120, 255 };
static const SDL_Color COL_TEXT  = {220, 224, 235, 255 };
static const SDL_Color COL_HILITE= {255, 255, 255, 255 };
static const SDL_Color COL_HINT  = {140, 148, 170, 255 };

static void draw_list(menu_t* m, const char* title, int x, int y, int w,
                      int n, const char* (*label)(menu_t*, int, char*, size_t)) {
    draw_text(m, x, y, COL_TITLE, title);
    y += ROW_H + 6;

    int visible = (m->win_h - y - 40) / ROW_H;
    if (visible < 1) visible = 1;
    if (m->sel < m->scroll)            m->scroll = m->sel;
    if (m->sel >= m->scroll + visible) m->scroll = m->sel - visible + 1;

    for (int i = 0; i < visible && m->scroll + i < n; ++i) {
        int idx = m->scroll + i;
        int ry = y + i * ROW_H;
        if (idx == m->sel) {
            fill(m, x - 6, ry - 2, w + 12, ROW_H, COL_BAR);
        }
        char buf[512];
        const char* s = label(m, idx, buf, sizeof(buf));
        draw_text(m, x, ry, idx == m->sel ? COL_HILITE : COL_TEXT, s);
    }
}

static const char* lbl_main(menu_t* m, int i, char* buf, size_t cap) {
    (void)cap;
    switch (i) {
        case MAIN_LOAD:   return "Load cartridge...";
        case MAIN_RESET:  return m->has_cart ? "Reset cartridge" : "Reset cartridge (none)";
        case MAIN_CTRL:   return "Controls (keyboard)";
        case MAIN_JOY:    return "Joystick";
        case MAIN_RESUME: return m->has_cart ? "Resume" : "Resume (no cartridge)";
        case MAIN_QUIT:   return "Quit";
    }
    (void)buf; return "";
}

static const char* lbl_browse(menu_t* m, int i, char* buf, size_t cap) {
    entry_t* e = &m->entries[i];
    if (e->is_dir)        snprintf(buf, cap, "[%s]", e->name);
    else if (e->title[0]) snprintf(buf, cap, "%s  -  %s", e->title, e->name);
    else           snprintf(buf, cap, "%s", e->name);
    return buf;
}

static const char* lbl_controls(menu_t* m, int i, char* buf, size_t cap) {
    SDL_Scancode sc = m->cfg->key[i];
    const char* kn = (sc == SDL_SCANCODE_UNKNOWN) ? "(none)" : SDL_GetScancodeName(sc);
    if (m->capturing && m->capture_btn == i)
        snprintf(buf, cap, "%-6s : < press a key >", hostcfg_pad_name(i));
    else
        snprintf(buf, cap, "%-6s : %s", hostcfg_pad_name(i), kn && *kn ? kn : "(none)");
    return buf;
}

static const char* lbl_joystick(menu_t* m, int i, char* buf, size_t cap) {
    SDL_GameControllerButton b = m->cfg->gbtn[i];
    const char* bn = (b == SDL_CONTROLLER_BUTTON_INVALID)
                       ? "(none)" : SDL_GameControllerGetStringForButton(b);
    if (m->capturing && m->capture_btn == i)
        snprintf(buf, cap, "%-6s : < press a button >", hostcfg_pad_name(i));
    else
        snprintf(buf, cap, "%-6s : %s", hostcfg_pad_name(i), bn && *bn ? bn : "(none)");
    return buf;
}

void menu_render(menu_t* m) {
    if (!m->open) return;

    SDL_SetRenderDrawBlendMode(m->ren, SDL_BLENDMODE_BLEND);
    fill(m, 0, 0, m->win_w, m->win_h, COL_DIM);

    int px = 40, py = 36, pw = m->win_w - 80, ph = m->win_h - 72;
    fill(m, px, py, pw, ph, COL_PANEL);

    int x = px + 28, y = py + 24, w = pw - 56;

    switch (m->screen) {
        case SCR_MAIN:
            draw_list(m, "CRONOPIO", x, y, w, MAIN_COUNT, lbl_main);
            break;
        case SCR_BROWSE: {
            char title[1100];
            snprintf(title, sizeof(title), "LOAD: %s", m->cur_dir);
            draw_list(m, title, x, y, w, m->n_entries, lbl_browse);
            /* Refresh the focused cart's metadata (author/controls) for the
             * footer; cached so we read the file only when the selection moves. */
            if (m->sel > 0 && m->sel < m->n_entries && !m->entries[m->sel].is_dir) {
                if (m->meta_sel != m->sel) {
                    char full[1300], sp[1320];
                    path_join(full, sizeof full, m->cur_dir, m->entries[m->sel].name);
                    cart_read_meta(full, &m->meta);
                    snprintf(sp, sizeof sp, "%s.sav", full);
                    m->save_size = file_size(sp);
                    m->meta_sel = m->sel;
                }
            } else {
                m->meta_sel = -1;
                m->meta.author[0] = m->meta.controls[0] = '\0';
                m->save_size = -1;
            }
            break;
        }
        case SCR_CONTROLS:
            draw_list(m, "CONTROLS - KEYBOARD", x, y, w, PAD_BTN_COUNT, lbl_controls);
            break;
        case SCR_JOYSTICK: {
            char title[300];
            snprintf(title, sizeof(title), "JOYSTICK: %s",
                     m->joy_name ? m->joy_name : "(none connected)");
            draw_list(m, title, x, y, w, PAD_BTN_COUNT, lbl_joystick);
            break;
        }
    }

    /* footer: focused-cart detail (browse) + status line + key hints */
    int fy = py + ph - CH - 14;
    if (m->screen == SCR_BROWSE) {
        if (m->save_size >= 0) {
            char sv[96];
            snprintf(sv, sizeof sv, "Save: %ld KB   (Del erases)",
                     (m->save_size + 1023) / 1024);
            draw_text(m, x, fy - ROW_H * 4, COL_TITLE, sv);
        }
        if (m->meta.author[0]) {
            char ab[128];
            snprintf(ab, sizeof ab, "by %s", m->meta.author);
            draw_text(m, x, fy - ROW_H * 3, COL_HINT, ab);
        }
        if (m->meta.controls[0])
            draw_text(m, x, fy - ROW_H * 2, COL_TEXT, m->meta.controls);
    }
    if (m->status[0]) draw_text(m, x, fy - ROW_H, COL_TITLE, m->status);
    const char* hint =
        (m->screen == SCR_MAIN) ? "Up/Down move   Enter select   Esc resume"
      : (m->screen == SCR_BROWSE) ? "Enter open/load   Del erase save   Esc back"
      : "Enter rebind   Esc back";
    draw_text(m, x, fy, COL_HINT, hint);
}
