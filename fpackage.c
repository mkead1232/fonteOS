/*
 * fpackage v0.5 — fonteOS package manager
 *
 * Commands:
 *   install <pkg>   build and install a package
 *   remove  <pkg>   remove an installed package (uses file manifest)
 *   list            list installed packages with versions
 *   search  <query> search local recipes AND remote index
 *   info    <pkg>   show recipe / index info
 *   sync            fetch/update the remote package index
 *   update  <pkg>   reinstall if a newer version is available
 *
 * Remote index format  (one package per line, hosted on your server):
 *   name|version|url|sha256|description
 *   e.g.  nano|8.3|https://example.com/nano-8.3.tar.gz|abc123...|Text editor
 *
 * FONTE recipe fields used:
 *   NAME=        package name
 *   VERSION=     version string
 *   URL=         source tarball URL
 *   SHA256=      expected sha256 of the tarball (optional but recommended)
 *   DEPS="a b c" space-separated dependency list (optional)
 *   build()      shell function called to configure+compile+install
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

/* ── tunables ────────────────────────────────────────────────── */
#define RECIPES_DIR   "/var/fpackage/recipes"
#define INSTALLED_DIR "/var/fpackage/installed"
#define CACHE_DIR     "/var/fpackage/cache"
#define INDEX_FILE    "/var/fpackage/index"
/* Change REMOTE_INDEX to wherever you host your package index file */
#define REMOTE_INDEX  "http://raw.githubusercontent.com/mkead1232/fonteOS/main/index"

#define VERSION       "0.5"
#define MAX_PKG       128
#define MAX_PATH      512
#define MAX_LINE      1024
#define MAX_CORES     16

/* ── safe path builder ───────────────────────────────────────── */
static void xpath(char *buf, size_t sz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sz) {
        fprintf(stderr, "fpackage: internal path overflow\n");
        exit(1);
    }
}

/* ── terminal width ──────────────────────────────────────────── */
static int term_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

static void progress(const char *label, int pct) {
    int bar = term_width() - 22;
    if (bar < 10) bar = 10;
    int fill = (pct * bar) / 100;
    printf("\r  %-10s [", label);
    for (int i = 0; i < bar; i++) putchar(i < fill ? '#' : '-');
    printf("] %3d%%", pct);
    fflush(stdout);
    if (pct >= 100) putchar('\n');
}

/* ── FONTE field reader ──────────────────────────────────────── */
/* Reads FIELD=value from a FONTE file into out[outsz].
   Strips surrounding double-quotes and trailing newline.
   Returns 1 on success, 0 if field not found.                   */
static int fonte_field(const char *fonte_path, const char *field,
                       char *out, size_t outsz) {
    FILE *f = fopen(fonte_path, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    size_t flen = strlen(field);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, field, flen) == 0 && line[flen] == '=') {
            char *val = line + flen + 1;
            if (*val == '"') val++;           /* strip leading quote  */
            val[strcspn(val, "\"\n")] = '\0'; /* strip trailing quote/newline */
            snprintf(out, outsz, "%s", val);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ── installed-package helpers ───────────────────────────────── */

/* Layout under INSTALLED_DIR/<pkg>/:
 *   info   — VERSION=x.y.z
 *   files  — one installed absolute file path per line (manifest) */

static int is_installed(const char *pkg) {
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/info", INSTALLED_DIR, pkg);
    struct stat st;
    return stat(p, &st) == 0;
}

static void installed_version(const char *pkg, char *buf, size_t sz) {
    buf[0] = '\0';
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/info", INSTALLED_DIR, pkg);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VERSION=", 8) == 0) {
            line[strcspn(line, "\n")] = '\0';
            snprintf(buf, sz, "%s", line + 8);
            break;
        }
    }
    fclose(f);
}

static void mark_installed(const char *pkg, const char *ver) {
    char dir[MAX_PATH];
    xpath(dir, sizeof(dir), "%s/%s", INSTALLED_DIR, pkg);
    mkdir(dir, 0755);
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/info", dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "VERSION=%s\n", ver[0] ? ver : "unknown"); fclose(f); }
}

static void mark_removed(const char *pkg) {
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/info", INSTALLED_DIR, pkg);
    remove(p);
    xpath(p, sizeof(p), "%s/%s", INSTALLED_DIR, pkg);
    rmdir(p); /* silently fails if dir still has files */
}

/* ── file manifest ───────────────────────────────────────────── */

static FILE *manifest_open(const char *pkg) {
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/files", INSTALLED_DIR, pkg);
    return fopen(p, "w");
}

static void manifest_add(FILE *mf, const char *path) {
    if (mf) fprintf(mf, "%s\n", path);
}

/* Delete every file listed in the manifest, then the manifest itself */
static void manifest_remove(const char *pkg) {
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/files", INSTALLED_DIR, pkg);
    FILE *f = fopen(p, "r");
    if (!f) {
        printf("  warning: no file manifest for '%s' — skipping file removal\n", pkg);
        return;
    }
    char line[MAX_PATH];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] && remove(line) == 0) n++;
    }
    fclose(f);
    remove(p);
    printf("  removed %d file(s)\n", n);
}

/* ── recipe helper ───────────────────────────────────────────── */
static int recipe_exists(const char *pkg) {
    char p[MAX_PATH];
    xpath(p, sizeof(p), "%s/%s/FONTE", RECIPES_DIR, pkg);
    struct stat st;
    return stat(p, &st) == 0;
}

/* ── SHA-256 verification ────────────────────────────────────── */
static int sha256_ok(const char *file, const char *expected) {
    if (!expected || expected[0] == '\0') return 1;
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd),
             "sha256sum '%s' 2>/dev/null | cut -d' ' -f1", file);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char actual[128] = {0};
    fgets(actual, sizeof(actual), p);
    pclose(p);
    actual[strcspn(actual, "\n")] = '\0';
    if (strcmp(actual, expected) != 0) {
        printf("  error: SHA-256 mismatch\n"
               "    expected: %s\n"
               "    got:      %s\n", expected, actual);
        return 0;
    }
    printf("  checksum OK\n");
    return 1;
}

/* ── remote index ────────────────────────────────────────────── */
typedef struct {
    char name[MAX_PKG];
    char version[64];
    char url[512];
    char sha256[128];
    char desc[256];
} IndexEntry;

static int parse_index_line(const char *raw, IndexEntry *e) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s", raw);
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '#' || buf[0] == '\0') return 0;
    char *f[5];
    int n = 0;
    char *p = buf;
    while (n < 5) {
        f[n++] = p;
        p = strchr(p, '|');
        if (!p) break;
        *p++ = '\0';
    }
    if (n < 5) return 0;
    snprintf(e->name,    sizeof(e->name),    "%s", f[0]);
    snprintf(e->version, sizeof(e->version), "%s", f[1]);
    snprintf(e->url,     sizeof(e->url),     "%s", f[2]);
    snprintf(e->sha256,  sizeof(e->sha256),  "%s", f[3]);
    snprintf(e->desc,    sizeof(e->desc),    "%s", f[4]);
    return 1;
}

static int index_lookup(const char *pkg, IndexEntry *out) {
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        IndexEntry e;
        if (parse_index_line(line, &e) && strcmp(e.name, pkg) == 0) {
            if (out) *out = e;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Synthesise a minimal FONTE recipe from a remote index entry */
static int synth_recipe(const IndexEntry *e) {
    char dir[MAX_PATH];
    xpath(dir, sizeof(dir), "%s/%s", RECIPES_DIR, e->name);
    mkdir(dir, 0755);
    char fonte[MAX_PATH];
    xpath(fonte, sizeof(fonte), "%s/FONTE", dir);
    FILE *f = fopen(fonte, "w");
    if (!f) { printf("  error: cannot write recipe for '%s'\n", e->name); return 0; }
    fprintf(f,
        "NAME=\"%s\"\n"
        "VERSION=\"%s\"\n"
        "URL=\"%s\"\n"
        "SHA256=\"%s\"\n"
        "\n"
        "build() {\n"
        "    ./configure --prefix=/usr 2>/dev/null || true\n"
        "    make -j${MAKEFLAGS##*-j}\n"
        "    make DESTDIR=\"$DESTDIR\" install\n"
        "}\n",
        e->name, e->version, e->url, e->sha256);
    fclose(f);
    printf("  synthesised recipe for %s %s from remote index\n",
           e->name, e->version);
    return 1;
}

/* ── repology lookup ─────────────────────────────────────────── */
/*
 * Queries https://repology.org/api/v1/project/<pkg> and prints
 * the newest version found across all repos.
 * Uses wget + basic JSON string scanning (no library needed).
 */
static void repology_search(const char *pkg) {
    char tmpfile[MAX_PATH];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/repology_%s.json", pkg);

    char cmd[MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd),
        "wget -q -T 5 -O '%s' "
        "'http://repology.org/api/v1/project/%s' 2>/dev/null",
        tmpfile, pkg);

    if (system(cmd) != 0) {
        printf("  [repology] unavailable (no network?)\n");
        return;
    }

    FILE *f = fopen(tmpfile, "r");
    if (!f) return;

    /* read entire file into a buffer for scanning */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz <= 0 || fsz > 1024*1024) { fclose(f); remove(tmpfile); return; }
    char *buf = malloc(fsz + 1);
    if (!buf) { fclose(f); remove(tmpfile); return; }
    fread(buf, 1, fsz, f);
    buf[fsz] = '\0';
    fclose(f);
    remove(tmpfile);

    /* scan for "newest" status entries and grab their version + repo */
    char best_ver[64]  = "";
    char best_repo[64] = "";
    char *p = buf;
    while ((p = strstr(p, "\"status\":\"newest\"")) != NULL) {
        /* look backwards in the same object for "version" */
        char *obj_start = p;
        while (obj_start > buf && *obj_start != '{') obj_start--;

        char *vp = strstr(obj_start, "\"version\":\"");
        char *rp = strstr(obj_start, "\"repo\":\"");
        if (vp && vp < p + 64) {
            vp += 11;
            size_t i = 0;
            char ver[64] = "";
            while (*vp && *vp != '"' && i < sizeof(ver)-1) ver[i++] = *vp++;
            ver[i] = '\0';
            char repo[64] = "";
            if (rp) {
                rp += 8;
                i = 0;
                while (*rp && *rp != '"' && i < sizeof(repo)-1) repo[i++] = *rp++;
                repo[i] = '\0';
            }
            if (ver[0] && best_ver[0] == '\0') {
                snprintf(best_ver,  sizeof(best_ver),  "%s", ver);
                snprintf(best_repo, sizeof(best_repo), "%s", repo);
            }
        }
        p++;
    }
    free(buf);

    if (best_ver[0])
        printf("  %-24s %-12s [repology/%s]\n", pkg, best_ver, best_repo);
    else
        printf("  [repology] no results for '%s'\n", pkg);
}

/* ── sync ────────────────────────────────────────────────────── */
static void cmd_sync(void) {
    printf("==> syncing package index...\n");
    mkdir("/var/fpackage", 0755);
    char cmd[MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd),
             "wget -q --no-check-certificate -O '%s' '%s'", INDEX_FILE, REMOTE_INDEX);
    if (system(cmd) == 0) {
        FILE *f = fopen(INDEX_FILE, "r");
        int count = 0;
        if (f) {
            char line[MAX_LINE];
            IndexEntry e;
            while (fgets(line, sizeof(line), f))
                if (parse_index_line(line, &e)) count++;
            fclose(f);
        }
        printf("==> index updated (%d package(s))\n\n", count);
    } else {
        printf("error: could not fetch index from:\n  %s\n\n", REMOTE_INDEX);
    }
}


/* ── repology install fallback ───────────────────────────────── */
/* Query repology for a download URL, synthesise a recipe, return 1 on success */
static int repology_synth(const char *pkg) {
    char tmpfile[MAX_PATH];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/repology_%s.json", pkg);

    char cmd[MAX_PATH + 128];
    snprintf(cmd, sizeof(cmd),
        "wget -q --no-check-certificate -T 5 -O '%s' "
        "'http://repology.org/api/v1/project/%s' 2>/dev/null",
        tmpfile, pkg);

    printf("  ==> querying repology for '%s'...\n", pkg);
    if (system(cmd) != 0) {
        printf("  error: could not reach repology\n");
        return 0;
    }

    FILE *f = fopen(tmpfile, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz <= 0 || fsz > 2*1024*1024) { fclose(f); remove(tmpfile); return 0; }
    char *buf = malloc(fsz + 1);
    if (!buf) { fclose(f); remove(tmpfile); return 0; }
    fread(buf, 1, fsz, f);
    buf[fsz] = '\0';
    fclose(f);
    remove(tmpfile);

    /* extract version from first "newest" entry */
    char ver[64] = "";
    char *sp = strstr(buf, "\"status\":\"newest\"");
    if (sp) {
        /* search backwards for version in same object */
        char *obj = sp;
        while (obj > buf && *obj != '{') obj--;
        char *vp = strstr(obj, "\"version\":\"");
        if (vp) {
            vp += 11;
            size_t i = 0;
            while (*vp && *vp != '"' && i < sizeof(ver)-1) ver[i++] = *vp++;
            ver[i] = '\0';
        }
    }

    /* extract first download URL */
    char url[512] = "";
    char *dp = strstr(buf, "\"downloads\":[\"\"");
    /* try multiple patterns for download URL */
    char *patterns[] = { "\"downloads\":[\"", "\"url\":\"http", "\"url\":\"https", NULL };
    for (int i = 0; patterns[i] && url[0] == '\0'; i++) {
        char *p = strstr(buf, patterns[i]);
        if (p) {
            /* find the http/https URL */
            char *u = strstr(p, "http");
            if (u && u < p + 256) {
                size_t j = 0;
                while (*u && *u != '"' && j < sizeof(url)-1) url[j++] = *u++;
                url[j] = '\0';
            }
        }
    }
    (void)dp;

    free(buf);

    if (ver[0] == '\0') {
        printf("  error: no version info found on repology for '%s'\n", pkg);
        return 0;
    }

    if (url[0] == '\0') {
        /* no download URL — build a best-guess GNU/freedesktop URL */
        printf("  warning: no download URL on repology, guessing tarball URL\n");
        snprintf(url, sizeof(url),
            "https://ftp.gnu.org/gnu/%s/%s-%s.tar.gz",
            pkg, pkg, ver);
    }

    printf("  ==> found: %s %s\n", pkg, ver);
    printf("  ==> url:   %s\n", url);

    /* synthesise a FONTE recipe */
    IndexEntry e;
    snprintf(e.name,    sizeof(e.name),    "%s", pkg);
    snprintf(e.version, sizeof(e.version), "%s", ver);
    snprintf(e.url,     sizeof(e.url),     "%s", url);
    e.sha256[0] = '\0';
    snprintf(e.desc,    sizeof(e.desc),    "from repology");
    return synth_recipe(&e);
}

/* ── dependency installer ────────────────────────────────────── */
/* forward declaration */
static void cmd_install(const char *pkg, int cores, int as_dep);

static void install_deps(const char *pkg, int cores) {
    char fonte[MAX_PATH];
    xpath(fonte, sizeof(fonte), "%s/%s/FONTE", RECIPES_DIR, pkg);
    char deps_raw[MAX_LINE] = {0};
    if (!fonte_field(fonte, "DEPS", deps_raw, sizeof(deps_raw))) return;
    char *dep = strtok(deps_raw, " \t");
    while (dep) {
        if (*dep && !is_installed(dep)) {
            printf("  ==> installing dependency: %s\n", dep);
            cmd_install(dep, cores, 1);
        }
        dep = strtok(NULL, " \t");
    }
}

/* ── install ─────────────────────────────────────────────────── */
static void cmd_install(const char *pkg, int cores, int as_dep) {
    /* reject package names with shell metacharacters */
    for (const char *c = pkg; *c; c++) {
        if (!( (*c>='a'&&*c<='z') || (*c>='A'&&*c<='Z') ||
               (*c>='0'&&*c<='9') || *c=='-' || *c=='_' || *c=='.' )) {
            printf("error: invalid package name '%s'\n", pkg);
            return;
        }
    }

    /* if no local recipe, try synthesising from the remote index */
    if (!recipe_exists(pkg)) {
        IndexEntry e;
        if (index_lookup(pkg, &e)) {
            if (!synth_recipe(&e)) return;
        } else {
            /* last resort: try repology */
            if (!repology_synth(pkg)) {
                printf("error: no recipe found for '%s'\n"
                       "  tried: local recipes, remote index, repology\n", pkg);
                return;
            }
        }
    }

    if (is_installed(pkg)) {
        printf("'%s' is already installed\n", pkg);
        return;
    }

    /* ask core count only for the top-level install, not for deps */
    if (!as_dep && cores == 0) {
        char input[16];
        printf("  cores to use for build [1-%d, default 1]: ", MAX_CORES);
        fflush(stdout);
        cores = 1;
        if (fgets(input, sizeof(input), stdin)) {
            int n = atoi(input);
            if (n > 0 && n <= MAX_CORES) cores = n;
        }
    } else if (cores == 0) {
        cores = 1;
    }

    /* read recipe metadata */
    char fonte[MAX_PATH];
    xpath(fonte, sizeof(fonte), "%s/%s/FONTE", RECIPES_DIR, pkg);
    char ver[64]="", url[512]="", sha256[128]="";
    fonte_field(fonte, "VERSION", ver,    sizeof(ver));
    fonte_field(fonte, "URL",     url,    sizeof(url));
    fonte_field(fonte, "SHA256",  sha256, sizeof(sha256));

    if (url[0] == '\0') {
        printf("error: recipe for '%s' has no URL\n", pkg);
        return;
    }

    printf("\n==> installing %s %s\n", pkg, ver);
    for (int i = 0; i <= 100; i += 20) { progress("preparing", i); usleep(20000); }

    install_deps(pkg, cores);

    /* set up cache directories */
    char pkgcache[MAX_PATH], srcdir[MAX_PATH], destdir[MAX_PATH];
    xpath(pkgcache, sizeof(pkgcache), "%s/%s",      CACHE_DIR, pkg);
    xpath(srcdir,   sizeof(srcdir),   "%s/src",     pkgcache);
    xpath(destdir,  sizeof(destdir),  "%s/dest",    pkgcache);
    mkdir(CACHE_DIR,  0755);
    mkdir(pkgcache,   0755);
    mkdir(srcdir,     0755);
    mkdir(destdir,    0755);

    /* download */
    printf("  ==> downloading...\n");
    char tarball[MAX_PATH];
    xpath(tarball, sizeof(tarball), "%s/src.tar.gz", pkgcache);
    char dl[MAX_PATH * 2 + 64];
    snprintf(dl, sizeof(dl), "wget -q --no-check-certificate -O '%s' '%s'", tarball, url);
    if (system(dl) != 0) { printf("error: download failed\n\n"); return; }

    /* verify checksum */
    if (!sha256_ok(tarball, sha256)) return;

    /* extract — try strip-components first, fall back to plain extract */
    printf("  ==> extracting...\n");
    char ex[MAX_PATH * 4 + 128];
    snprintf(ex, sizeof(ex),
        "tar -xf '%s' -C '%s' --strip-components=1 2>/dev/null"
        " || tar -xf '%s' -C '%s'",
        tarball, srcdir, tarball, srcdir);
    if (system(ex) != 0) { printf("error: extraction failed\n\n"); return; }

    /* build: source FONTE, then call build() */
    printf("  ==> building with %d core(s)...\n", cores);
    char build[MAX_PATH * 4 + 256];
    snprintf(build, sizeof(build),
        "cd '%s' && "
        "export MAKEFLAGS=-j%d && "
        "export DESTDIR='%s' && "
        ". '%s' && "
        "build",
        srcdir, cores, destdir, fonte);
    if (system(build) != 0) {
        printf("error: build failed for '%s'\n\n", pkg);
        return;
    }

    /* install files from DESTDIR -> real filesystem, recording manifest */
    printf("  ==> installing files...\n");
    char instdir[MAX_PATH];
    xpath(instdir, sizeof(instdir), "%s/%s", INSTALLED_DIR, pkg);
    mkdir(instdir, 0755);

    FILE *mf = manifest_open(pkg);
    char find_cmd[MAX_PATH + 32];
    snprintf(find_cmd, sizeof(find_cmd), "find '%s' -type f", destdir);
    FILE *fp = popen(find_cmd, "r");
    int nfiles = 0;
    if (fp) {
        char line[MAX_PATH];
        size_t dlen = strlen(destdir);
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = '\0';
            const char *rel = line + dlen; /* path relative to DESTDIR */
            char cp_cmd[MAX_PATH * 2 + 32];
            snprintf(cp_cmd, sizeof(cp_cmd), "cp -a '%s' '%s' 2>/dev/null", line, rel);
            if (system(cp_cmd) == 0) {
                manifest_add(mf, rel);
                nfiles++;
            }
        }
        pclose(fp);
    }
    if (mf) fclose(mf);

    mark_installed(pkg, ver);
    printf("==> %s %s installed (%d file(s))\n\n", pkg, ver, nfiles);
}

/* ── remove ──────────────────────────────────────────────────── */
static void cmd_remove(const char *pkg) {
    if (!is_installed(pkg)) {
        printf("'%s' is not installed\n", pkg);
        return;
    }
    printf("==> removing %s...\n", pkg);
    manifest_remove(pkg);
    mark_removed(pkg);
    printf("==> %s removed\n\n", pkg);
}

/* ── list ────────────────────────────────────────────────────── */
static void cmd_list(void) {
    printf("installed packages:\n\n");
    DIR *d = opendir(INSTALLED_DIR);
    if (!d) { printf("  (none)\n\n"); return; }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char ver[64];
        installed_version(ent->d_name, ver, sizeof(ver));
        printf("  %-24s %s\n", ent->d_name, ver[0] ? ver : "(unknown)");
        count++;
    }
    closedir(d);
    if (count == 0) printf("  (none)\n");
    printf("\n");
}

/* ── search ──────────────────────────────────────────────────── */
static void cmd_search(const char *query) {
    printf("results for '%s':\n\n", query);
    int found = 0;

    /* local recipes */
    DIR *d = opendir(RECIPES_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            if (strstr(ent->d_name, query)) {
                char ver[64]="", fonte[MAX_PATH];
                xpath(fonte, sizeof(fonte), "%s/%s/FONTE", RECIPES_DIR, ent->d_name);
                fonte_field(fonte, "VERSION", ver, sizeof(ver));
                printf("  %-24s %-12s [local]\n", ent->d_name, ver);
                found++;
            }
        }
        closedir(d);
    }

    /* remote index — skip packages already shown from local recipes */
    FILE *f = fopen(INDEX_FILE, "r");
    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            IndexEntry e;
            if (!parse_index_line(line, &e)) continue;
            if (strstr(e.name, query) || strstr(e.desc, query)) {
                if (!recipe_exists(e.name)) {
                    printf("  %-24s %-12s [remote] %s\n",
                           e.name, e.version, e.desc);
                    found++;
                }
            }
        }
        fclose(f);
    }

    /* repology */
    printf("  checking repology...\n");
    repology_search(query);

    if (!found) printf("  (no local/remote matches)\n");
    printf("\n");
}

/* ── info ────────────────────────────────────────────────────── */
static void cmd_info(const char *pkg) {
    int local  = recipe_exists(pkg);
    IndexEntry ie;
    int remote = index_lookup(pkg, &ie);

    if (!local && !remote) {
        printf("error: no recipe or index entry for '%s'\n\n", pkg);
        return;
    }

    printf("\npackage : %s\n", pkg);

    if (local) {
        char fonte[MAX_PATH], ver[64]="", url[512]="";
        xpath(fonte, sizeof(fonte), "%s/%s/FONTE", RECIPES_DIR, pkg);
        fonte_field(fonte, "VERSION", ver, sizeof(ver));
        fonte_field(fonte, "URL",     url, sizeof(url));
        printf("  version : %s\n", ver[0] ? ver : "(unknown)");
        printf("  url     : %s\n", url[0] ? url : "(none)");
        printf("  recipe  : local\n");
    }
    if (remote)
        printf("  remote  : %s  —  %s\n", ie.version, ie.desc);

    printf("  status  : %s", is_installed(pkg) ? "installed" : "not installed");
    if (is_installed(pkg)) {
        char ver[64];
        installed_version(pkg, ver, sizeof(ver));
        printf(" (%s)", ver);
    }
    printf("  repology: ");
    repology_search(pkg);
    printf("\n");
}

/* ── update ──────────────────────────────────────────────────── */
static void cmd_update(const char *pkg) {
    if (!is_installed(pkg)) {
        printf("'%s' is not installed\n", pkg);
        return;
    }
    char cur[64];
    installed_version(pkg, cur, sizeof(cur));

    char new_ver[64] = "";
    if (recipe_exists(pkg)) {
        char fonte[MAX_PATH];
        xpath(fonte, sizeof(fonte), "%s/%s/FONTE", RECIPES_DIR, pkg);
        fonte_field(fonte, "VERSION", new_ver, sizeof(new_ver));
    }
    if (new_ver[0] == '\0') {
        IndexEntry ie;
        if (index_lookup(pkg, &ie))
            snprintf(new_ver, sizeof(new_ver), "%s", ie.version);
    }
    if (new_ver[0] == '\0') {
        printf("error: cannot determine available version for '%s'\n", pkg);
        return;
    }

    printf("==> %s: installed=%s  available=%s\n", pkg, cur, new_ver);
    if (strcmp(cur, new_ver) == 0) {
        printf("==> %s is already up to date\n\n", pkg);
        return;
    }
    printf("==> updating %s  %s -> %s\n", pkg, cur, new_ver);
    cmd_remove(pkg);
    cmd_install(pkg, 0, 0);
}

/* ── help ────────────────────────────────────────────────────── */
static void help(void) {
    printf("\n  fpackage v%s — fonteOS package manager\n\n", VERSION);
    printf("  install <pkg>    build and install a package\n");
    printf("  remove  <pkg>    remove an installed package\n");
    printf("  list             list installed packages\n");
    printf("  search  <query>  search local recipes and remote index\n");
    printf("  info    <pkg>    show package information\n");
    printf("  sync             fetch/update the remote package index\n");
    printf("  update  <pkg>    reinstall if a newer version is available\n\n");
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) { help(); return 0; }
    const char *cmd = argv[1];

    if      (strcmp(cmd, "install") == 0 && argc == 3) cmd_install(argv[2], 0, 0);
    else if (strcmp(cmd, "remove")  == 0 && argc == 3) cmd_remove(argv[2]);
    else if (strcmp(cmd, "list")    == 0)               cmd_list();
    else if (strcmp(cmd, "search")  == 0 && argc == 3) cmd_search(argv[2]);
    else if (strcmp(cmd, "info")    == 0 && argc == 3) cmd_info(argv[2]);
    else if (strcmp(cmd, "sync")    == 0)               cmd_sync();
    else if (strcmp(cmd, "update")  == 0 && argc == 3) cmd_update(argv[2]);
    else { help(); return 1; }

    return 0;
}
/* placeholder */
