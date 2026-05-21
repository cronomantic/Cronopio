/* cronopio-cc — one-line cartridge compiler.
 *
 * cvm-cc is generic: every cart has to spell out the Cronopio memory map
 * (--region=fb:... --region=pal:...) and the default reserves. This wrapper
 * bakes those in and points the include path at the Cronopio SDK, so a cart
 * is built with just:
 *
 *     cronopio-cc game.c -o game.bin
 *
 * Everything else is forwarded to cvm-cc unchanged (-O, -I, --rom, -v, ...).
 * --heap-reserve / --stack-reserve override the defaults if you pass them.
 *
 * It also scaffolds a new cart project:
 *
 *     cronopio-cc new mygame        # writes mygame/{main.c,CMakeLists.txt,README.md}
 *
 * Discovery (same philosophy as cvm-cc — build-tree default baked by CMake,
 * install layout probed relative to the exe, both overridable):
 *   - cvm-cc:     --cvm-cc=PATH > sibling of this exe > CRONOPIO_CVM_CC_DEFAULT > PATH
 *   - SDK headers: --sdk=DIR    > <exedir>/../include (install) > CRONOPIO_SDK_INCLUDE
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <process.h>
#  include <direct.h>
#  define PATH_SEP '\\'
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define PATH_SEP '/'
#  define MKDIR(p) mkdir((p), 0755)
#endif

#ifdef CRONOPIO_CC_HAVE_PATHS_H
#  include "cronopio_cc_paths.h"   /* CMake-generated: CRONOPIO_CVM_CC_DEFAULT */
#endif

#ifndef CRONOPIO_CVM_CC_DEFAULT
#  define CRONOPIO_CVM_CC_DEFAULT "cvm-cc"
#endif
#ifndef CRONOPIO_SDK_INCLUDE
#  define CRONOPIO_SDK_INCLUDE "."
#endif

/* Cronopio machine defaults — must match sdk/cmake/CronopioCart.cmake. */
#define CRON_REGION_FB   "--region=fb:76800:rw"
#define CRON_REGION_PAL  "--region=pal:1024:rw"
#define CRON_HEAP_DEF    "--heap-reserve=32M"
#define CRON_STACK_DEF   "--stack-reserve=256K"

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *exe_dir_end(const char *argv0) {
    const char *slash = strrchr(argv0, '/');
#if defined(_WIN32)
    const char *back = strrchr(argv0, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash;
}

static char *sibling_of(const char *argv0, const char *name) {
    const char *slash = exe_dir_end(argv0);
    if (!slash) return NULL;
    size_t dirlen = (size_t)(slash - argv0);
    size_t namlen = strlen(name);
    char  *p = (char *)malloc(dirlen + 1 + namlen + 1);
    if (!p) return NULL;
    memcpy(p, argv0, dirlen);
    p[dirlen] = PATH_SEP;
    memcpy(p + dirlen + 1, name, namlen + 1);
    return p;
}

/* Install layout puts the SDK headers at <exedir>/../include. Probe for
 * cronopio.h there; on hit return that dir (caller frees), else NULL. */
static char *find_install_sdk(const char *argv0) {
    const char *slash = exe_dir_end(argv0);
    if (!slash) return NULL;
    size_t dirlen = (size_t)(slash - argv0);
    char probe[1024];
    int n = snprintf(probe, sizeof probe, "%.*s%c..%cinclude%ccronopio.h",
                     (int)dirlen, argv0, PATH_SEP, PATH_SEP, PATH_SEP);
    if (n < 0 || n >= (int)sizeof probe) return NULL;
    if (!file_exists(probe)) return NULL;
    char *dir = (char *)malloc((size_t)n + 1);
    if (!dir) return NULL;
    snprintf(dir, (size_t)n + 1, "%.*s%c..%cinclude",
             (int)dirlen, argv0, PATH_SEP, PATH_SEP);
    return dir;
}

static char *find_cvm_cc(const char *override, const char *argv0) {
    if (override) return strdup(override);
#if defined(_WIN32)
    const char *exe = "cvm-cc.exe";
#else
    const char *exe = "cvm-cc";
#endif
    char *sib = sibling_of(argv0, exe);
    if (sib && file_exists(sib)) return sib;
    free(sib);
    if (file_exists(CRONOPIO_CVM_CC_DEFAULT)) return strdup(CRONOPIO_CVM_CC_DEFAULT);
    return strdup(exe);
}

static int run_cmd(int verbose, char **argv) {
    if (verbose) {
        fprintf(stderr, "cronopio-cc:");
        for (int i = 0; argv[i]; ++i) fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }
#if defined(_WIN32)
    intptr_t r = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    if (r == -1) {
        fprintf(stderr, "cronopio-cc: failed to spawn '%s': %s\n", argv[0], strerror(errno));
        return -1;
    }
    return (int)r;
#else
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "cronopio-cc: failed to exec '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) { perror("waitpid"); return -1; }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

static void usage(FILE *f) {
    fprintf(f,
        "Usage:\n"
        "  cronopio-cc <input.c> -o <output.bin> [options]   compile a cartridge\n"
        "  cronopio-cc new <name>                            scaffold a cart project\n"
        "\n"
        "Compile bakes in the Cronopio memory map and SDK include path, then\n"
        "forwards everything else to cvm-cc:\n"
        "  -o <file>                output .bin (required)\n"
        "  -O0|-O1|-O2|-O3|-Os      optimisation level\n"
        "  -I <dir>                 extra include dir (repeatable)\n"
        "  --heap-reserve=N[K|M]    override default 32M\n"
        "  --stack-reserve=N[K|M]   override default 256K\n"
        "  --rom=FILE               bake FILE as read-only cartridge ROM\n"
        "  -v, --verbose            print every command\n"
        "\n"
        "Discovery overrides:\n"
        "  --cvm-cc=PATH            cvm-cc binary\n"
        "  --sdk=DIR                SDK include dir (holds cronopio.h)\n");
}

/* ---- scaffold (`cronopio-cc new <name>`) ------------------------------- */

static int write_file(const char *dir, const char *name, const char *body) {
    char path[1024];
    snprintf(path, sizeof path, "%s%c%s", dir, PATH_SEP, name);
    if (file_exists(path)) {
        fprintf(stderr, "cronopio-cc: refusing to overwrite %s\n", path);
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cronopio-cc: cannot write %s: %s\n", path, strerror(errno)); return 1; }
    fputs(body, f);
    fclose(f);
    printf("  %s\n", path);
    return 0;
}

static int scaffold(const char *name) {
    if (!name || !*name) { fprintf(stderr, "cronopio-cc: new: missing project name\n"); return 2; }
    if (MKDIR(name) != 0 && errno != EEXIST) {
        fprintf(stderr, "cronopio-cc: cannot create %s: %s\n", name, strerror(errno));
        return 1;
    }

    char main_c[2048];
    snprintf(main_c, sizeof main_c,
        "/* %s — a Cronopio cartridge. Build: cmake -B build && cmake --build build\n"
        " * or directly: cronopio-cc main.c -o %s.bin */\n"
        "#include <cronopio.h>\n"
        "\n"
        "static int32_t t;\n"
        "\n"
        "void setup(void) {\n"
        "    cron_log(\"%s booting\\n\", %d);\n"
        "}\n"
        "\n"
        "void frame(void) {\n"
        "    cron_cls(1);                       /* clear to colour 1 */\n"
        "    int32_t x = (t * 2) %% (CRON_SCREEN_W - 32);\n"
        "    cron_rect(x, 100, 32, 32, 7);      /* a moving box */\n"
        "    static const char hi[] = \"%s\";\n"
        "    cron_text(hi, (int32_t)sizeof(hi) - 1, 8, 8, 15);\n"
        "    if (cron_pad(0) & CRON_BTN_A) cron_cls(8);\n"
        "    t++;\n"
        "}\n"
        "\n"
        "CRONOPIO_CART_INIT(setup, frame)\n",
        name, name, name, (int)strlen(name) + 9, name);

    char cml[1024];
    snprintf(cml, sizeof cml,
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(%s C)\n"
        "\n"
        "# Needs an installed Cronopio SDK on CMAKE_PREFIX_PATH, or pass\n"
        "#   -DCMAKE_PREFIX_PATH=<cronopio-install-prefix>\n"
        "find_package(Cronopio REQUIRED)\n"
        "\n"
        "cronopio_add_cartridge(%s SOURCES main.c)\n",
        name, name);

    char readme[1024];
    snprintf(readme, sizeof readme,
        "# %s\n\n"
        "A Cronopio cartridge.\n\n"
        "## Build\n\n"
        "```sh\n"
        "cmake -B build -DCMAKE_PREFIX_PATH=<cronopio-install-prefix>\n"
        "cmake --build build\n"
        "```\n\n"
        "Produces `build/%s.bin`. Or compile directly without CMake:\n\n"
        "```sh\n"
        "cronopio-cc main.c -o %s.bin\n"
        "```\n\n"
        "## Run\n\n"
        "```sh\n"
        "cronopio %s.bin\n"
        "```\n",
        name, name, name, name);

    printf("Scaffolding cart '%s':\n", name);
    int rc = 0;
    rc |= write_file(name, "main.c", main_c);
    rc |= write_file(name, "CMakeLists.txt", cml);
    rc |= write_file(name, "README.md", readme);
    if (rc == 0)
        printf("Done. Next: cd %s && cronopio-cc main.c -o %s.bin\n", name, name);
    return rc;
}

/* ---- compile ----------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "new") == 0)
        return scaffold(argc >= 3 ? argv[2] : NULL);
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }

    const char *cvm_cc_override = NULL;
    const char *sdk_override    = NULL;
    int have_heap = 0, have_stack = 0, verbose = 0;

    /* Collect pass-through args; intercept only the few we care about. */
    char *fwd[256];
    int   nf = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--cvm-cc=", 9) == 0)      { cvm_cc_override = a + 9; continue; }
        if (strncmp(a, "--sdk=", 6) == 0)         { sdk_override = a + 6; continue; }
        if (strncmp(a, "--heap-reserve=", 15) == 0)  have_heap = 1;
        if (strncmp(a, "--stack-reserve=", 16) == 0) have_stack = 1;
        if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) verbose = 1;
        if (nf >= 240) { fprintf(stderr, "cronopio-cc: too many arguments\n"); return 2; }
        fwd[nf++] = (char *)a;
    }

    char *cvm_cc = find_cvm_cc(cvm_cc_override, argv[0]);
    char *sdk    = sdk_override ? strdup(sdk_override) : find_install_sdk(argv[0]);
    if (!sdk) sdk = strdup(CRONOPIO_SDK_INCLUDE);

    /* argv for cvm-cc: <cvm-cc> <baked defaults> <forwarded user args>.
     * cvm-cc takes the include dir as a separate token (-I <dir>). */
    char *cargv[256 + 16];
    int   n = 0;
    cargv[n++] = cvm_cc;
    cargv[n++] = (char *)CRON_REGION_FB;
    cargv[n++] = (char *)CRON_REGION_PAL;
    if (!have_heap)  cargv[n++] = (char *)CRON_HEAP_DEF;
    if (!have_stack) cargv[n++] = (char *)CRON_STACK_DEF;
    cargv[n++] = (char *)"-I";
    cargv[n++] = sdk;
    for (int i = 0; i < nf; ++i) cargv[n++] = fwd[i];
    cargv[n] = NULL;

    int rc = run_cmd(verbose, cargv);
    free(cvm_cc); free(sdk);
    if (rc != 0) {
        fprintf(stderr, "cronopio-cc: cvm-cc failed (exit %d)\n", rc);
        return rc < 0 ? 1 : rc;
    }
    return 0;
}
