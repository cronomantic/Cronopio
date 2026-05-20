#ifndef CRONOPIO_SYSCALLS_H
#define CRONOPIO_SYSCALLS_H

#include <stdint.h>
#include "console.h"

struct cvm_image;

/* Register every Cronopio host handler with the loaded CronoVM image.
 *
 * Imports are looked up by string name (CronoVM convention: the translator
 * adds any `cvm_sys_*` symbol the binary references to its IMPORTS section).
 * The console pointer is captured so handlers can read/write framebuffer,
 * audio, input and exit state.
 *
 * Returns 0 if every Cronopio syscall the cart references was resolved.
 * Returns a non-zero count of unresolved imports otherwise — the host can
 * still cvm_run, but any SYSCALL into an unresolved entry will trap with
 * CVM_E_UNLINKED_SYSCALL. */
int cronopio_syscalls_install(struct cvm_image* img, cronopio_console_t* c);

/* Resolve the framebuffer and palette host regions for `img`, populate
 * c->fb_offset / c->pal_offset / c->regions_ok, and seed the palette with
 * the default 32-colour table. Returns 0 on success, non-zero if the cart
 * didn't declare both regions — in that case the host runs the cart
 * anyway, but draws nothing (regions_ok stays 0). */
int cronopio_resolve_video_regions(struct cvm_image* img, cronopio_console_t* c);

#endif
