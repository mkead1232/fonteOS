#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <time.h>

#define RECIPES_DIR "/var/fpackage/recipes"
#define INSTALLED_DIR "/var/fpackage/installed"
#define CACHE_DIR "/var/fpackage/cache"
#define VERSION "0.2"

int get_terminal_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

void progress_bar(const char *label, int percent) {
    int width = get_terminal_width() - 20;
    if (width < 10) width = 10;
    int filled = (percent * width) / 100;
    printf("\r  %-10s [", label);
    for (int i = 0; i < width; i++)
        putchar(i < filled ? '#' : '-');
    printf("] %3d%%", percent);
    fflush(stdout);
    if (percent >= 100) putchar('\n');
}

void spinner(const char *label) {
    static int frame = 0;
    const char *frames[] = {"|", "/", "-", "\\"};
    printf("\r  %s %s ", frames[frame % 4], label);
    fflush(stdout);
    frame++;
}

int is_installed(const char *pkg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", INSTALLED_DIR, pkg);
    struct stat st;
    return stat(path, &st) == 0;
}

void mark_installed(const char *pkg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", INSTALLED_DIR, pkg);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "installed\n"); fclose(f); }
}

void mark_removed(const char *pkg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", INSTALLED_DIR, pkg);
    remove(path);
}

int recipe_exists(const char *pkg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/FONTE", RECIPES_DIR, pkg);
    struct stat st;
    return stat(path, &st) == 0;
}

void install_deps(const char *pkg, int cores) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/FONTE", RECIPES_DIR, pkg);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "DEPS=", 5) == 0) {
            char deps[512];
            strncpy(deps, line + 6, sizeof(deps));
            deps[strcspn(deps, "\"\n")] = 0;
            char *dep = strtok(deps, " ");
            while (dep) {
                if (!is_installed(dep)) {
                    printf("  ==> dependency: %s\n", dep);
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "fpackage install %s", dep);
                    system(cmd);
                }
                dep = strtok(NULL, " ");
            }
        }
    }
    fclose(f);
}

void cmd_install(const char *pkg) {
    if (!recipe_exists(pkg)) {
        printf("error: no recipe found for '%s'\n", pkg);
        return;
    }
    if (is_installed(pkg)) {
        printf("'%s' is already installed\n", pkg);
        return;
    }

    int cores = 1;
    char input[16];
    printf("  cores to use for build [1-%d, default 1]: ", 16);
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin)) {
        int n = atoi(input);
        if (n > 0 && n <= 16) cores = n;
    }

    printf("\n==> installing %s\n", pkg);

    for (int i = 0; i <= 100; i += 10) {
        progress_bar("preparing", i);
        usleep(30000);
    }

    install_deps(pkg, cores);

    printf("  ==> downloading...\n");
    char script[2048];
    snprintf(script, sizeof(script),
        "cd %s && "
        "mkdir -p %s/%s && "
        "cd %s/%s && "
        ". %s/%s/FONTE && "
        "wget -q \"$URL\" && "
        "echo '==> extracting...' && "
        "tar -xf * 2>/dev/null && "
        "cd $(ls -d */ | head -1) && "
        "echo '==> building...' && "
        "MAKEFLAGS=-j%d build",
        CACHE_DIR,
        CACHE_DIR, pkg,
        CACHE_DIR, pkg,
        RECIPES_DIR, pkg,
        cores
    );

    for (int i = 0; i < 3; i++) {
        spinner("working");
        usleep(100000);
    }

    int ret = system(script);
    printf("\n");
    if (ret == 0) {
        mark_installed(pkg);
        printf("==> %s installed successfully\n\n", pkg);
    } else {
        printf("error: build failed for '%s'\n\n", pkg);
    }
}

void cmd_remove(const char *pkg) {
    if (!is_installed(pkg)) {
        printf("'%s' is not installed\n", pkg);
        return;
    }
    printf("==> removing %s...\n", pkg);
    mark_removed(pkg);
    printf("==> %s removed\n", pkg);
}

void cmd_list() {
    printf("installed packages:\n\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls %s", INSTALLED_DIR);
    int ret = system(cmd);
    if (ret != 0) printf("  (none)\n");
    printf("\n");
}

void cmd_search(const char *pkg) {
    printf("available recipes matching '%s':\n\n", pkg);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ls %s | grep %s", RECIPES_DIR, pkg);
    int ret = system(cmd);
    if (ret != 0) printf("  (no matches)\n");
    printf("\n");
}

void cmd_info(const char *pkg) {
    if (!recipe_exists(pkg)) {
        printf("error: no recipe found for '%s'\n", pkg);
        return;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/%s/FONTE", RECIPES_DIR, pkg);
    printf("recipe for %s:\n\n", pkg);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "cat %.590s", path);
    system(cmd);
    printf("\nstatus: %s\n\n", is_installed(pkg) ? "installed" : "not installed");
}

void help() {
    printf("\n  fpackage v%s - fonteOS package manager\n\n", VERSION);
    printf("  install <pkg>   build and install a package\n");
    printf("  remove  <pkg>   remove an installed package\n");
    printf("  list            list installed packages\n");
    printf("  search  <pkg>   search available recipes\n");
    printf("  info    <pkg>   show package info\n\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { help(); return 0; }
    if (strcmp(argv[1], "install") == 0 && argc == 3) cmd_install(argv[2]);
    else if (strcmp(argv[1], "remove") == 0 && argc == 3) cmd_remove(argv[2]);
    else if (strcmp(argv[1], "list") == 0) cmd_list();
    else if (strcmp(argv[1], "search") == 0 && argc == 3) cmd_search(argv[2]);
    else if (strcmp(argv[1], "info") == 0 && argc == 3) cmd_info(argv[2]);
    else { help(); return 1; }
    return 0;
}
