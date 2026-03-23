#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void read_file_line(const char *path, char *buf, int size) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, size, "unknown"); return; }
    fgets(buf, size, f);
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
}

int main() {
    char hostname[64], kernel[128], cpumodel[128];

    read_file_line("/etc/hostname", hostname, sizeof(hostname));

    FILE *kver = fopen("/proc/version", "r");
    strcpy(kernel, "unknown");
    if (kver) {
        char tmp[256];
        fgets(tmp, sizeof(tmp), kver);
        sscanf(tmp, "Linux version %127s", kernel);
        fclose(kver);
    }

    FILE *cpu = fopen("/proc/cpuinfo", "r");
    strcpy(cpumodel, "unknown");
    if (cpu) {
        char line[256];
        while (fgets(line, sizeof(line), cpu)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) { strncpy(cpumodel, colon + 2, sizeof(cpumodel)); cpumodel[strcspn(cpumodel, "\n")] = 0; }
                break;
            }
        }
        fclose(cpu);
    }

    int uptime_secs = 0;
    FILE *up = fopen("/proc/uptime", "r");
    if (up) { fscanf(up, "%d", &uptime_secs); fclose(up); }

    long mem_total = 0, mem_free = 0;
    FILE *mem = fopen("/proc/meminfo", "r");
    if (mem) {
        char line[128];
        while (fgets(line, sizeof(line), mem)) {
            if (strncmp(line, "MemTotal", 8) == 0) sscanf(line, "MemTotal: %ld kB", &mem_total);
            if (strncmp(line, "MemAvailable", 12) == 0) sscanf(line, "MemAvailable: %ld kB", &mem_free);
        }
        fclose(mem);
    }
    long mem_used = (mem_total - mem_free) / 1024;
    long mem_total_mb = mem_total / 1024;

    printf("\n");
    printf("                                       .#%%@@@@@@@@@@@@@@@%%:  \n");
    printf(" +@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@+--            *@#: \n");
    printf(":@@:                                    :-.             :+@%% \n");
    printf("-@#                                     %%@.              .@%% \n");
    printf("-@#   *@@@@@@@@@@@@@@@             .::::@@.              .@%% \n");
    printf("@@#   @@                           *@@@@@@@@@@@@         .@%% \n");
    printf("@@   :@@                           :@*  %%@.   =%%%%%%#:  .@%% \n");
    printf("@@   @@:                     :=    :@#  %%@. .@@+----*@#  .@%% \n");
    printf("@@ :+@@@@@@@@@@= #@@@@@@@@@# @@@=  :@#  *@@ .@@@@@@@#@@@ .@@:\n");
    printf("@@ -#@@=         @@       @@= @@@@=-@#   @@..@%%   .*###=  *@#\n");
    printf("@@   @@          @@:::::::=@* @@-%%@@@@:  #@+.@@:::..       @@\n");
    printf("@#   @@          #@@@@@@@@@@= #*   .%%@:  =@+.@@@@@@@@@@@@@@@@\n");
    printf("@#                                       .:.        .::::::::\n");
    printf(" :***********:                  .+******-                    \n");
    printf(" :**********@@@@@@@@@@@@@@@@@@@@@@******=                    \n");
    printf("                                                             \n");
    printf("                        -@@@@@@@@@@@@@@@@@@@@@@-             \n");
    printf("                        ...                                  \n");
    printf("                                                             \n");
    printf("      %%@@@@@@@@@@@@@@@@@@@@@@@%%                              \n");
    printf("                                                             \n");
    printf("                           #@@@@@@@@@@@*                     \n");
    printf("\n");
    printf("  host     %s\n", hostname);
    printf("  kernel   %s\n", kernel);
    printf("  uptime   %d seconds\n", uptime_secs);
    printf("  cpu      %s\n", cpumodel);
    printf("  memory   %ld MB / %ld MB\n", mem_used, mem_total_mb);
    printf("\n");

    return 0;
}
