/* host_cfg_t defaults + a tiny `key=value` reader/writer. See hostcfg.h. */

#include "hostcfg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* const pad_names[PAD_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
    "L", "R", "Start", "Select"
};

const char* hostcfg_pad_name(int btn) {
    if (btn < 0 || btn >= PAD_BTN_COUNT) return "?";
    return pad_names[btn];
}

void hostcfg_defaults(host_cfg_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->key[PAD_UP]    = SDL_SCANCODE_UP;
    cfg->key[PAD_DOWN]  = SDL_SCANCODE_DOWN;
    cfg->key[PAD_LEFT]  = SDL_SCANCODE_LEFT;
    cfg->key[PAD_RIGHT] = SDL_SCANCODE_RIGHT;
    cfg->key[PAD_A]     = SDL_SCANCODE_Z;
    cfg->key[PAD_B]     = SDL_SCANCODE_X;
    cfg->key[PAD_X]     = SDL_SCANCODE_C;
    cfg->key[PAD_Y]     = SDL_SCANCODE_V;
    cfg->key[PAD_L]     = SDL_SCANCODE_A;
    cfg->key[PAD_R]     = SDL_SCANCODE_S;
    cfg->key[PAD_START] = SDL_SCANCODE_RETURN;
    cfg->key[PAD_SELECT]= SDL_SCANCODE_RSHIFT;

    cfg->gbtn[PAD_UP]    = SDL_CONTROLLER_BUTTON_DPAD_UP;
    cfg->gbtn[PAD_DOWN]  = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    cfg->gbtn[PAD_LEFT]  = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    cfg->gbtn[PAD_RIGHT] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    cfg->gbtn[PAD_A]     = SDL_CONTROLLER_BUTTON_A;
    cfg->gbtn[PAD_B]     = SDL_CONTROLLER_BUTTON_B;
    cfg->gbtn[PAD_X]     = SDL_CONTROLLER_BUTTON_X;
    cfg->gbtn[PAD_Y]     = SDL_CONTROLLER_BUTTON_Y;
    cfg->gbtn[PAD_L]     = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    cfg->gbtn[PAD_R]     = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    cfg->gbtn[PAD_START] = SDL_CONTROLLER_BUTTON_START;
    cfg->gbtn[PAD_SELECT]= SDL_CONTROLLER_BUTTON_BACK;

    cfg->joy_guid[0] = '\0';
    cfg->last_dir[0] = '\0';

    cfg->scale      = 3;   /* 960x720 window */
    cfg->fullscreen = 0;
    cfg->vsync      = 1;
    cfg->save_dir[0] = '\0';   /* beside the cartridge */
}

/* Strip a trailing CR/LF and any leading whitespace; returns the start. */
static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
    return s;
}

int hostcfg_load(host_cfg_t* cfg, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);
        if (!*p || *p == '#') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(p);
        char* val = trim(eq + 1);

        /* key.<button> / gbtn.<button> = integer; one line per pad button. */
        for (int b = 0; b < PAD_BTN_COUNT; ++b) {
            char kk[32], gk[32];
            snprintf(kk, sizeof(kk), "key.%s", pad_names[b]);
            snprintf(gk, sizeof(gk), "gbtn.%s", pad_names[b]);
            if (!strcmp(key, kk)) cfg->key[b]  = (SDL_Scancode)atoi(val);
            if (!strcmp(key, gk)) cfg->gbtn[b] = (SDL_GameControllerButton)atoi(val);
        }
        if (!strcmp(key, "joy_guid"))
            snprintf(cfg->joy_guid, sizeof(cfg->joy_guid), "%s", val);
        if (!strcmp(key, "last_dir"))
            snprintf(cfg->last_dir, sizeof(cfg->last_dir), "%s", val);
        if (!strcmp(key, "scale"))      cfg->scale      = atoi(val);
        if (!strcmp(key, "fullscreen")) cfg->fullscreen = atoi(val) != 0;
        if (!strcmp(key, "vsync"))      cfg->vsync      = atoi(val) != 0;
        if (!strcmp(key, "save_dir"))
            snprintf(cfg->save_dir, sizeof(cfg->save_dir), "%s", val);
    }

    if (cfg->scale < HOSTCFG_SCALE_MIN) cfg->scale = HOSTCFG_SCALE_MIN;
    if (cfg->scale > HOSTCFG_SCALE_MAX) cfg->scale = HOSTCFG_SCALE_MAX;
    fclose(f);
    return 0;
}

int hostcfg_save(const host_cfg_t* cfg, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "# Cronopio desktop host configuration\n");
    fprintf(f, "# Pad bindings: SDL scancodes (key.*) and controller buttons (gbtn.*).\n");
    for (int b = 0; b < PAD_BTN_COUNT; ++b)
        fprintf(f, "key.%s=%d\n", pad_names[b], (int)cfg->key[b]);
    for (int b = 0; b < PAD_BTN_COUNT; ++b)
        fprintf(f, "gbtn.%s=%d\n", pad_names[b], (int)cfg->gbtn[b]);
    fprintf(f, "joy_guid=%s\n", cfg->joy_guid);
    fprintf(f, "last_dir=%s\n", cfg->last_dir);
    fprintf(f, "# Video: scale 1..6 (window = scale*320 x scale*240), fullscreen/vsync 0|1.\n");
    fprintf(f, "scale=%d\n", cfg->scale);
    fprintf(f, "fullscreen=%d\n", cfg->fullscreen);
    fprintf(f, "vsync=%d\n", cfg->vsync);
    fprintf(f, "# Save folder: \"\"=beside the cartridge; relative=under the host dir; absolute=verbatim.\n");
    fprintf(f, "save_dir=%s\n", cfg->save_dir);

    fclose(f);
    return 0;
}
