/* quickfetch — a small fastfetch-like system info tool written in C.
 * Ported from quickfetch.nim.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>

#ifdef QF_TIMING
static double qfNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#define QF_MARK(var) double var = qfNowMs()
#define QF_LOG(label, start, end) \
    fprintf(stderr, "[qf] %-28s %.3f ms\n", label, (end) - (start))
#else
#define QF_MARK(var)
#define QF_LOG(label, start, end)
#endif

/* --------------------------------------------------------------------- */
/* Colors                                                                 */
/* --------------------------------------------------------------------- */

#define RESET      "\x1b[0m"
#define BOLD       "\x1b[1m"
#define FOREGROUND "\x1b[0m"
#define RED        "\x1b[31m"
#define GREEN      "\x1b[32m"
#define YELLOW     "\x1b[33m"
#define BLUE       "\x1b[34m"
#define MAGENTA    "\x1b[35m"
#define CYAN       "\x1b[36m"

static const char *terminalColors[] = {
    "\x1b[90m", "\x1b[37m", "\x1b[36m", "\x1b[35m",
    "\x1b[34m", "\x1b[33m", "\x1b[32m", "\x1b[31m",
};
#define N_TERMINAL_COLORS (sizeof(terminalColors) / sizeof(terminalColors[0]))

/* --------------------------------------------------------------------- */
/* Appearance                                                             */
/* --------------------------------------------------------------------- */

static const char *logo[] = {
    "           +           ",
    "         +++++         ",
    "  +      +++++      +  ",
    "++++++    +++    ++++++",
    " +++++++  +++  +++++++ ",
    "     +++++++++++++     ",
    "        +++++++        ",
    "     +++++++++++++     ",
    " +++++++  +++  +++++++ ",
    "++++++    +++    ++++++",
    "  +      +++++      +  ",
    "         +++++         ",
    "           +           ",
};
#define N_LOGO_LINES (sizeof(logo) / sizeof(logo[0]))
#define LOGO_COLOR CYAN

#define ICON_OS "\U0000f17c"
#define ICON_OS_COLOR BLUE

#define ICON_SHELL "\U0000ebca"
#define ICON_SHELL_COLOR BLUE

#define ICON_TERMINAL "\U0000f489"
#define ICON_TERMINAL_COLOR BLUE

#define ICON_CPU "\U0000f4bc"
#define ICON_CPU_COLOR BLUE

#define ICON_GPU "\U000f08ae"
#define ICON_GPU_COLOR BLUE

#define ICON_MEMORY "\U000f17f1"
#define ICON_MEMORY_COLOR BLUE

#define TITLE_COLOR CYAN
#define COLOR_CHARACTER "\U000025cf"
#define SEPARATOR_COLOR FOREGROUND

/* --------------------------------------------------------------------- */
/* Modules order                                                          */
/* --------------------------------------------------------------------- */

typedef enum {
    M_TITLE, M_SEPARATOR, M_OS, M_SHELL, M_TERMINAL,
    M_CPU, M_GPU, M_MEMORY, M_BREAK, M_COLORS,
    M_COUNT
} Module;

static const Module modulesOrder[] = {
    M_TITLE, M_SEPARATOR, M_OS, M_SHELL, M_TERMINAL,
    M_CPU, M_GPU, M_MEMORY, M_BREAK, M_COLORS,
};
#define N_MODULES (sizeof(modulesOrder) / sizeof(modulesOrder[0]))

/* --------------------------------------------------------------------- */
/* String helpers                                                         */
/* --------------------------------------------------------------------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static void replaceAll(char *s, const char *needle, const char *repl) {
    /* In-place-ish replace via a temp buffer; needle/repl are short so this is fine. */
    char buf[512];
    size_t needleLen = strlen(needle);
    size_t replLen = strlen(repl);
    char *dst = buf;
    char *src = s;
    size_t remaining = sizeof(buf) - 1;
    while (*src && remaining > 0) {
        if (strncmp(src, needle, needleLen) == 0) {
            size_t n = replLen < remaining ? replLen : remaining;
            memcpy(dst, repl, n);
            dst += n;
            remaining -= n;
            src += needleLen;
        } else {
            *dst++ = *src++;
            remaining--;
        }
    }
    *dst = '\0';
    snprintf(s, 512, "%s", buf);
}

static int startsWith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* --------------------------------------------------------------------- */
/* Get functions — return plain data, no coloring/printing                */
/* --------------------------------------------------------------------- */

static void getUserHost(char *out, size_t outLen) {
    const char *user = getenv("USER");
    if (!user) user = "user";

    char host[256] = "";
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(host, sizeof(host), f)) {
            char *t = trim(host);
            memmove(host, t, strlen(t) + 1);
        }
        fclose(f);
    }
    if (host[0] == '\0') {
        const char *h = getenv("HOSTNAME");
        snprintf(host, sizeof(host), "%s", h ? h : "host");
    }
    snprintf(out, outLen, "%s@%s", user, host);
}

static void getOS(char *out, size_t outLen) {
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (startsWith(line, "PRETTY_NAME=")) {
                char *value = line + strlen("PRETTY_NAME=");
                value = trim(value);
                /* strip surrounding quotes */
                size_t len = strlen(value);
                if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
                    value[len - 1] = '\0';
                    value++;
                }
                snprintf(out, outLen, "%s", value);
                fclose(f);
                return;
            }
        }
        fclose(f);
    }
    snprintf(out, outLen, "Unknown");
}

static void getShell(char *out, size_t outLen) {
    const char *shellPath = getenv("SHELL");
    if (!shellPath) shellPath = "unknown";

    const char *slash = strrchr(shellPath, '/');
    const char *name = slash ? slash + 1 : shellPath;

    char version[128] = "";

    if (strcmp(name, "bash") == 0) {
        const char *v = getenv("BASH_VERSION");
        if (v) {
            snprintf(version, sizeof(version), "%s", v);
            char *paren = strchr(version, '(');
            if (paren) *paren = '\0';
            char *t = trim(version);
            memmove(version, t, strlen(t) + 1);
        }
    } else if (strcmp(name, "zsh") == 0) {
        const char *v = getenv("ZSH_VERSION");
        if (v) snprintf(version, sizeof(version), "%s", v);
    } else if (strcmp(name, "fish") == 0) {
        const char *v = getenv("FISH_VERSION");
        if (v) snprintf(version, sizeof(version), "%s", v);
    }

    if (version[0] == '\0') {
        /* Uncommon shell with no known version env var — fall back to
         * spawning it, since we have no other cheap way to get this. */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", shellPath);
        FILE *p = popen(cmd, "r");
        if (p) {
            char firstLine[512] = "";
            if (fgets(firstLine, sizeof(firstLine), p)) {
                char *tok = strtok(firstLine, " \t\r\n");
                while (tok) {
                    if (isdigit((unsigned char)tok[0])) {
                        snprintf(version, sizeof(version), "%s", tok);
                        break;
                    }
                    tok = strtok(NULL, " \t\r\n");
                }
            }
            pclose(p);
        }
    }

    if (version[0] != '\0')
        snprintf(out, outLen, "%s %s", name, version);
    else
        snprintf(out, outLen, "%s", name);
}

static int isKnownShellComm(const char *comm) {
    static const char *known[] = {"bash", "zsh", "fish", "sh", "quickfetch"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strcmp(comm, known[i]) == 0)
            return 1;
    return 0;
}

static void getTerminal(char *out, size_t outLen) {
    const char *term = getenv("TERM_PROGRAM");
    if (term && term[0] != '\0') {
        snprintf(out, outLen, "%s", term);
        return;
    }

    /* Walk up the process tree looking for the first non-shell ancestor. */
    pid_t pid = getpid();
    char result[256] = "";

    for (int i = 0; i < 10; i++) {
        char statPath[64];
        snprintf(statPath, sizeof(statPath), "/proc/%d/stat", pid);

        FILE *f = fopen(statPath, "r");
        if (!f) break;
        char stat[1024];
        size_t n = fread(stat, 1, sizeof(stat) - 1, f);
        fclose(f);
        stat[n] = '\0';

        char *closeParen = strrchr(stat, ')');
        if (!closeParen) break;

        /* Fields after "<pid> (<comm>) " are: state, ppid, ... */
        char *rest = closeParen + 2; /* skip ") " */
        char state[16], ppidStr[16];
        if (sscanf(rest, "%15s %15s", state, ppidStr) != 2) break;

        char commPath[64];
        snprintf(commPath, sizeof(commPath), "/proc/%s/comm", ppidStr);

        FILE *cf = fopen(commPath, "r");
        if (!cf) break;
        char comm[256] = "";
        if (fgets(comm, sizeof(comm), cf)) {
            char *t = trim(comm);
            memmove(comm, t, strlen(t) + 1);
        }
        fclose(cf);

        if (!isKnownShellComm(comm)) {
            snprintf(result, sizeof(result), "%s", comm);
            break;
        }
        pid = (pid_t)atoi(ppidStr);
    }

    snprintf(out, outLen, "%s", result[0] != '\0' ? result : "unknown");
}

static void cleanCpuModel(const char *raw, char *out, size_t outLen) {
    /* Strips vendor/marketing cruft fastfetch-style, e.g. turns
     * "AMD Ryzen 5 7500F 6-Core Processor" into "AMD Ryzen 5 7500F", and
     * "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz" into "Intel Core i7-9700K". */
    char model[512];
    snprintf(model, sizeof(model), "%s", raw);
    replaceAll(model, "(R)", "");
    replaceAll(model, "(TM)", "");
    replaceAll(model, "(C)", "");

    char *atIdx = strstr(model, " @");
    if (atIdx) *atIdx = '\0';

    /* Tokenize, cut at first "cpu" (case-insensitive) or token ending in "-core" */
    char tokens[64][128];
    int nTokens = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", model);
    char *tok = strtok(tmp, " \t");
    while (tok && nTokens < 64) {
        snprintf(tokens[nTokens], sizeof(tokens[nTokens]), "%s", tok);
        nTokens++;
        tok = strtok(NULL, " \t");
    }

    int cutAt = nTokens;
    for (int i = 0; i < nTokens; i++) {
        char lower[128];
        snprintf(lower, sizeof(lower), "%s", tokens[i]);
        for (char *c = lower; *c; c++) *c = (char)tolower((unsigned char)*c);
        size_t len = strlen(lower);
        int endsWithCore = len >= 5 && strcmp(lower + len - 5, "-core") == 0;
        if (strcmp(lower, "cpu") == 0 || endsWithCore) {
            cutAt = i;
            break;
        }
    }

    out[0] = '\0';
    for (int i = 0; i < cutAt; i++) {
        if (i > 0) strncat(out, " ", outLen - strlen(out) - 1);
        strncat(out, tokens[i], outLen - strlen(out) - 1);
    }
}

static void cleanGpuModel(const char *raw, char *out, size_t outLen) {
    /* lspci reports e.g. "NVIDIA Corporation AD104 [GeForce RTX 4070 Ti]
     * (rev a1)" — pull out the product name from the last bracket pair. */
    const char *closeB = strrchr(raw, ']');
    if (closeB) {
        const char *openB = NULL;
        for (const char *p = raw; p < closeB; p++)
            if (*p == '[') openB = p;
        if (openB) {
            size_t len = (size_t)(closeB - openB - 1);
            if (len >= outLen) len = outLen - 1;
            memcpy(out, openB + 1, len);
            out[len] = '\0';
            return;
        }
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", raw);
    char *t = trim(tmp);
    snprintf(out, outLen, "%s", t);
}

static void getCpu(char *out, size_t outLen) {
    /* /proc/cpuinfo is small enough (a few KB) that one read() covers
     * it; skip fopen's fstat-for-buffer-sizing + heap allocation and
     * just read it raw, same as getGpu's uevent reads. */
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd >= 0) {
        char buf[8192];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            const char *p = buf;
            while (*p) {
                if (startsWith(p, "model name")) {
                    const char *colon = strchr(p, ':');
                    if (colon) {
                        const char *lineEnd = strpbrk(colon, "\r\n");
                        size_t len = lineEnd ? (size_t)(lineEnd - colon) : strlen(colon);
                        char valueBuf[512];
                        if (len >= sizeof(valueBuf)) len = sizeof(valueBuf) - 1;
                        memcpy(valueBuf, colon + 1, len);
                        valueBuf[len] = '\0';
                        char *value = trim(valueBuf);
                        cleanCpuModel(value, out, outLen);
                        return;
                    }
                }
                const char *nl = strchr(p, '\n');
                if (!nl) break;
                p = nl + 1;
            }
        }
    }
    snprintf(out, outLen, "Unknown CPU");
}

static const char *pciIdsPaths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    "/usr/share/pci.ids",
};
#define N_PCI_IDS_PATHS (sizeof(pciIdsPaths) / sizeof(pciIdsPaths[0]))

static void lookupPciNames(const char *vendorId, const char *deviceId,
                            char *vendorOut, size_t vendorLen,
                            char *deviceOut, size_t deviceLen) {
    /* Parses pci.ids to resolve a vendor:device pair to human-readable names,
     * without spawning lspci. Format is tab-indented:
     *   vendorId  vendorName
     *   \tdeviceId  deviceName */
    vendorOut[0] = '\0';
    deviceOut[0] = '\0';

    for (size_t pi = 0; pi < N_PCI_IDS_PATHS; pi++) {
        FILE *f = fopen(pciIdsPaths[pi], "r");
        if (!f) continue;

        /* pci.ids can be 2-3 MB; a larger stdio buffer means far fewer
         * read() syscalls while scanning down to the matching vendor. */
        static char pciBuf[65536];
        setvbuf(f, pciBuf, _IOFBF, sizeof(pciBuf));

        int inVendor = 0;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;

            if (line[0] != '\t') {
                /* top-level vendor line: "<id>  <name>" */
                char id[16] = "";
                char *p = line;
                int i = 0;
                while (*p && !isspace((unsigned char)*p) && i < 15) id[i++] = *p++;
                id[i] = '\0';
                while (isspace((unsigned char)*p)) p++;
                for (char *c = id; *c; c++) *c = (char)tolower((unsigned char)*c);

                if (id[0] != '\0' && *p != '\0' && strcmp(id, vendorId) == 0) {
                    snprintf(vendorOut, vendorLen, "%s", p);
                    inVendor = 1;
                } else {
                    inVendor = 0;
                }
            } else if (inVendor && line[1] != '\0' && line[1] != '\t') {
                char id[16] = "";
                char *p = line + 1;
                int i = 0;
                while (*p && !isspace((unsigned char)*p) && i < 15) id[i++] = *p++;
                id[i] = '\0';
                while (isspace((unsigned char)*p)) p++;
                for (char *c = id; *c; c++) *c = (char)tolower((unsigned char)*c);

                if (id[0] != '\0' && *p != '\0' && strcmp(id, deviceId) == 0) {
                    snprintf(deviceOut, deviceLen, "%s", p);
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
        if (vendorOut[0] != '\0') return;
    }
}

#ifdef HAVE_PCI_TABLE
#include "pci_table.h"

static int pciTableLookup(const char *vendorIdHex, const char *deviceIdHex,
                           char *out, size_t outLen) {
    uint32_t vendorId = (uint32_t)strtoul(vendorIdHex, NULL, 16);
    uint32_t deviceId = (uint32_t)strtoul(deviceIdHex, NULL, 16);
    uint32_t key = (vendorId << 16) | deviceId;

    size_t lo = 0, hi = PCI_TABLE_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pciTable[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo < PCI_TABLE_COUNT && pciTable[lo].key == key) {
        snprintf(out, outLen, "%s", pciTable[lo].name);
        return 1;
    }
    return 0;
}
#else
static int pciTableLookup(const char *vendorIdHex, const char *deviceIdHex,
                           char *out, size_t outLen) {
    (void)vendorIdHex; (void)deviceIdHex; (void)out; (void)outLen;
    return 0; /* no compiled table available — caller falls back to lookupPciNames() */
}
#endif

/* Reads a whole small sysfs pseudo-file with raw syscalls (open/read/close)
 * instead of fopen/fgets/fclose — skips stdio's fstat-to-size-the-buffer
 * and heap allocation, which is pure overhead for a file that's a few
 * hundred bytes and read exactly once. */
static int readSmallFile(int dirFd, const char *relPath, char *buf, size_t bufLen) {
    int fd = openat(dirFd, relPath, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, bufLen - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return 1;
}

/* Pulls the value of a `KEY=value` line out of a uevent file's contents. */
static int parseUeventField(const char *content, const char *key, char *out, size_t outLen) {
    size_t keyLen = strlen(key);
    const char *p = content;
    while (*p) {
        if (strncmp(p, key, keyLen) == 0) {
            p += keyLen;
            const char *end = strpbrk(p, "\r\n");
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len >= outLen) len = outLen - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 1;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static void getGpu(char *out, size_t outLen) {
    /* /sys/class/drm/cardN are symlinks straight to actual GPU PCI
     * devices — walking this instead of /sys/bus/pci/devices means we
     * only ever touch the 1-2 devices that are actually GPUs, instead
     * of every PCI function on the bus (USB/audio/NVMe/bridges/etc,
     * often 20-40+ devices on a typical desktop). */
    const char *drmDir = "/sys/class/drm";
    DIR *d = opendir(drmDir);
    if (!d) {
        snprintf(out, outLen, "Unknown GPU");
        return;
    }
    int dfd = dirfd(d);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        /* Only want "cardN" entries, not "cardN-HDMI-A-1" style output
         * connectors or "renderD1xx" nodes. */
        if (strncmp(name, "card", 4) != 0) continue;
        const char *digits = name + 4;
        if (*digits == '\0') continue;
        int allDigits = 1;
        for (const char *c = digits; *c; c++) {
            if (!isdigit((unsigned char)*c)) { allDigits = 0; break; }
        }
        if (!allDigits) continue;

        char relPath[64];
        snprintf(relPath, sizeof(relPath), "%s/device/uevent", name);

        /* One read gets both the class code and vendor:device pair —
         * versus separate class/vendor/device files under the old
         * /sys/bus/pci/devices walk. */
        char content[1024];
        if (!readSmallFile(dfd, relPath, content, sizeof(content))) continue;

        char classStr[16] = "";
        if (!parseUeventField(content, "PCI_CLASS=", classStr, sizeof(classStr))) continue;

        /* PCI_CLASS is the full class/subclass/prog-if code; top byte
         * 0x03 = display controller (VGA/3D/other). */
        unsigned long classVal = strtoul(classStr, NULL, 16);
        if (((classVal >> 16) & 0xFF) != 0x03) continue;

        char idStr[16] = "";
        if (!parseUeventField(content, "PCI_ID=", idStr, sizeof(idStr))) continue;
        char *colon = strchr(idStr, ':');
        if (!colon) continue;
        *colon = '\0';

        /* pci.ids / pciTable ids are lowercase hex; PCI_ID from uevent
         * is uppercase, so normalize before lookup. */
        char vId[16], dId[16];
        snprintf(vId, sizeof(vId), "%s", idStr);
        snprintf(dId, sizeof(dId), "%s", colon + 1);
        for (char *c = vId; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (char *c = dId; *c; c++) *c = (char)tolower((unsigned char)*c);

        char device[256] = "";
        if (pciTableLookup(vId, dId, device, sizeof(device))) {
            char name2[256];
            cleanGpuModel(device, name2, sizeof(name2));
            snprintf(out, outLen, "%s", name2);
            closedir(d);
            return;
        }

        char vendor[256] = "";
        device[0] = '\0';
        lookupPciNames(vId, dId, vendor, sizeof(vendor), device, sizeof(device));

        if (device[0] != '\0') {
            char name2[256];
            cleanGpuModel(device, name2, sizeof(name2));
            snprintf(out, outLen, "%s", name2);
            closedir(d);
            return;
        } else if (vendor[0] != '\0') {
            snprintf(out, outLen, "%s", vendor);
            closedir(d);
            return;
        }
    }
    closedir(d);
    snprintf(out, outLen, "Unknown GPU");
}

static void getMemory(double *totalGiB, int *usedPercent) {
    long totalKb = 0, availKb = 0;
    int haveTotal = 0, haveAvail = 0;

    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!haveTotal && startsWith(line, "MemTotal:")) {
                sscanf(line + strlen("MemTotal:"), "%ld", &totalKb);
                haveTotal = 1;
            } else if (startsWith(line, "MemAvailable:")) {
                sscanf(line + strlen("MemAvailable:"), "%ld", &availKb);
                haveAvail = 1;
            }
            if (haveTotal && haveAvail) break;
        }
        fclose(f);
    }

    *totalGiB = (double)totalKb / 1024.0 / 1024.0;
    *usedPercent = totalKb > 0
        ? (int)(((double)(totalKb - availKb) / (double)totalKb) * 100.0)
        : 0;
}

/* --------------------------------------------------------------------- */
/* Print function — all coloring and output happens here                  */
/* --------------------------------------------------------------------- */

static const char *colorForPercent(int p) {
    if (p < 50) return GREEN;
    if (p < 80) return YELLOW;
    return RED;
}

#define MAX_INFO_LINES 16
#define INFO_LINE_LEN 512

/* Each detector writes into its own fixed-size buffer, so running them on
 * separate threads is safe with no locking: no shared mutable state. */
typedef struct {
    char buf[256];
} StrResult;

typedef struct {
    double totalGiB;
    int usedPercent;
} MemResult;

typedef struct {
    StrResult os, shell, terminal;
    MemResult mem;
} CheapResult;

static void *cheapThread(void *arg) {
    CheapResult *r = (CheapResult *)arg;
    QF_MARK(t0);
    getOS(r->os.buf, sizeof(r->os.buf));
    QF_MARK(t1); QF_LOG("getOS", t0, t1);

    QF_MARK(t2);
    getShell(r->shell.buf, sizeof(r->shell.buf));
    QF_MARK(t3); QF_LOG("getShell", t2, t3);

    QF_MARK(t4);
    getTerminal(r->terminal.buf, sizeof(r->terminal.buf));
    QF_MARK(t5); QF_LOG("getTerminal", t4, t5);

    QF_MARK(t6);
    getMemory(&r->mem.totalGiB, &r->mem.usedPercent);
    QF_MARK(t7); QF_LOG("getMemory", t6, t7);
    return NULL;
}

static void *cpuThread(void *arg) {
    QF_MARK(t0);
    getCpu(((StrResult *)arg)->buf, sizeof(((StrResult *)arg)->buf));
    QF_MARK(t1);
    QF_LOG("getCpu", t0, t1);
    return NULL;
}
static void *gpuThread(void *arg) {
    QF_MARK(t0);
    getGpu(((StrResult *)arg)->buf, sizeof(((StrResult *)arg)->buf));
    QF_MARK(t1);
    QF_LOG("getGpu", t0, t1);
    return NULL;
}

static int renderInfoLines(char lines[MAX_INFO_LINES][INFO_LINE_LEN]) {
    QF_MARK(tStart);

    /* Serial version: no threads at all. With each detector down to
     * single-digit-to-low-double-digit microseconds, pthread_create's
     * setup cost (mmap+guard page, kernel thread bookkeeping) can cost
     * more than the parallelism it buys back. This runs everything in
     * call order on the main thread. */
    char userHost[256];
    QF_MARK(tUh0);
    getUserHost(userHost, sizeof(userHost));
    QF_MARK(tUh1);
    QF_LOG("getUserHost", tUh0, tUh1);

    char separator[256];
    size_t hostLen = strlen(userHost);
    if (hostLen >= sizeof(separator)) hostLen = sizeof(separator) - 1;
    memset(separator, '-', hostLen);
    separator[hostLen] = '\0';

    StrResult osR, shellR, terminalR, cpuR, gpuR;
    MemResult memR;

    QF_MARK(tOs0);
    getOS(osR.buf, sizeof(osR.buf));
    QF_MARK(tOs1);
    QF_LOG("getOS", tOs0, tOs1);

    QF_MARK(tShell0);
    getShell(shellR.buf, sizeof(shellR.buf));
    QF_MARK(tShell1);
    QF_LOG("getShell", tShell0, tShell1);

    QF_MARK(tTerm0);
    getTerminal(terminalR.buf, sizeof(terminalR.buf));
    QF_MARK(tTerm1);
    QF_LOG("getTerminal", tTerm0, tTerm1);

    QF_MARK(tMem0);
    getMemory(&memR.totalGiB, &memR.usedPercent);
    QF_MARK(tMem1);
    QF_LOG("getMemory", tMem0, tMem1);

    QF_MARK(tCpu0);
    getCpu(cpuR.buf, sizeof(cpuR.buf));
    QF_MARK(tCpu1);
    QF_LOG("getCpu", tCpu0, tCpu1);

    QF_MARK(tGpu0);
    getGpu(gpuR.buf, sizeof(gpuR.buf));
    QF_MARK(tGpu1);
    QF_LOG("getGpu", tGpu0, tGpu1);

    char *os = osR.buf, *shell = shellR.buf, *terminal = terminalR.buf;
    char *cpu = cpuR.buf, *gpu = gpuR.buf;
    double totalGiB = memR.totalGiB;
    int usedPercent = memR.usedPercent;

    int n = 0;
    for (size_t mi = 0; mi < N_MODULES; mi++) {
        Module m = modulesOrder[mi];
        char *out = lines[n];
        switch (m) {
            case M_TITLE: {
                char *at = strchr(userHost, '@');
                if (at) {
                    *at = '\0';
                    snprintf(out, INFO_LINE_LEN,
                        BOLD TITLE_COLOR "%s" FOREGROUND "@" BOLD TITLE_COLOR "%s" RESET,
                        userHost, at + 1);
                    *at = '@';
                } else {
                    snprintf(out, INFO_LINE_LEN, BOLD TITLE_COLOR "%s" RESET, userHost);
                }
                break;
            }
            case M_SEPARATOR:
                snprintf(out, INFO_LINE_LEN, SEPARATOR_COLOR "%s" RESET, separator);
                break;
            case M_OS:
                snprintf(out, INFO_LINE_LEN, ICON_OS_COLOR ICON_OS RESET "  %s", os);
                break;
            case M_SHELL:
                snprintf(out, INFO_LINE_LEN, ICON_SHELL_COLOR ICON_SHELL RESET "  %s", shell);
                break;
            case M_TERMINAL:
                snprintf(out, INFO_LINE_LEN, ICON_TERMINAL_COLOR ICON_TERMINAL RESET "  %s", terminal);
                break;
            case M_CPU:
                snprintf(out, INFO_LINE_LEN, ICON_CPU_COLOR ICON_CPU RESET "  %s", cpu);
                break;
            case M_GPU:
                snprintf(out, INFO_LINE_LEN, ICON_GPU_COLOR ICON_GPU RESET "  %s", gpu);
                break;
            case M_MEMORY:
                snprintf(out, INFO_LINE_LEN,
                    ICON_MEMORY_COLOR ICON_MEMORY RESET "  %.2f GiB %s%d%%" RESET,
                    totalGiB, colorForPercent(usedPercent), usedPercent);
                break;
            case M_BREAK:
                out[0] = '\0';
                break;
            case M_COLORS: {
                out[0] = '\0';
                for (size_t c = 0; c < N_TERMINAL_COLORS; c++) {
                    if (c > 0) strncat(out, " ", INFO_LINE_LEN - strlen(out) - 1);
                    char dot[64];
                    snprintf(dot, sizeof(dot), "%s%s%s", terminalColors[c], COLOR_CHARACTER, RESET);
                    strncat(out, dot, INFO_LINE_LEN - strlen(out) - 1);
                }
                break;
            }
            default:
                out[0] = '\0';
                break;
        }
        n++;
    }

#ifdef QF_TIMING
    QF_MARK(tEnd);
    fprintf(stderr, "[qf] %-28s %.3f ms\n", "TOTAL", tEnd - tStart);
#endif

    return n;
}

static void printFetch(void) {
    char infoLines[MAX_INFO_LINES][INFO_LINE_LEN];
    int infoCount = renderInfoLines(infoLines);

    size_t logoWidth = strlen(logo[0]);
    size_t lineCount = N_LOGO_LINES > (size_t)infoCount ? N_LOGO_LINES : (size_t)infoCount;

    /* Assemble the entire frame in memory first. stdout is line-buffered
     * when attached to a terminal, so printing line-by-line would cost one
     * write() syscall per line; one buffer + one fwrite costs a single
     * syscall for the whole frame. */
    char out[8192];
    size_t len = 0;
    len += (size_t)snprintf(out + len, sizeof(out) - len, "%s", "");

    for (size_t i = 0; i < lineCount; i++) {
        if (i < N_LOGO_LINES) {
            len += (size_t)snprintf(out + len, sizeof(out) - len,
                BOLD LOGO_COLOR "%s" RESET, logo[i]);
        } else {
            for (size_t s = 0; s < logoWidth && len < sizeof(out) - 1; s++)
                out[len++] = ' ';
            out[len] = '\0';
        }
        len += (size_t)snprintf(out + len, sizeof(out) - len, "  ");
        if (i < (size_t)infoCount) {
            len += (size_t)snprintf(out + len, sizeof(out) - len, "%s", infoLines[i]);
        }
        if (len < sizeof(out) - 1) out[len++] = '\n';
        if (len >= sizeof(out) - 1) break; /* frame too large — truncate safely */
    }

    fwrite(out, 1, len, stdout);
}

int main(void) {
    printFetch();
    return 0;
}
