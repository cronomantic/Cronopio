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
#ifndef CRONOPIO_TEMPLATES_DIR
#  define CRONOPIO_TEMPLATES_DIR "."
#endif

#if defined(_WIN32)
#  define ISATTY(fd) _isatty(fd)
#  define FILENO(f)  _fileno(f)
#else
#  define ISATTY(fd) isatty(fd)
#  define FILENO(f)  fileno(f)
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
        "  cronopio-cc new <name> [--template=T]             scaffold a cart project\n"
        "  cronopio-cc new --list                            list templates\n"
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

/* ---- scaffold (`cronopio-cc new <name> [--template=T]`) ---------------- */

/* The curated template set. Each is a directory under the templates root
 * holding (at least) main.c; CMakeLists.txt / README.md / .gitignore are
 * taken from the template, or fall back to the _common directory. */
static const struct { const char *name, *desc; } TEMPLATES[] = {
    { "basic",   "moving box + text (start here)" },
    { "sprites", "an 8x8 sprite moved with the d-pad (image banks, blt)" },
    { "3d",      "a spinning textured cube (the 3D pipeline)" },
};
static const int N_TEMPLATES = (int)(sizeof TEMPLATES / sizeof TEMPLATES[0]);

static int is_known_template(const char *t) {
    for (int i = 0; i < N_TEMPLATES; ++i)
        if (strcmp(t, TEMPLATES[i].name) == 0) return 1;
    return 0;
}

static void list_templates(FILE *f) {
    fprintf(f, "Available templates:\n");
    for (int i = 0; i < N_TEMPLATES; ++i)
        fprintf(f, "  %-9s %s\n", TEMPLATES[i].name, TEMPLATES[i].desc);
}

/* Resolve the templates root: --templates > install layout > CMake bake. */
static char *find_templates_dir(const char *override, const char *argv0) {
    if (override) return strdup(override);
    const char *slash = exe_dir_end(argv0);
    if (slash) {
        size_t dirlen = (size_t)(slash - argv0);
        char probe[1024];
        int n = snprintf(probe, sizeof probe,
                         "%.*s%c..%cshare%ccronopio%ctemplates%cbasic%cmain.c",
                         (int)dirlen, argv0, PATH_SEP, PATH_SEP, PATH_SEP,
                         PATH_SEP, PATH_SEP, PATH_SEP);
        if (n > 0 && n < (int)sizeof probe && file_exists(probe)) {
            char *dir = (char *)malloc((size_t)dirlen + 64);
            if (dir) {
                snprintf(dir, dirlen + 64, "%.*s%c..%cshare%ccronopio%ctemplates",
                         (int)dirlen, argv0, PATH_SEP, PATH_SEP, PATH_SEP, PATH_SEP);
                return dir;
            }
        }
    }
    return strdup(CRONOPIO_TEMPLATES_DIR);
}

/* Copy src -> dst, replacing every "@NAME@" with `name`. */
static int copy_substituted(const char *src, const char *dst, const char *name) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;                 /* not found is not fatal: caller decides */
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); fprintf(stderr, "cronopio-cc: cannot write %s\n", dst); return 1; }
    int c;
    const char *tok = "@NAME@";
    int matched = 0;                    /* chars of tok matched so far */
    while ((c = fgetc(in)) != EOF) {
        if (c == tok[matched]) {
            if (tok[++matched] == '\0') { fputs(name, out); matched = 0; }
        } else {
            if (matched) { fwrite(tok, 1, (size_t)matched, out); matched = 0; }
            /* the failed char might itself start a match */
            if (c == tok[0]) matched = 1; else fputc(c, out);
        }
    }
    if (matched) fwrite(tok, 1, (size_t)matched, out);
    fclose(in); fclose(out);
    return 0;
}

/* Copy one project file from <templates>/<tpl>/<file>, falling back to
 * <templates>/_common/<file>. Missing in both is silently skipped. */
static int emit_file(const char *templates, const char *tpl,
                     const char *dstdir, const char *file, const char *name) {
    char src[1024], dst[1024];
    snprintf(dst, sizeof dst, "%s%c%s", dstdir, PATH_SEP, file);
    if (file_exists(dst)) { fprintf(stderr, "cronopio-cc: refusing to overwrite %s\n", dst); return 1; }

    snprintf(src, sizeof src, "%s%c%s%c%s", templates, PATH_SEP, tpl, PATH_SEP, file);
    int rc = copy_substituted(src, dst, name);
    if (rc == -1) {                     /* not in the template — try _common */
        snprintf(src, sizeof src, "%s%c_common%c%s", templates, PATH_SEP, PATH_SEP, file);
        rc = copy_substituted(src, dst, name);
        if (rc == -1) return 0;         /* in neither: nothing to emit */
    }
    if (rc == 0) printf("  %s\n", dst);
    return rc;
}

static int scaffold(int argc, char **argv) {
    const char *name = NULL, *tpl = NULL, *templates_override = NULL;
    int do_list = 0;
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--list") == 0)             do_list = 1;
        else if (strncmp(a, "--template=", 11) == 0) tpl = a + 11;
        else if (strcmp(a, "-t") == 0 && i + 1 < argc) tpl = argv[++i];
        else if (strncmp(a, "--templates=", 12) == 0) templates_override = a + 12;
        else if (a[0] == '-') { fprintf(stderr, "cronopio-cc: new: unknown option '%s'\n", a); return 2; }
        else if (!name) name = a;
        else { fprintf(stderr, "cronopio-cc: new: unexpected argument '%s'\n", a); return 2; }
    }

    if (do_list) { list_templates(stdout); return 0; }
    if (!name || !*name) {
        fprintf(stderr, "cronopio-cc: new: missing project name\n"
                        "usage: cronopio-cc new <name> [--template=basic|sprites|3d]\n");
        return 2;
    }

    /* Pick a template: explicit flag, else interactive prompt on a TTY, else
     * 'basic'. */
    char chosen[32];
    if (tpl) {
        if (!is_known_template(tpl)) {
            fprintf(stderr, "cronopio-cc: unknown template '%s'\n", tpl);
            list_templates(stderr);
            return 2;
        }
        snprintf(chosen, sizeof chosen, "%s", tpl);
    } else if (ISATTY(FILENO(stdin)) && ISATTY(FILENO(stdout))) {
        list_templates(stdout);
        printf("Template [basic]: ");
        fflush(stdout);
        char line[64];
        if (fgets(line, sizeof line, stdin)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = 0;
            if (n == 0) snprintf(chosen, sizeof chosen, "basic");
            else if (is_known_template(line)) snprintf(chosen, sizeof chosen, "%s", line);
            else { fprintf(stderr, "cronopio-cc: unknown template '%s'\n", line); return 2; }
        } else {
            snprintf(chosen, sizeof chosen, "basic");
        }
    } else {
        snprintf(chosen, sizeof chosen, "basic");
    }

    char *templates = find_templates_dir(templates_override, argv[0]);

    if (MKDIR(name) != 0 && errno != EEXIST) {
        fprintf(stderr, "cronopio-cc: cannot create %s: %s\n", name, strerror(errno));
        free(templates);
        return 1;
    }

    printf("Scaffolding cart '%s' from template '%s':\n", name, chosen);
    static const char *files[] = { "main.c", "CMakeLists.txt", "README.md", ".gitignore" };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof files / sizeof files[0]); ++i)
        rc |= emit_file(templates, chosen, name, files[i], name);
    free(templates);

    if (rc == 0)
        printf("Done. Next: cd %s && cronopio-cc main.c -o %s.bin\n", name, name);
    return rc;
}

/* ---- compile ----------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "new") == 0)
        return scaffold(argc, argv);
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
