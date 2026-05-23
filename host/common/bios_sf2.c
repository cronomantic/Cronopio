/* The default General MIDI "BIOS" SoundFont (GeneralUser GS v2.0.3), embedded
 * straight into the binary with C23 #embed so the Cronopio executable is
 * self-contained — no external asset file to ship or lose. Loaded into synth
 * slot 0 at console init (see console.c / midisynth.c).
 *
 * License: bios/GeneralUser-GS-LICENSE.txt (free use, including in software
 * projects). This translation unit must be compiled as C23 (-std=gnu23); the
 * CMakeLists sets that for this file only. */

#include <stddef.h>

const unsigned char cron_bios_sf2[] = {
#embed "bios/GeneralUser-GS.sf2"
};
const size_t cron_bios_sf2_len = sizeof(cron_bios_sf2);
