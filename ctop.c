#define _GNU_SOURCE

#include <nvml.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTOP_VERSION "0.1.0"
#define GRAPH_ROWS 5
#define HISTORY_LIMIT 16384
#define MAX_GPUS 32
#define MAX_GPU_PROCESSES 512
#define MAX_CPU_PROCESSES 2048
#define MAX_SCREEN_COLS 2048
#define MAX_OVERLAYS 8192
#define MAX_NETWORK_INTERFACES 32
#define MAX_CLICK_TARGETS 32
#define INPUT_BUFFER_SIZE 8192

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum {
    ATTR_NORMAL = 0,
    ATTR_DIM,
    ATTR_CYAN,
    ATTR_CYAN_BOLD,
    ATTR_GREEN,
    ATTR_TEXT_GREEN,
    ATTR_YELLOW,
    ATTR_RED,
    ATTR_SELECTED,
    ATTR_PROMPT,
};

static bool color_enabled = true;
static FILE *debug_log_file = NULL;
static const char *BLOCK_GLYPHS[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

/* ------------------------------------------------------------------------- */
/* Data types                                                                */

typedef struct {
    double timestamp;
    double value;
} Sample;

typedef struct {
    Sample *data;
    size_t capacity;
    size_t count;
    size_t start;
} History;

typedef struct {
    pid_t pid;
    unsigned long long gpu_memory;
    bool gpu_memory_valid;
    double sm;
    bool sm_valid;
    double cpu;
    bool cpu_valid;
    double memory;
    bool memory_valid;
    double elapsed;
    bool elapsed_valid;
    char user[64];
    char command[512];
} ProcessInfo;

typedef struct {
    int index;
    nvmlDevice_t handle;
    char name[96];
    double temperature;
    bool temperature_valid;
    double power;
    bool power_valid;
    double power_limit;
    bool power_limit_valid;
    unsigned long long memory_used;
    unsigned long long memory_total;
    bool memory_valid;
    double utilization;
    bool utilization_valid;
    ProcessInfo processes[MAX_GPU_PROCESSES];
    size_t process_count;
} GpuInfo;

typedef struct {
    unsigned long long ram_used;
    unsigned long long ram_total;
    double ram_percent;
    unsigned long long swap_used;
    unsigned long long swap_total;
    double swap_percent;
} SystemInfo;

typedef struct {
    char name[64];
    double rx_bps;
    double tx_bps;
    double total_bps;
    double capacity_bps;
    double utilization_percent;
    bool capacity_valid;
    bool utilization_valid;
    bool valid;
} NetworkInterfaceInfo;

typedef struct {
    NetworkInterfaceInfo interfaces[MAX_NETWORK_INTERFACES];
    int interface_count;
    double rx_bps;
    double tx_bps;
    double total_bps;
    double utilization_percent;
    double capacity_bps;
    bool utilization_valid;
    bool valid;
} NetworkInfo;

typedef struct {
    char name[64];
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    double timestamp;
    bool valid;
} NetworkPrevious;

typedef struct {
    pid_t pid;
    unsigned long long process_ticks;
    unsigned long long total_ticks;
    double value;
    bool valid;
} CpuPrevious;

typedef struct {
    int row;
    int col;
    const char *glyph;
    unsigned char attr;
} Overlay;

typedef struct {
    int rows;
    int cols;
    char *cells;
    unsigned char *attrs;
    Overlay overlays[MAX_OVERLAYS];
    size_t overlay_count;
} Screen;

typedef struct {
    int usage_rule;
    int system_rule;
} GraphRows;

typedef struct {
    int row;
    int left;
    int right;
    unsigned char key;
} ClickTarget;

typedef struct {
    bool usage_collapsed;
    bool system_collapsed;
    int selected_gpu;
    pid_t selected_pid;
    bool kill_prompt;
    pid_t prompt_pid;
    char notice[128];
    bool notice_error;
    ClickTarget click_targets[MAX_CLICK_TARGETS];
    size_t click_target_count;
    int network_interface_count;
} UiState;

typedef struct {
    int row;
    bool expanded;
} Marker;

typedef struct {
    char data[INPUT_BUFFER_SIZE];
    size_t length;
} InputBuffer;

static struct termios saved_terminal;
static bool terminal_saved = false;
static bool alternate_screen = false;
static int exit_prompt_row = 24;
static volatile sig_atomic_t stop_requested = 0;
static CpuPrevious cpu_previous[MAX_CPU_PROCESSES];
static size_t cpu_previous_count = 0;
static CpuPrevious cpu_current[MAX_CPU_PROCESSES];
static size_t cpu_current_count = 0;
static NetworkPrevious network_previous[MAX_NETWORK_INTERFACES];
static size_t network_previous_count = 0;

/* ------------------------------------------------------------------------- */
/* Generic helpers                                                           */

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void close_debug_log(void) {
    if (debug_log_file) {
        fclose(debug_log_file);
        debug_log_file = NULL;
    }
}

static void debug_log(const char *format, ...) {
    if (!debug_log_file) return;
    struct timespec ts;
    struct tm local_time;
    char timestamp[32] = "time-unavailable";
    bool have_time = clock_gettime(CLOCK_REALTIME, &ts) == 0;
    if (have_time && localtime_r(&ts.tv_sec, &local_time))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    fprintf(debug_log_file, "%s.%03ld ", timestamp,
            have_time ? ts.tv_nsec / 1000000L : 0L);
    va_list args;
    va_start(args, format);
    vfprintf(debug_log_file, format, args);
    va_end(args);
    fputc('\n', debug_log_file);
    fflush(debug_log_file);
}

static void copy_string(char *dst, size_t size, const char *src) {
    if (size == 0) return;
    if (!src) src = "";
    snprintf(dst, size, "%s", src);
}

static void format_bytes(char *out, size_t size, unsigned long long bytes) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit < ARRAY_LEN(units) - 1) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, size, "%.0f%s", value, units[unit]);
    else snprintf(out, size, "%.1f%s", value, units[unit]);
}

static void format_gib(char *out, size_t size, unsigned long long bytes) {
    snprintf(out, size, "%.1fGiB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

static void format_network_rate(char *out, size_t size, double bps, bool valid) {
    if (!valid) {
        snprintf(out, size, "N/A");
    } else if (bps >= 1000000000.0) {
        snprintf(out, size, "%.1fG", bps / 1000000000.0);
    } else if (bps >= 1000000.0) {
        snprintf(out, size, "%.1fM", bps / 1000000.0);
    } else if (bps >= 1000.0) {
        snprintf(out, size, "%.1fK", bps / 1000.0);
    } else {
        snprintf(out, size, "%.0f", bps);
    }
}

static void format_watts(char *out, size_t size, double value, bool valid) {
    if (!valid) snprintf(out, size, "N/A");
    else snprintf(out, size, "%.1fW", value);
}

static void format_duration(char *out, size_t size, double seconds, bool valid) {
    if (!valid || seconds < 0.0) {
        snprintf(out, size, "--:--:--");
        return;
    }
    unsigned long long total = (unsigned long long)seconds;
    unsigned long long days = total / 86400;
    total %= 86400;
    unsigned long long hours = total / 3600;
    total %= 3600;
    unsigned long long minutes = total / 60;
    unsigned long long secs = total % 60;
    if (days) snprintf(out, size, "%llud%02llu:%02llu", days, hours, minutes);
    else snprintf(out, size, "%02llu:%02llu:%02llu", hours, minutes, secs);
}

static void format_cuda_api(char *out, size_t size, int version) {
    if (version <= 0) snprintf(out, size, "N/A");
    else snprintf(out, size, "%d.%d", version / 1000, (version % 1000) / 10);
}

static void clamp_percent(double *value) {
    if (*value < 0.0) *value = 0.0;
    if (*value > 100.0) *value = 100.0;
}

/* ------------------------------------------------------------------------- */
/* CUDA/NVML information                                                     */

static void detect_cuda_library(char *library, size_t library_size,
                                char *runtime, size_t runtime_size) {
    const char *candidates[] = {
        "/usr/local/cuda/lib64/libcudart.so",
        "/usr/local/cuda/targets/x86_64-linux/lib/libcudart.so",
        "/usr/local/cuda-12.8/lib64/libcudart.so",
        "/usr/local/cuda-12.8/targets/x86_64-linux/lib/libcudart.so",
        "/usr/local/cuda-12/lib64/libcudart.so",
    };
    char resolved[PATH_MAX] = {0};
    library[0] = '\0';
    for (size_t i = 0; i < ARRAY_LEN(candidates); i++) {
        if (!realpath(candidates[i], resolved)) continue;
        const char *needle = strstr(resolved, "libcudart.so.");
        if (needle) {
            needle += strlen("libcudart.so.");
            if (*needle) {
                snprintf(library, library_size, "%s", needle);
                break;
            }
        }
    }
    if (!library[0]) snprintf(library, library_size, "N/A");

    /* The installed libcudart's ABI reports its runtime version through this symbol. */
    runtime[0] = '\0';
    void *handle = dlopen("libcudart.so", RTLD_LAZY | RTLD_LOCAL);
    if (handle) {
        typedef int (*cuda_runtime_version_fn)(int *);
        cuda_runtime_version_fn get_version =
            (cuda_runtime_version_fn)dlsym(handle, "cudaRuntimeGetVersion");
        if (get_version) {
            int version = 0;
            if (get_version(&version) == 0) format_cuda_api(runtime, runtime_size, version);
        }
        dlclose(handle);
    }
    if (!runtime[0]) snprintf(runtime, runtime_size, "N/A");
}

static bool query_process_api(
    nvmlDevice_t device,
    nvmlReturn_t (*function)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *),
    nvmlProcessInfo_t *output,
    unsigned int *output_count
) {
    unsigned int count = 32;
    for (int attempt = 0; attempt < 4; attempt++) {
        nvmlProcessInfo_t *items = calloc(count, sizeof(*items));
        if (!items) return false;
        unsigned int requested = count;
        nvmlReturn_t result = function(device, &requested, items);
        if (result == NVML_SUCCESS) {
            unsigned int copy_count = requested < *output_count ? requested : *output_count;
            memcpy(output, items, copy_count * sizeof(*items));
            *output_count = copy_count;
            free(items);
            return true;
        }
        free(items);
        if (result != NVML_ERROR_INSUFFICIENT_SIZE) return false;
        count = requested + 16;
    }
    return false;
}

static void add_nvml_process(GpuInfo *gpu, const nvmlProcessInfo_t *item) {
    if (item->pid == 0) return;
    for (size_t i = 0; i < gpu->process_count; i++) {
        if (gpu->processes[i].pid == (pid_t)item->pid) {
            if (item->usedGpuMemory != (unsigned long long)NVML_VALUE_NOT_AVAILABLE &&
                (!gpu->processes[i].gpu_memory_valid ||
                 item->usedGpuMemory > gpu->processes[i].gpu_memory)) {
                gpu->processes[i].gpu_memory = item->usedGpuMemory;
                gpu->processes[i].gpu_memory_valid = true;
            }
            return;
        }
    }
    if (gpu->process_count >= MAX_GPU_PROCESSES) return;
    ProcessInfo *process = &gpu->processes[gpu->process_count++];
    memset(process, 0, sizeof(*process));
    process->pid = (pid_t)item->pid;
    if (item->usedGpuMemory != (unsigned long long)NVML_VALUE_NOT_AVAILABLE) {
        process->gpu_memory = item->usedGpuMemory;
        process->gpu_memory_valid = true;
    }
}

static void collect_processes(GpuInfo *gpu) {
    nvmlProcessInfo_t items[1024];
    unsigned int count = ARRAY_LEN(items);
    if (query_process_api(gpu->handle, nvmlDeviceGetComputeRunningProcesses_v3, items, &count)) {
        for (unsigned int i = 0; i < count; i++) add_nvml_process(gpu, &items[i]);
    }
    count = ARRAY_LEN(items);
    if (query_process_api(gpu->handle, nvmlDeviceGetGraphicsRunningProcesses_v3, items, &count)) {
        for (unsigned int i = 0; i < count; i++) add_nvml_process(gpu, &items[i]);
    }
}

static void collect_process_sm(GpuInfo *gpu) {
    nvmlProcessUtilizationSample_t *samples = NULL;
    unsigned int count = 32;
    for (int attempt = 0; attempt < 4; attempt++) {
        samples = calloc(count, sizeof(*samples));
        if (!samples) return;
        unsigned int requested = count;
        nvmlReturn_t result = nvmlDeviceGetProcessUtilization(gpu->handle, samples, &requested, 0);
        if (result == NVML_SUCCESS) {
            for (unsigned int i = 0; i < requested; i++) {
                for (size_t j = 0; j < gpu->process_count; j++) {
                    if (gpu->processes[j].pid == (pid_t)samples[i].pid) {
                        gpu->processes[j].sm = (double)samples[i].smUtil;
                        gpu->processes[j].sm_valid = true;
                    }
                }
            }
            free(samples);
            return;
        }
        free(samples);
        samples = NULL;
        if (result != NVML_ERROR_INSUFFICIENT_SIZE) return;
        count = requested + 16;
    }
}

/* ------------------------------------------------------------------------- */
/* /proc process and system information                                      */

static bool read_cpu_total(unsigned long long *total) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    char line[512];
    bool ok = false;
    if (fgets(line, sizeof(line), file)) {
        unsigned long long values[10] = {0};
        int count = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &values[0], &values[1], &values[2], &values[3], &values[4],
                           &values[5], &values[6], &values[7], &values[8], &values[9]);
        if (count > 0) {
            *total = 0;
            for (int i = 0; i < count; i++) *total += values[i];
            ok = true;
        }
    }
    fclose(file);
    return ok;
}

static bool read_proc_stat(pid_t pid, unsigned long long *process_ticks,
                           unsigned long long *start_ticks) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[8192];
    bool ok = false;
    if (fgets(line, sizeof(line), file)) {
        char *right = strrchr(line, ')');
        if (right && right[1] == ' ') {
            char *cursor = right + 2;
            char *save = NULL;
            char *token = strtok_r(cursor, " ", &save);
            int field = 3; /* first token after comm is field 3 */
            unsigned long long user_ticks = 0, system_ticks = 0, started = 0;
            bool have_user = false, have_system = false, have_started = false;
            while (token) {
                if (field == 14) {
                    user_ticks = strtoull(token, NULL, 10);
                    have_user = true;
                } else if (field == 15) {
                    system_ticks = strtoull(token, NULL, 10);
                    have_system = true;
                } else if (field == 22) {
                    started = strtoull(token, NULL, 10);
                    have_started = true;
                }
                field++;
                token = strtok_r(NULL, " ", &save);
            }
            if (have_user && have_system && have_started) {
                *process_ticks = user_ticks + system_ticks;
                *start_ticks = started;
                ok = true;
            }
        }
    }
    fclose(file);
    return ok;
}

static void read_proc_status(pid_t pid, unsigned int *uid, unsigned long long *rss_kb) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Uid:", 4) == 0) sscanf(line + 4, "%u", uid);
        else if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line + 6, "%llu", rss_kb);
    }
    fclose(file);
}

static void read_proc_command(pid_t pid, char *command, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    FILE *file = fopen(path, "rb");
    size_t length = 0;
    if (file) {
        length = fread(command, 1, size - 1, file);
        fclose(file);
        for (size_t i = 0; i < length; i++) if (command[i] == '\0') command[i] = ' ';
        while (length && command[length - 1] == ' ') length--;
        command[length] = '\0';
    }
    if (!length) {
        snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
        file = fopen(path, "r");
        if (file) {
            if (!fgets(command, (int)size, file)) command[0] = '\0';
            fclose(file);
            command[strcspn(command, "\r\n")] = '\0';
        }
    }
    if (!command[0]) snprintf(command, size, "?");
}

static CpuPrevious *find_previous(pid_t pid) {
    for (size_t i = 0; i < cpu_previous_count; i++) {
        if (cpu_previous[i].pid == pid) return &cpu_previous[i];
    }
    if (cpu_previous_count >= ARRAY_LEN(cpu_previous)) return NULL;
    CpuPrevious *entry = &cpu_previous[cpu_previous_count++];
    memset(entry, 0, sizeof(*entry));
    entry->pid = pid;
    return entry;
}

static CpuPrevious *find_current(pid_t pid) {
    for (size_t i = 0; i < cpu_current_count; i++) {
        if (cpu_current[i].pid == pid) return &cpu_current[i];
    }
    if (cpu_current_count >= ARRAY_LEN(cpu_current)) return NULL;
    CpuPrevious *entry = &cpu_current[cpu_current_count++];
    memset(entry, 0, sizeof(*entry));
    entry->pid = pid;
    return entry;
}

static void fill_process_info(ProcessInfo *process, unsigned long long total_ticks,
                              unsigned long long ram_total, double uptime,
                              long ticks_per_second) {
    unsigned long long process_ticks = 0, start_ticks = 0;
    unsigned int uid = 0;
    unsigned long long rss_kb = 0;
    read_proc_status(process->pid, &uid, &rss_kb);
    read_proc_command(process->pid, process->command, sizeof(process->command));

    struct passwd *password = getpwuid((uid_t)uid);
    copy_string(process->user, sizeof(process->user), password ? password->pw_name : "?");

    if (!read_proc_stat(process->pid, &process_ticks, &start_ticks)) return;
    CpuPrevious *current = find_current(process->pid);
    CpuPrevious *previous = find_previous(process->pid);
    if (current && current->valid) {
        process->cpu = current->value;
        process->cpu_valid = true;
    } else {
        if (previous && previous->valid && total_ticks > previous->total_ticks && process_ticks >= previous->process_ticks) {
            unsigned long long delta_process = process_ticks - previous->process_ticks;
            unsigned long long delta_total = total_ticks - previous->total_ticks;
            if (delta_total) {
                /* Normalize against aggregate CPU ticks: one process stays within 0-100%. */
                process->cpu = 100.0 * (double)delta_process / (double)delta_total;
                process->cpu_valid = true;
            }
        }
        if (current) {
            current->value = process->cpu;
            current->valid = process->cpu_valid;
        }
        if (previous) {
            previous->process_ticks = process_ticks;
            previous->total_ticks = total_ticks;
            previous->valid = true;
        }
    }
    if (ram_total) {
        process->memory = 100.0 * (double)(rss_kb * 1024ULL) / (double)ram_total;
        process->memory_valid = true;
    }
    if (ticks_per_second > 0) {
        process->elapsed = uptime - (double)start_ticks / (double)ticks_per_second;
        process->elapsed_valid = process->elapsed >= 0.0;
    }
}

static bool read_system_info(SystemInfo *system) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return false;
    unsigned long long mem_total_kb = 0, mem_available_kb = 0;
    unsigned long long swap_total_kb = 0, swap_free_kb = 0;
    char key[64];
    unsigned long long value;
    char unit[16];
    while (fscanf(file, "%63s %llu %15s\n", key, &value, unit) >= 2) {
        if (strcmp(key, "MemTotal:") == 0) mem_total_kb = value;
        else if (strcmp(key, "MemAvailable:") == 0) mem_available_kb = value;
        else if (strcmp(key, "SwapTotal:") == 0) swap_total_kb = value;
        else if (strcmp(key, "SwapFree:") == 0) swap_free_kb = value;
    }
    fclose(file);
    if (!mem_total_kb) return false;
    system->ram_total = mem_total_kb * 1024ULL;
    unsigned long long available = mem_available_kb * 1024ULL;
    system->ram_used = system->ram_total > available ? system->ram_total - available : 0;
    system->ram_percent = 100.0 * (double)system->ram_used / (double)system->ram_total;
    system->swap_total = swap_total_kb * 1024ULL;
    unsigned long long swap_free = swap_free_kb * 1024ULL;
    system->swap_used = system->swap_total > swap_free ? system->swap_total - swap_free : 0;
    system->swap_percent = system->swap_total ? 100.0 * (double)system->swap_used / (double)system->swap_total : 0.0;
    return true;
}

static NetworkPrevious *find_network_previous(const char *name) {
    for (size_t i = 0; i < network_previous_count; i++) {
        if (strcmp(network_previous[i].name, name) == 0) return &network_previous[i];
    }
    if (network_previous_count >= ARRAY_LEN(network_previous)) return NULL;
    NetworkPrevious *previous = &network_previous[network_previous_count++];
    memset(previous, 0, sizeof(*previous));
    snprintf(previous->name, sizeof(previous->name), "%s", name);
    return previous;
}

static bool ignored_network_interface(const char *name) {
    return strcmp(name, "lo") == 0 ||
           strncmp(name, "docker", 6) == 0 ||
           strncmp(name, "br-", 3) == 0 ||
           strncmp(name, "veth", 4) == 0;
}

static bool read_network_capacity(const char *name, double *capacity_bps) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "/sys/class/net/%s/speed", name);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;

    FILE *file = fopen(path, "r");
    if (!file) return false;

    long long speed_mbps = -1;
    bool valid = fscanf(file, "%lld", &speed_mbps) == 1 && speed_mbps > 0;
    fclose(file);
    if (!valid) return false;

    *capacity_bps = (double)speed_mbps * 1000000.0;
    return true;
}

static bool read_network_info(NetworkInfo *network) {
    memset(network, 0, sizeof(*network));
    FILE *file = fopen("/proc/net/dev", "r");
    if (!file) return false;

    double timestamp = monotonic_seconds();
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char interface[64];
        unsigned long long rx_bytes = 0, tx_bytes = 0;
        int fields = sscanf(line,
                            " %63[^:]: %llu %*s %*s %*s %*s %*s %*s %llu",
                            interface, &rx_bytes, &tx_bytes);
        if (fields != 3 || ignored_network_interface(interface)) continue;
        if (network->interface_count >= MAX_NETWORK_INTERFACES) continue;

        NetworkInterfaceInfo *current = &network->interfaces[network->interface_count++];
        snprintf(current->name, sizeof(current->name), "%s", interface);
        current->capacity_valid = read_network_capacity(interface, &current->capacity_bps);
        NetworkPrevious *previous = find_network_previous(interface);
        if (previous && previous->valid && timestamp > previous->timestamp &&
            rx_bytes >= previous->rx_bytes && tx_bytes >= previous->tx_bytes) {
            double elapsed = timestamp - previous->timestamp;
            current->rx_bps = (double)(rx_bytes - previous->rx_bytes) * 8.0 / elapsed;
            current->tx_bps = (double)(tx_bytes - previous->tx_bytes) * 8.0 / elapsed;
            current->total_bps = current->rx_bps + current->tx_bps;
            if (current->capacity_valid) {
                current->utilization_percent =
                    100.0 * current->total_bps / current->capacity_bps;
                current->utilization_valid = true;
            }
            current->valid = true;
        }
        if (previous) {
            previous->rx_bytes = rx_bytes;
            previous->tx_bytes = tx_bytes;
            previous->timestamp = timestamp;
            previous->valid = true;
        }
    }
    fclose(file);
    if (!network->interface_count) return false;

    bool has_valid_interface = false;
    bool all_valid_interfaces_have_capacity = true;
    for (int i = 0; i < network->interface_count; i++) {
        const NetworkInterfaceInfo *current = &network->interfaces[i];
        if (current->capacity_valid) network->capacity_bps += current->capacity_bps;
        if (!current->valid) continue;
        has_valid_interface = true;
        network->rx_bps += current->rx_bps;
        network->tx_bps += current->tx_bps;
        if (!current->capacity_valid) all_valid_interfaces_have_capacity = false;
    }
    network->total_bps = network->rx_bps + network->tx_bps;
    network->utilization_valid = has_valid_interface &&
                                 network->capacity_bps > 0.0 &&
                                 all_valid_interfaces_have_capacity;
    network->utilization_percent = network->utilization_valid
                                       ? 100.0 * network->total_bps / network->capacity_bps
                                       : 0.0;
    network->valid = has_valid_interface;
    return true;
}

static double read_uptime(void) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) return 0.0;
    double uptime = 0.0;
    if (fscanf(file, "%lf", &uptime) != 1) uptime = 0.0;
    fclose(file);
    return uptime;
}

/* ------------------------------------------------------------------------- */
/* History and log-time compression                                          */

static bool history_init(History *history) {
    history->data = calloc(HISTORY_LIMIT, sizeof(*history->data));
    if (!history->data) return false;
    history->capacity = HISTORY_LIMIT;
    history->count = 0;
    history->start = 0;
    return true;
}

static void history_free(History *history) {
    free(history->data);
    memset(history, 0, sizeof(*history));
}

static void history_clear(History *history) {
    history->count = 0;
    history->start = 0;
}

static void history_add(History *history, double timestamp, double value) {
    if (!history->data || !history->capacity) return;
    size_t position;
    if (history->count < history->capacity) {
        position = (history->start + history->count) % history->capacity;
        history->count++;
    } else {
        position = history->start;
        history->start = (history->start + 1) % history->capacity;
    }
    history->data[position] = (Sample){timestamp, value};
}

static Sample history_at(const History *history, size_t index) {
    return history->data[(history->start + index) % history->capacity];
}

static void compress_history(
    const History *history,
    int plot_width,
    double *values,
    bool *valid,
    int *label_positions,
    char labels[][16],
    int *label_count
) {
    const double period_weights[] = {30.0, 10.0, 3.0, 1.0}; /* oldest -> newest */
    const double history_window = 400.0;
    double periods[4];
    int widths[4];
    int base = plot_width / 4;
    int remainder = plot_width % 4;
    double weighted_seconds = 0.0;
    for (int i = 0; i < 4; i++) {
        widths[i] = base + (i < remainder ? 1 : 0);
        weighted_seconds += period_weights[i] * widths[i];
    }
    for (int i = 0; i < 4; i++)
        periods[i] = period_weights[i] * history_window / weighted_seconds;
    double total_seconds = history_window;
    for (int i = 0; i < plot_width; i++) {
        values[i] = 0.0;
        valid[i] = false;
    }

    *label_count = 0;
    label_positions[(*label_count)] = 0;
    snprintf(labels[(*label_count)++], 16, "-%lds", (long)llround(total_seconds));
    double consumed = 0.0;
    int cell_position = 0;
    double reference = history->count ? history_at(history, history->count - 1).timestamp : 0.0;
    for (int segment = 0; segment < 4; segment++) {
        double period = periods[segment];
        for (int cell = 0; cell < widths[segment]; cell++, cell_position++) {
            double upper_age = total_seconds - consumed - period * cell;
            double lower_age = fmax(0.0, upper_age - period);
            for (size_t i = 0; i < history->count; i++) {
                Sample sample = history_at(history, i);
                double age = reference - sample.timestamp;
                if (age >= lower_age && age < upper_age &&
                    (!valid[cell_position] || sample.value > values[cell_position])) {
                    values[cell_position] = sample.value;
                    valid[cell_position] = true;
                }
            }
        }
        consumed += period * widths[segment];
        if (segment < 3) {
            label_positions[*label_count] = cell_position;
            double age = fmax(0.0, total_seconds - consumed);
            int age_seconds = (int)llround(age);
            if (age_seconds > 9999) age_seconds = 9999;
            snprintf(labels[*label_count], 16, "-%ds", age_seconds);
            (*label_count)++;
        }
    }
    /* Sampling intervals and bucket boundaries do not always line up.
       Hold the last known value across internal empty buckets so a live
       history does not develop artificial holes between real samples. */
    int first_valid = -1;
    for (int i = 0; i < plot_width; i++) {
        if (valid[i]) {
            first_valid = i;
            break;
        }
    }
    if (first_valid >= 0) {
        double last_value = values[first_valid];
        for (int i = first_valid + 1; i < plot_width; i++) {
            if (valid[i]) last_value = values[i];
            else {
                values[i] = last_value;
                valid[i] = true;
            }
        }
    }
    label_positions[*label_count] = plot_width;
    snprintf(labels[*label_count], 16, "now");
    (*label_count)++;
}

/* ------------------------------------------------------------------------- */
/* Fixed-cell ANSI screen                                                    */

static const char *ansi_for_attr(unsigned char attr) {
    switch (attr) {
        case ATTR_DIM: return "\033[2m";
        case ATTR_CYAN: return "\033[36m";
        case ATTR_CYAN_BOLD: return "\033[1;36m";
        case ATTR_GREEN: return "\033[32m";
        case ATTR_TEXT_GREEN: return "\033[1;32m";
        case ATTR_YELLOW: return "\033[33m";
        case ATTR_RED: return "\033[1;31m";
        case ATTR_SELECTED: return "\033[7;1;36m";
        case ATTR_PROMPT: return "\033[7;1;33m";
        default: return "\033[0m";
    }
}

static unsigned char load_attr(double value) {
    clamp_percent(&value);
    if (value >= 95.0) return ATTR_RED;
    if (value >= 80.0) return ATTR_YELLOW;
    return ATTR_GREEN;
}

static unsigned char threshold_attr_value(double value, bool valid, double warning, double critical) {
    if (!valid) return ATTR_DIM;
    if (value >= critical) return ATTR_RED;
    if (value >= warning) return ATTR_YELLOW;
    return ATTR_TEXT_GREEN;
}

static unsigned char text_load_attr(double value) {
    clamp_percent(&value);
    if (value >= 95.0) return ATTR_RED;
    if (value >= 80.0) return ATTR_YELLOW;
    return ATTR_TEXT_GREEN;
}

static bool screen_init(Screen *screen, int rows, int cols) {
    size_t size = (size_t)rows * (size_t)cols;
    screen->rows = rows;
    screen->cols = cols;
    screen->cells = malloc(size);
    screen->attrs = malloc(size);
    if (!screen->cells || !screen->attrs) {
        free(screen->cells);
        free(screen->attrs);
        memset(screen, 0, sizeof(*screen));
        return false;
    }
    return true;
}

static bool screen_resize(Screen *screen, int rows, int cols) {
    if (rows == screen->rows && cols == screen->cols && screen->cells && screen->attrs) return true;
    size_t size = (size_t)rows * (size_t)cols;
    char *new_cells = malloc(size);
    unsigned char *new_attrs = malloc(size);
    if (!new_cells || !new_attrs) {
        free(new_cells);
        free(new_attrs);
        return false;
    }
    free(screen->cells);
    free(screen->attrs);
    screen->cells = new_cells;
    screen->attrs = new_attrs;
    screen->rows = rows;
    screen->cols = cols;
    return true;
}

static void screen_free(Screen *screen) {
    free(screen->cells);
    free(screen->attrs);
    memset(screen, 0, sizeof(*screen));
}

static void screen_clear(Screen *screen) {
    size_t size = (size_t)screen->rows * (size_t)screen->cols;
    memset(screen->cells, ' ', size);
    memset(screen->attrs, ATTR_NORMAL, size);
    screen->overlay_count = 0;
}

static void screen_overlay(Screen *screen, int row, int col, const char *glyph, unsigned char attr) {
    if (screen->overlay_count >= MAX_OVERLAYS) return;
    if (row < 0 || row >= screen->rows || col < 0 || col >= screen->cols) return;
    screen->overlays[screen->overlay_count++] = (Overlay){row, col, glyph, attr};
}

static void screen_ch_attr(Screen *screen, int row, int col, char value, unsigned char attr) {
    if (row < 0 || row >= screen->rows || col < 0 || col >= screen->cols) return;
    size_t index = (size_t)row * (size_t)screen->cols + (size_t)col;
    screen->cells[index] = value;
    screen->attrs[index] = attr;
}

static void screen_ch(Screen *screen, int row, int col, char value) {
    screen_ch_attr(screen, row, col, value, ATTR_NORMAL);
}

static void screen_put_attr(Screen *screen, int row, int col, const char *text,
                            int width, unsigned char attr) {
    if (row < 0 || row >= screen->rows || col >= screen->cols || width <= 0) return;
    if (col < 0) {
        text += -col;
        width += col;
        col = 0;
    }
    if (width > screen->cols - col) width = screen->cols - col;
    for (int i = 0; i < width && text[i]; i++) {
        screen_ch_attr(screen, row, col + i, text[i], attr);
    }
}

static void screen_put(Screen *screen, int row, int col, const char *text, int width) {
    screen_put_attr(screen, row, col, text, width, ATTR_NORMAL);
}

static void screen_putf_attr(Screen *screen, int row, int col, int width,
                             unsigned char attr, const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    screen_put_attr(screen, row, col, text, width, attr);
}

static void screen_putf(Screen *screen, int row, int col, int width, const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    screen_put(screen, row, col, text, width);
}

static void screen_flush(const Screen *screen, const Marker *markers, size_t marker_count) {
    debug_log("frame begin rows=%d cols=%d markers=%zu", screen->rows, screen->cols, marker_count);
    /* Compose the complete frame off-screen first. */
    char *frame = NULL;
    size_t frame_length = 0;
    FILE *stream = open_memstream(&frame, &frame_length);
    if (!stream) {
        debug_log("frame composition failed: %s", strerror(errno));
        return;
    }
    fputs("\033[?2026h\033[?25l", stream);
    const Overlay **overlay_at = calloc((size_t)screen->cols, sizeof(*overlay_at));
    if (!overlay_at) {
        debug_log("frame composition failed: overlay allocation (%s)", strerror(errno));
        fclose(stream);
        free(frame);
        return;
    }
    for (int row = 0; row < screen->rows; row++) {
        memset(overlay_at, 0, (size_t)screen->cols * sizeof(*overlay_at));
        for (size_t i = 0; i < screen->overlay_count; i++) {
            const Overlay *overlay = &screen->overlays[i];
            if (overlay->row == row && overlay->col < screen->cols)
                overlay_at[overlay->col] = overlay;
        }

        fprintf(stream, "\033[%d;1H", row + 1);
        unsigned char current_attr = 255;
        for (int col = 0; col < screen->cols; col++) {
            size_t index = (size_t)row * (size_t)screen->cols + (size_t)col;
            const Overlay *overlay = overlay_at[col];
            unsigned char attr = overlay ? overlay->attr : screen->attrs[index];
            if (color_enabled && attr != current_attr) {
                fputs(ansi_for_attr(attr), stream);
                current_attr = attr;
            }
            if (overlay) fputs(overlay->glyph, stream);
            else fputc(screen->cells[index], stream);
        }
        if (color_enabled) fputs("\033[0m", stream);
        fputs("\033[K", stream);
    }
    free(overlay_at);
    for (size_t i = 0; i < marker_count; i++) {
        if (color_enabled) fputs("\033[1;36m", stream);
        fprintf(stream, "\033[%d;%dH%s", markers[i].row + 1, screen->cols - 1,
               markers[i].expanded ? "\xE2\x96\xBC" : "\xE2\x96\xB6");
        if (color_enabled) fputs("\033[0m", stream);
    }
    fputs("\033[?2026l", stream);
    fclose(stream);

    size_t written = 0;
    debug_log("frame write begin bytes=%zu", frame_length);
    while (written < frame_length) {
        ssize_t count = write(STDOUT_FILENO, frame + written, frame_length - written);
        if (count > 0) written += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else {
            int write_error = errno;
            debug_log("frame write stopped after %zu/%zu bytes (errno=%d: %s)",
                      written, frame_length, write_error, strerror(write_error));
            break;
        }
    }
    debug_log("frame write done bytes=%zu/%zu", written, frame_length);
    free(frame);
}

static void draw_rule(Screen *screen, int row, const char *title,
                      bool graph, bool expanded, Marker *markers, size_t *marker_count) {
    if (row < 0 || row >= screen->rows) return;
    screen_ch(screen, row, 0, '+');
    for (int col = 1; col < screen->cols - 1; col++) screen_ch(screen, row, col, '-');
    if (screen->cols > 1) screen_ch(screen, row, screen->cols - 1, '+');
    char title_text[256];
    snprintf(title_text, sizeof(title_text), "[ %s ]", title);
    screen_put_attr(screen, row, 2, title_text, screen->cols - 4, ATTR_CYAN_BOLD);
    if (graph && marker_count && *marker_count < 4) {
        markers[*marker_count] = (Marker){row, expanded};
        (*marker_count)++;
    }
}

static void draw_plot_graph(Screen *screen, int top, int left, int width,
                            const History *history, int graph_rows) {
    if (width < 12 || graph_rows <= 0) return;
    int label_width = 4;
    int axis_col = left + label_width;
    int plot_left = axis_col + 1;
    int plot_width = width - label_width - 1;
    double values[MAX_SCREEN_COLS];
    bool valid[MAX_SCREEN_COLS];
    int label_positions[8];
    char labels[8][16];
    int label_count = 0;
    if (plot_width > MAX_SCREEN_COLS) plot_width = MAX_SCREEN_COLS;
    compress_history(history, plot_width, values, valid, label_positions, labels, &label_count);

    for (int row = 0; row < graph_rows; row++) {
        int level = graph_rows == 1
                        ? 0
                        : (int)llround(100.0 * (double)(graph_rows - 1 - row) /
                                      (double)(graph_rows - 1));
        screen_putf_attr(screen, top + row, left, 4, ATTR_DIM, "%3d ", level);
        screen_ch_attr(screen, top + row, axis_col, '|', ATTR_DIM);
    }
    int bottom_row = top + graph_rows - 1;
    for (int x = 0; x < plot_width; x++) {
        screen_overlay(screen, bottom_row, plot_left + x, "-", ATTR_DIM);
        if (!valid[x]) continue;

        double value = values[x];
        clamp_percent(&value);
        double level = value / 100.0 * graph_rows;
        int full_rows = (int)floor(level);
        double fraction = level - full_rows;
        if (full_rows > graph_rows) full_rows = graph_rows;
        unsigned char attr = load_attr(value);

        for (int filled = 0; filled < full_rows; filled++)
            screen_overlay(screen, bottom_row - filled, plot_left + x, "█", attr);
        if (full_rows < graph_rows) {
            int glyph_index = (int)llround(fraction * 8.0);
            if (glyph_index > 0)
                screen_overlay(screen, bottom_row - full_rows, plot_left + x,
                               BLOCK_GLYPHS[glyph_index], attr);
        }
    }
    int footer = top + graph_rows;
    int last_label = label_count - 1;
    int now_x = plot_left + plot_width - (int)strlen(labels[last_label]);
    if (now_x < plot_left) now_x = plot_left;
    int previous_end = plot_left - 1;
    for (int i = 0; i < label_count; i++) {
        int label_length = (int)strlen(labels[i]);
        int x = i == last_label
                    ? now_x
                    : plot_left + label_positions[i];
        if (x < previous_end + 2) x = previous_end + 2;
        if (i != last_label && x + label_length > now_x - 1) continue;
        screen_put_attr(screen, footer, x, labels[i], screen->cols - x, ATTR_DIM);
        previous_end = x + label_length - 1;
    }
}

static void draw_plot(Screen *screen, int top, int left, int width,
                      const History *history, const char *title,
                      const char *title_info) {
    if (width < 12) {
        screen_put_attr(screen, top, left, title, width, ATTR_CYAN_BOLD);
        screen_put_attr(screen, top + 1, left, "terminal too narrow", width, ATTR_DIM);
        return;
    }
    screen_put_attr(screen, top, left, title, width, ATTR_CYAN_BOLD);
    if (title_info && title[0]) {
        int title_length = (int)strlen(title);
        if (title_length + 1 < width)
            screen_put_attr(screen, top, left + title_length + 1, title_info,
                            width - title_length - 1, ATTR_TEXT_GREEN);
    }
    draw_plot_graph(screen, top + 1, left, width, history, GRAPH_ROWS);
}

static void draw_load_bar(Screen *screen, int row, int start, double memory, double gpu) {
    int width = screen->cols - start;
    if (width < 8) return;
    int inner = width - 3;  /* outer bars and the center '+' */
    int left_width = inner / 2;
    int right_width = inner - left_width;
    clamp_percent(&memory);
    clamp_percent(&gpu);
    int memory_cells = (int)((double)left_width * memory / 100.0);
    int gpu_cells = (int)((double)right_width * gpu / 100.0);
    if (memory > 0.0 && memory_cells == 0) memory_cells = 1;
    if (gpu > 0.0 && gpu_cells == 0) gpu_cells = 1;
    if (memory < 100.0 && memory_cells >= left_width) memory_cells = left_width - 1;
    if (gpu < 100.0 && gpu_cells >= right_width) gpu_cells = right_width - 1;
    unsigned char memory_attr = load_attr(memory);
    unsigned char gpu_attr = load_attr(gpu);

    screen_ch_attr(screen, row, start, '|', ATTR_DIM);
    screen_ch_attr(screen, row, screen->cols - 1, '|', ATTR_DIM);
    int divider = start + 1 + left_width;
    if (memory_cells == 0 && gpu_cells == 0) {
        screen_ch_attr(screen, row, divider - 1, '<', ATTR_DIM);
        screen_ch_attr(screen, row, divider, '+', ATTR_DIM);
        screen_ch_attr(screen, row, divider + 1, '>', ATTR_DIM);
        return;
    }

    for (int i = 0; i < left_width; i++) {
        int first_filled = left_width - memory_cells;
        char cell = i < first_filled ? ' ' : i == first_filled ? '<' : '-';
        unsigned char attr = i < first_filled ? ATTR_NORMAL : memory_attr;
        screen_ch_attr(screen, row, start + 1 + i, cell, attr);
    }
    screen_ch_attr(screen, row, divider, '+', ATTR_DIM);
    for (int i = 0; i < right_width; i++) {
        char cell = i < gpu_cells - 1 ? '-' : i == gpu_cells - 1 ? '>' : ' ';
        unsigned char attr = i < gpu_cells ? gpu_attr : ATTR_NORMAL;
        screen_ch_attr(screen, row, divider + 1 + i, cell, attr);
    }
}

/* ------------------------------------------------------------------------- */
/* GPU and process rendering                                                 */

static void add_click_target(UiState *ui, int row, int left, int right, unsigned char key);

static void draw_gpu_status(Screen *screen, int top, const GpuInfo *gpus, int gpu_count) {
    const int bar_start = 86;
    screen_put_attr(screen, top, 0, "GPU", 5, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 5, "NAME", 20, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 27, "TEMP", 5, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 34, "POWER DRAW/LIMIT", 16, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 52, "VRAM USED/TOTAL", 22, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 76, "GPU", 8, ATTR_CYAN_BOLD);
    if (screen->cols - bar_start >= 8)
        screen_put_attr(screen, top, bar_start, "LOAD: VRAM <-  +  -> GPU", screen->cols - bar_start, ATTR_CYAN_BOLD);

    for (int i = 0; i < gpu_count; i++) {
        const GpuInfo *gpu = &gpus[i];
        int row = top + 1 + i;
        screen_putf_attr(screen, row, 0, 4, ATTR_CYAN, "%3d", gpu->index);
        screen_put(screen, row, 5, gpu->name, 20);
        unsigned char temperature_attr = threshold_attr_value(gpu->temperature, gpu->temperature_valid, 70.0, 85.0);
        if (gpu->temperature_valid) screen_putf_attr(screen, row, 27, 5, temperature_attr, "%.0fC", gpu->temperature);
        else screen_put_attr(screen, row, 27, "N/A", 5, ATTR_DIM);
        char power[64], used[32], total[32];
        format_watts(power, sizeof(power), gpu->power, gpu->power_valid);
        char limit[32];
        format_watts(limit, sizeof(limit), gpu->power_limit, gpu->power_limit_valid);
        double power_ratio = (gpu->power_valid && gpu->power_limit_valid && gpu->power_limit > 0.0)
                                  ? 100.0 * gpu->power / gpu->power_limit : 0.0;
        unsigned char power_attr = threshold_attr_value(power_ratio,
                                                         gpu->power_valid && gpu->power_limit_valid,
                                                         90.0, 99.0);
        screen_putf_attr(screen, row, 34, 16, power_attr, "%s/%s", power, limit);
        if (gpu->memory_valid) {
            format_gib(used, sizeof(used), gpu->memory_used);
            format_gib(total, sizeof(total), gpu->memory_total);
            double memory = 100.0 * (double)gpu->memory_used / (double)gpu->memory_total;
            screen_putf_attr(screen, row, 52, 22, text_load_attr(memory), "%s/%s %3.0f%%", used, total, memory);
        } else screen_put_attr(screen, row, 52, "N/A", 22, ATTR_DIM);
        double gpu_util = gpu->utilization_valid ? gpu->utilization : 0.0;
        if (gpu->utilization_valid) screen_putf_attr(screen, row, 76, 8, text_load_attr(gpu_util), "%.1f%%", gpu_util);
        else screen_put_attr(screen, row, 76, "N/A", 8, ATTR_DIM);
        double memory = gpu->memory_valid
                            ? 100.0 * (double)gpu->memory_used / (double)gpu->memory_total
                            : 0.0;
        draw_load_bar(screen, row, bar_start, memory, gpu_util);
    }
}

static void draw_processes(Screen *screen, int top, const GpuInfo *gpus, int gpu_count,
                            UiState *ui) {
    screen_put_attr(screen, top, 0, "GPU", 7, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 7, "PID", 8, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 15, "USER", 10, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 25, "GPU-MEM", 11, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 36, "%GPU-LOAD", 10, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 46, "CPU AVG", 8, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 54, "%MEM", 7, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 61, "TIME", 10, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 71, "COMMAND", screen->cols - 71, ATTR_CYAN_BOLD);

    int row = top + 1;
    for (int i = 0; i < gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            if (row >= screen->rows) return;
            const ProcessInfo *p = &gpus[i].processes[j];
            char memory[32], elapsed[32];
            if (p->gpu_memory_valid) format_bytes(memory, sizeof(memory), p->gpu_memory);
            else snprintf(memory, sizeof(memory), "N/A");
            format_duration(elapsed, sizeof(elapsed), p->elapsed, p->elapsed_valid);
            screen_putf_attr(screen, row, 0, 7, ATTR_CYAN, "%3d", gpus[i].index);
            screen_putf(screen, row, 7, 8, "%-7ld", (long)p->pid);
            screen_put(screen, row, 15, p->user, 10);
            screen_put(screen, row, 25, memory, 9);
            if (p->sm_valid) screen_putf_attr(screen, row, 36, 5, text_load_attr(p->sm), "%-5.1f", p->sm);
            else screen_put_attr(screen, row, 36, "--", 5, ATTR_DIM);
            if (p->cpu_valid) screen_putf(screen, row, 46, 5, "%-5.1f", p->cpu);
            else screen_put_attr(screen, row, 46, "--", 5, ATTR_DIM);
            if (p->memory_valid) screen_putf(screen, row, 54, 5, "%-5.1f", p->memory);
            else screen_put_attr(screen, row, 54, "--", 5, ATTR_DIM);
            screen_put(screen, row, 61, elapsed, 10);
            screen_put(screen, row, 71, p->command, screen->cols - 71);
            bool selected = ui && ui->selected_gpu == gpus[i].index && ui->selected_pid == p->pid;
            if (selected) {
                for (int col = 0; col < screen->cols; col++)
                    screen->attrs[(size_t)row * (size_t)screen->cols + (size_t)col] = ATTR_SELECTED;
                if (ui->kill_prompt && ui->prompt_pid == p->pid) {
                    const char prompt[] = "[ Kill? Y/N ]";
                    int prompt_length = (int)strlen(prompt);
                    int prompt_col = screen->cols - prompt_length;
                    screen_put_attr(screen, row, prompt_col, prompt, prompt_length, ATTR_PROMPT);
                    add_click_target(ui, row, prompt_col + 8, prompt_col + 9, 'y');
                    add_click_target(ui, row, prompt_col + 10, prompt_col + 11, 'n');
                }
            }
            row++;
        }
    }
    if (row == top + 1 && row < screen->rows)
        screen_put_attr(screen, row, 0, "No GPU processes reported by NVML.", screen->cols, ATTR_DIM);
}

/* ------------------------------------------------------------------------- */
/* Layout and rendering                                                      */

static int system_content_rows(int interface_count, bool collapsed);

static GraphRows graph_rows(int gpu_count, const UiState *ui) {
    int y = 3;
    y += 1;                 /* GPU section rule */
    y += 1 + gpu_count;     /* GPU header and rows */
    GraphRows rows = {.usage_rule = y, .system_rule = 0};
    y += 1;                 /* usage section rule */
    y += ui->usage_collapsed ? 1 : 2 + GRAPH_ROWS;
    rows.system_rule = y;
    return rows;
}

static int process_table_top(int gpu_count, const UiState *ui) {
    GraphRows rows = graph_rows(gpu_count, ui);
    int y = rows.system_rule + 1;
    y += system_content_rows(ui->network_interface_count, ui->system_collapsed);
    y += 1;                 /* process section rule, then table header */
    return y;
}

static bool process_at_screen_row(int y, int gpu_count, const UiState *ui,
                                  const GpuInfo *gpus, int *gpu_index, size_t *process_index) {
    int row = y - process_table_top(gpu_count, ui) - 1;
    if (row < 0) return false;
    for (int i = 0; i < gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            if (row == 0) {
                if (gpu_index) *gpu_index = i;
                if (process_index) *process_index = j;
                return true;
            }
            row--;
        }
    }
    return false;
}

static void select_process_at(UiState *ui, int y, int gpu_count, const GpuInfo *gpus) {
    int gpu_index = -1;
    size_t process_index = 0;
    if (process_at_screen_row(y, gpu_count, ui, gpus, &gpu_index, &process_index)) {
        ui->selected_gpu = gpus[gpu_index].index;
        ui->selected_pid = gpus[gpu_index].processes[process_index].pid;
        ui->notice[0] = '\0';
        return;
    }
    ui->selected_gpu = -1;
    ui->selected_pid = 0;
}

static void move_process_selection(UiState *ui, const GpuInfo *gpus, int gpu_count, int direction) {
    int total = 0;
    int current = -1;
    for (int i = 0; i < gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            if (gpus[i].index == ui->selected_gpu && gpus[i].processes[j].pid == ui->selected_pid)
                current = total;
            total++;
        }
    }
    if (total == 0) {
        snprintf(ui->notice, sizeof(ui->notice), "No GPU process is available");
        ui->notice_error = true;
        return;
    }
    int target = current < 0 ? (direction > 0 ? 0 : total - 1) : current + direction;
    if (target < 0) target = 0;
    if (target >= total) target = total - 1;
    int index = 0;
    for (int i = 0; i < gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            if (index++ == target) {
                ui->selected_gpu = gpus[i].index;
                ui->selected_pid = gpus[i].processes[j].pid;
                ui->notice[0] = '\0';
                return;
            }
        }
    }
}

static int network_line_count(const NetworkInfo *network) {
    return network->interface_count > 0 ? network->interface_count : 1;
}

static int network_graph_rows(int line_count) {
    int rows = GRAPH_ROWS - (line_count - 1);
    return rows > 0 ? rows : 0;
}

static int system_content_rows(int interface_count, bool collapsed) {
    int line_count = interface_count > 0 ? interface_count : 1;
    if (collapsed) return line_count;
    int graph_rows = network_graph_rows(line_count);
    int network_rows = line_count + (graph_rows > 0 ? 1 : 0);
    int memory_rows = 2 + GRAPH_ROWS;
    return network_rows > memory_rows ? network_rows : memory_rows;
}

static void format_network_capacity(char *out, size_t size, double capacity_bps, bool valid) {
    if (!valid) snprintf(out, size, "N/A");
    else snprintf(out, size, "%.0fMbps", capacity_bps / 1000000.0);
}

static void format_network_line(const NetworkInterfaceInfo *interface,
                                bool include_name, char *line, size_t line_size,
                                int *down_offset, int *up_offset) {
    char capacity[32], load[32], total[32], rx[32], tx[32];
    bool valid = interface && interface->valid;
    bool capacity_valid = interface && interface->capacity_valid;
    bool utilization_valid = interface && interface->utilization_valid;
    format_network_capacity(capacity, sizeof(capacity),
                            interface ? interface->capacity_bps : 0.0,
                            capacity_valid);
    if (utilization_valid)
        snprintf(load, sizeof(load), "%.1f%%", interface->utilization_percent);
    else
        snprintf(load, sizeof(load), "N/A");
    format_network_rate(total, sizeof(total), interface ? interface->total_bps : 0.0, valid);
    format_network_rate(rx, sizeof(rx), interface ? interface->rx_bps : 0.0, valid);
    format_network_rate(tx, sizeof(tx), interface ? interface->tx_bps : 0.0, valid);
    const char *name = interface && interface->name[0] ? interface->name : "N/A";
    char prefix[96];
    if (include_name) snprintf(prefix, sizeof(prefix), "NETWORK(%s)", name);
    else snprintf(prefix, sizeof(prefix), "NETWORK");
    snprintf(line, line_size, "%s | %s(%s) | TOTAL:%s | d:%s | u:%s",
             prefix, capacity, load, total, rx, tx);
    char *down = strstr(line, "d:");
    char *up = strstr(line, "u:");
    *down_offset = down ? (int)(down - line) : -1;
    *up_offset = up ? (int)(up - line) : -1;
}

static void draw_network_arrows(Screen *screen, int row, int left, int width,
                                int down_offset, int up_offset, unsigned char attr) {
    if (down_offset >= 0 && left + down_offset < left + width)
        screen_overlay(screen, row, left + down_offset, "\xE2\x96\xBC", attr);
    if (up_offset >= 0 && left + up_offset < left + width)
        screen_overlay(screen, row, left + up_offset, "\xE2\x96\xB2", attr);
}

static void draw_network_line(Screen *screen, int row, int left, int width,
                              const NetworkInterfaceInfo *interface,
                              bool include_name) {
    char line[512];
    int down_offset, up_offset;
    format_network_line(interface, include_name, line, sizeof(line), &down_offset, &up_offset);
    bool valid = interface && interface->valid;
    unsigned char info_attr = valid ? ATTR_TEXT_GREEN : ATTR_DIM;
    screen_put_attr(screen, row, left, line, width, info_attr);
    char *separator = strstr(line, " | TOTAL:");
    int cyan_length = separator ? (int)(separator - line) : (int)strlen(line);
    screen_put_attr(screen, row, left, line, cyan_length, ATTR_CYAN_BOLD);
    draw_network_arrows(screen, row, left, width, down_offset, up_offset, info_attr);
}

static void draw_network_graph(Screen *screen, int top, int left, int width,
                               const History *history, int graph_rows) {
    draw_plot_graph(screen, top, left, width, history, graph_rows);
}

static void draw_system(Screen *screen, int top, const SystemInfo *system,
                        const History *ram, const History *swap,
                        const History *network_history,
                        const NetworkInfo *network, bool collapsed) {
    char ram_title[32], swap_title[32], ram_info[64], swap_info[64];
    snprintf(ram_title, sizeof(ram_title), "RAM(%.0f%%)", system->ram_percent);
    snprintf(ram_info, sizeof(ram_info), "%.1fG/%.1fG",
             (double)system->ram_used / (1024.0 * 1024.0 * 1024.0),
             (double)system->ram_total / (1024.0 * 1024.0 * 1024.0));
    snprintf(swap_title, sizeof(swap_title), "SWAP(%.0f%%)", system->swap_percent);
    snprintf(swap_info, sizeof(swap_info), "%.1fG/%.1fG",
             (double)system->swap_used / (1024.0 * 1024.0 * 1024.0),
             (double)system->swap_total / (1024.0 * 1024.0 * 1024.0));

    int gap = 2;
    int available = screen->cols - 2 * gap;
    int network_width = available * 57 / 100;
    if (network_width < 12) network_width = 12;
    if (network_width > available - 2) network_width = available - 2;
    int memory_width = available - network_width;
    int left_width = memory_width / 2;
    int middle_width = memory_width - left_width;
    int middle_left = left_width + gap;
    int right_left = middle_left + middle_width + gap;
    int right_width = screen->cols - right_left;
    int line_count = network_line_count(network);
    bool include_name = line_count > 1;

    if (collapsed) {
        char ram_summary[128], swap_summary[128];
        snprintf(ram_summary, sizeof(ram_summary), "%s %s", ram_title, ram_info);
        snprintf(swap_summary, sizeof(swap_summary), "%s %s", swap_title, swap_info);
        screen_put_attr(screen, top, 0, ram_summary, left_width, text_load_attr(system->ram_percent));
        screen_put_attr(screen, top, middle_left, swap_summary, middle_width, text_load_attr(system->swap_percent));
        for (int i = 0; i < line_count; i++) {
            const NetworkInterfaceInfo *interface = network->interface_count > 0
                                                         ? &network->interfaces[i]
                                                         : NULL;
            draw_network_line(screen, top + i, right_left, right_width, interface, include_name);
        }
        return;
    }
    draw_plot(screen, top, 0, left_width, ram, ram_title, ram_info);
    draw_plot(screen, top, middle_left, middle_width, swap, swap_title, swap_info);
    for (int i = 0; i < line_count; i++) {
        const NetworkInterfaceInfo *interface = network->interface_count > 0
                                                     ? &network->interfaces[i]
                                                     : NULL;
        draw_network_line(screen, top + i, right_left, right_width, interface, include_name);
    }
    int graph_rows = network_graph_rows(line_count);
    if (graph_rows > 0)
        draw_network_graph(screen, top + line_count, right_left, right_width,
                           network_history, graph_rows);
}

static void add_click_target(UiState *ui, int row, int left, int right, unsigned char key) {
    if (!ui || left >= right || ui->click_target_count >= MAX_CLICK_TARGETS) return;
    ui->click_targets[ui->click_target_count++] = (ClickTarget){row, left, right, key};
}

static void draw_clickable_text(Screen *screen, UiState *ui, int row, int *column,
                                const char *text, int highlight_start, int highlight_length,
                                int hit_start, int hit_length, unsigned char key) {
    int length = (int)strlen(text);
    screen_put_attr(screen, row, *column, text, length, ATTR_DIM);
    for (int i = 0; i < highlight_length && highlight_start + i < length; i++)
        screen_ch_attr(screen, row, *column + highlight_start + i,
                       text[highlight_start + i], ATTR_CYAN_BOLD);
    add_click_target(ui, row, *column + hit_start, *column + hit_start + hit_length, key);
    *column += length;
}

static void draw_header(Screen *screen, const char *driver, const char *cuda_library,
                        const char *cuda_runtime, int cuda_driver, double interval,
                        bool paused, UiState *ui) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char time_text[64];
    strftime(time_text, sizeof(time_text), "%a %b %d %H:%M:%S %Y", &local_time);
    screen_putf_attr(screen, 0, 0, screen->cols, ATTR_CYAN, "%s  (Press h for help or q to quit)", time_text);
    char cuda_api[32];
    format_cuda_api(cuda_api, sizeof(cuda_api), cuda_driver);
    if (strcmp(cuda_runtime, "N/A") != 0)
        screen_putf_attr(screen, 1, 0, screen->cols, ATTR_CYAN_BOLD, "ctop %s  |  Driver %s  |  CUDA lib %s (runtime %s)  |  Driver API %s",
                         CTOP_VERSION, driver, cuda_library, cuda_runtime, cuda_api);
    else
        screen_putf_attr(screen, 1, 0, screen->cols, ATTR_CYAN_BOLD, "ctop %s  |  Driver %s  |  CUDA lib %s  |  Driver API %s",
                         CTOP_VERSION, driver, cuda_library, cuda_api);

    ui->click_target_count = 0;
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s  refresh %.1fs  |  ", paused ? "PAUSED" : "LIVE", interval);
    screen_put_attr(screen, 2, 0, prefix, screen->cols, paused ? ATTR_YELLOW : ATTR_DIM);
    int column = (int)strlen(prefix);
    draw_clickable_text(screen, ui, 2, &column, "q:quit  ", 0, 1, 0, 1, 'q');
    draw_clickable_text(screen, ui, 2, &column, "h:help  ", 0, 1, 0, 1, 'h');
    draw_clickable_text(screen, ui, 2, &column, "g:GPU graph  ", 0, 1, 0, 1, 'g');
    draw_clickable_text(screen, ui, 2, &column, "m:RAM/Swap + network  ", 0, 1, 0, 1, 'm');
    draw_clickable_text(screen, ui, 2, &column, "space:pause  ", 0, 5, 0, 5, ' ');
    int interval_column = column;
    draw_clickable_text(screen, ui, 2, &column, "+/-:interval  ", 0, 1, 0, 1, '+');
    add_click_target(ui, 2, interval_column + 2, interval_column + 3, '-');
    screen_ch_attr(screen, 2, interval_column + 2, '-', ATTR_CYAN_BOLD);
    draw_clickable_text(screen, ui, 2, &column, "r:reset  ", 0, 1, 0, 1, 'r');
    draw_clickable_text(screen, ui, 2, &column, "j:next  ", 0, 1, 0, 1, 'j');
    int kill_column = column;
    draw_clickable_text(screen, ui, 2, &column, "k/K:kill  ", 0, 1, 0, 1, 'k');
    add_click_target(ui, 2, kill_column + 2, kill_column + 3, 'K');
    screen_ch_attr(screen, 2, kill_column + 2, 'K', ATTR_CYAN_BOLD);
    if (ui->notice[0] && column + 2 < screen->cols) {
        char notice[144];
        snprintf(notice, sizeof(notice), "  %s", ui->notice);
        screen_put_attr(screen, 2, column, notice, screen->cols - column,
                        ui->notice_error ? ATTR_RED : ATTR_TEXT_GREEN);
    }
}

static void render_screen(Screen *screen, const GpuInfo *gpus, int gpu_count,
                          const SystemInfo *system, const History *vram,
                          const History *gpu_util, const History *ram,
                          const History *swap, const History *network_history,
                          const NetworkInfo *network, UiState *ui,
                          const char *driver, const char *cuda_library,
                          const char *cuda_runtime, int cuda_driver,
                          double interval, bool paused, bool help_visible) {
    screen_clear(screen);
    Marker markers[4] = {0};
    size_t marker_count = 0;
    draw_header(screen, driver, cuda_library, cuda_runtime, cuda_driver, interval, paused, ui);
    if (help_visible) {
        const char *lines[] = {
            "KEYS",
            "q or Esc     quit",
            "h            show/hide this help",
            "click g       collapse/expand GPU history",
            "click m       collapse/expand RAM/Swap/network section",
            "Space        pause/resume sampling",
            "+ / -        increase/decrease refresh interval",
            "j            select next GPU process",
            "k / K        ask before sending SIGTERM to selected process",
            "r            clear graph history",
            "click keys in the top bar to run the same actions",
            "",
            "Mouse input uses the terminal's SGR mouse protocol; no tmux setting is required.",
        };
        for (size_t i = 0; i < ARRAY_LEN(lines); i++) screen_put(screen, 4 + (int)i, 0, lines[i], screen->cols);
        exit_prompt_row = 4 + (int)ARRAY_LEN(lines);
        if (exit_prompt_row > screen->rows) exit_prompt_row = screen->rows;
        screen_flush(screen, markers, 0);
        return;
    }

    int y = 3;
    draw_rule(screen, y, "GPU STATUS", false, true, markers, &marker_count);
    y++;
    draw_gpu_status(screen, y, gpus, gpu_count);
    y += 1 + gpu_count;

    draw_rule(screen, y, "USAGE HISTORY - PEAK", true, !ui->usage_collapsed, markers, &marker_count);
    y++;
    double vram_average = 0.0, gpu_average = 0.0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].memory_valid && gpus[i].memory_total)
            vram_average += 100.0 * (double)gpus[i].memory_used / (double)gpus[i].memory_total;
        if (gpus[i].utilization_valid) gpu_average += gpus[i].utilization;
    }
    if (gpu_count) {
        vram_average /= gpu_count;
        gpu_average /= gpu_count;
    }
    if (ui->usage_collapsed) {
        int half = (screen->cols - 2) / 2;
        screen_putf(screen, y, 0, half, "VRAM AVG(%.1f%%)", vram_average);
        screen_putf(screen, y, half + 2, screen->cols - half - 2, "GPU AVG(%.1f%%)", gpu_average);
        y++;
    } else {
        int gap = 2;
        int left_width = (screen->cols - gap) / 2;
        int right_left = left_width + gap;
        int right_width = screen->cols - right_left;
        char vram_title[64], gpu_title[64];
        snprintf(vram_title, sizeof(vram_title), "VRAM AVG(%.1f%%) [30s|10s|3s|1s]", vram_average);
        snprintf(gpu_title, sizeof(gpu_title), "GPU AVG(%.1f%%) [30s|10s|3s|1s]", gpu_average);
        draw_plot(screen, y, 0, left_width, vram, vram_title, NULL);
        draw_plot(screen, y, right_left, right_width, gpu_util, gpu_title, NULL);
        y += 2 + GRAPH_ROWS;
    }

    draw_rule(screen, y, "SYSTEM", true, !ui->system_collapsed, markers, &marker_count);
    y++;
    draw_system(screen, y, system, ram, swap, network_history, network, ui->system_collapsed);
    y += system_content_rows(network->interface_count, ui->system_collapsed);

    draw_rule(screen, y, "GPU PROCESSES", false, true, markers, &marker_count);
    y++;
    draw_processes(screen, y, gpus, gpu_count, ui);
    int last_content_row = y;
    for (int i = 0; i < gpu_count; i++) last_content_row += (int)gpus[i].process_count;
    exit_prompt_row = last_content_row + 1;
    if (exit_prompt_row > screen->rows) exit_prompt_row = screen->rows;
    screen_flush(screen, markers, marker_count);
}

/* ------------------------------------------------------------------------- */
/* Terminal and input                                                        */

static void restore_terminal(void) {
    if (!terminal_saved) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
    if (alternate_screen) {
        printf("\033[?1000l\033[?1006l\033[?25h\033[?1049l\033[0m");
    } else {
        struct winsize size;
        int rows = exit_prompt_row;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row) {
            if (rows < 1 || rows > (int)size.ws_row) rows = size.ws_row;
        }
        /* Put the shell prompt immediately below the last ctop content row.
           The CRLF may scroll once only when that row is the terminal bottom. */
        printf("\033[?1000l\033[?1006l\033[%d;1H\033[0m\r\n\033[?25h", rows);
    }
    fflush(stdout);
    terminal_saved = false;
}

static void signal_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static bool enter_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &saved_terminal) != 0) return false;
    terminal_saved = true;
    struct termios raw = saved_terminal;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    if (alternate_screen)
        printf("\033[?1049h\033[?25l\033[?1000h\033[?1006h\033[2J\033[H");
    else {
        /* Use the current screen so the final ctop frame remains visible on exit. */
        printf("\033[2J\033[H\033[?25l\033[?1000h\033[?1006h");
    }
    fflush(stdout);
    atexit(restore_terminal);
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    signal(SIGWINCH, SIG_IGN);
    return true;
}

static bool terminal_size(int *rows, int *cols) {
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || !size.ws_row || !size.ws_col) {
        *rows = 24;
        *cols = 120;
        return false;
    }
    *rows = size.ws_row;
    *cols = size.ws_col > MAX_SCREEN_COLS ? MAX_SCREEN_COLS : size.ws_col;
    return true;
}

static void toggle_mouse_at(UiState *ui, int x, int y, int cols, int gpu_count) {
    GraphRows rows = graph_rows(gpu_count, ui);
    if (x < cols - 6) return;
    if (y == rows.usage_rule) ui->usage_collapsed = !ui->usage_collapsed;
    else if (y == rows.system_rule) ui->system_collapsed = !ui->system_collapsed;
}

static void handle_mouse(UiState *ui, const GpuInfo *gpus, int x, int y, int cols,
                         int gpu_count, bool pressed, bool *button_down) {
    bool process_row = process_at_screen_row(y, gpu_count, ui, gpus, NULL, NULL);
    if (process_row) {
        if (pressed) {
            if (!*button_down) select_process_at(ui, y, gpu_count, gpus);
            *button_down = true;
        } else {
            if (!*button_down) select_process_at(ui, y, gpu_count, gpus);
            *button_down = false;
        }
        return;
    }

    /* Only the left button toggles.  A press toggles immediately; a
       release-only terminal event also toggles, but press+release does not
       toggle twice. */
    if (pressed) {
        if (!*button_down) {
            toggle_mouse_at(ui, x, y, cols, gpu_count);
            *button_down = true;
        }
    } else {
        if (!*button_down) toggle_mouse_at(ui, x, y, cols, gpu_count);
        *button_down = false;
    }
}

static unsigned char clickable_key_at(const UiState *ui, int x, int y) {
    for (size_t i = 0; i < ui->click_target_count; i++) {
        const ClickTarget *target = &ui->click_targets[i];
        if (target->row == y && x >= target->left && x < target->right)
            return target->key;
    }
    return 0;
}

static void handle_key(unsigned char key, UiState *ui, bool *paused, bool *help_visible,
                       double *interval, History *vram, History *gpu,
                       History *ram, History *swap, History *network,
                       const GpuInfo *gpus, int gpu_count) {
    if (ui->kill_prompt) {
        if (key == 'y' || key == 'Y') {
            if (kill(ui->prompt_pid, SIGTERM) == 0) {
                snprintf(ui->notice, sizeof(ui->notice), "SIGTERM sent to PID %ld", (long)ui->prompt_pid);
                ui->notice_error = false;
                ui->selected_gpu = -1;
                ui->selected_pid = 0;
            } else {
                snprintf(ui->notice, sizeof(ui->notice), "kill PID %ld failed: %s",
                         (long)ui->prompt_pid, strerror(errno));
                ui->notice_error = true;
            }
            ui->kill_prompt = false;
        } else if (key == 'n' || key == 'N') {
            snprintf(ui->notice, sizeof(ui->notice), "Kill cancelled");
            ui->notice_error = false;
            ui->kill_prompt = false;
        }
        return;
    }

    ui->notice[0] = '\0';
    ui->notice_error = false;
    if (key == 'h' || key == 'H') *help_visible = !*help_visible;
    else if (key == 'g' || key == 'G') ui->usage_collapsed = !ui->usage_collapsed;
    else if (key == 'm' || key == 'M') ui->system_collapsed = !ui->system_collapsed;
    else if (key == ' ') *paused = !*paused;
    else if (key == '+' || key == '=') {
        *interval += 0.1;
        if (*interval > 10.0) *interval = 10.0;
    } else if (key == '-') {
        *interval -= 0.1;
        if (*interval < 0.1) *interval = 0.1;
    } else if (key == 'r' || key == 'R') {
        history_clear(vram);
        history_clear(gpu);
        history_clear(ram);
        history_clear(swap);
        history_clear(network);
        ui->selected_gpu = -1;
        ui->selected_pid = 0;
        ui->prompt_pid = 0;
        ui->kill_prompt = false;
    } else if (key == 'j') {
        move_process_selection(ui, gpus, gpu_count, 1);
    } else if (key == 'k' || key == 'K') {
        if (ui->selected_pid > 0) {
            ui->prompt_pid = ui->selected_pid;
            ui->kill_prompt = true;
        } else {
            snprintf(ui->notice, sizeof(ui->notice), "Select a process first");
            ui->notice_error = true;
        }
    }
}

static void dispatch_key(unsigned char key, UiState *ui, bool *paused, bool *help_visible,
                         double *interval, History *vram, History *gpu,
                         History *ram, History *swap, History *network,
                         const GpuInfo *gpus, int gpu_count, bool *quit) {
    if (key == 'q' || key == 'Q') *quit = true;
    else handle_key(key, ui, paused, help_visible, interval, vram, gpu, ram, swap, network, gpus, gpu_count);
}

static void parse_input(InputBuffer *input, UiState *ui, bool *paused, bool *help_visible,
                        double *interval, History *vram, History *gpu,
                        History *ram, History *swap, History *network,
                        const GpuInfo *gpus, int cols, int gpu_count,
                        bool *quit, bool *mouse_button_down) {
    size_t position = 0;
    while (position < input->length) {
        unsigned char *data = (unsigned char *)input->data;
        if (data[position] != 0x1b) {
            dispatch_key(data[position], ui, paused, help_visible, interval,
                         vram, gpu, ram, swap, network, gpus, gpu_count, quit);
            position++;
            continue;
        }
        if (input->length - position < 3) break;
        if (data[position + 1] == '[' && data[position + 2] == '<') {
            size_t end = position + 3;
            while (end < input->length && data[end] != 'M' && data[end] != 'm') end++;
            if (end == input->length) break;
            char sequence[128];
            size_t length = end - (position + 3);
            if (length >= sizeof(sequence)) length = sizeof(sequence) - 1;
            memcpy(sequence, data + position + 3, length);
            sequence[length] = '\0';
            int button = 0, x = 0, y = 0;
            if (sscanf(sequence, "%d;%d;%d", &button, &x, &y) == 3 && (button & 3) == 0) {
                int screen_x = x - 1;
                int screen_y = y - 1;
                bool pressed = data[end] == 'M';
                unsigned char clicked_key = clickable_key_at(ui, screen_x, screen_y);
                if (clicked_key) {
                    if (pressed) {
                        if (!*mouse_button_down)
                            dispatch_key(clicked_key, ui, paused, help_visible, interval,
                                         vram, gpu, ram, swap, network, gpus, gpu_count, quit);
                        *mouse_button_down = true;
                    } else {
                        if (!*mouse_button_down)
                            dispatch_key(clicked_key, ui, paused, help_visible, interval,
                                         vram, gpu, ram, swap, network, gpus, gpu_count, quit);
                        *mouse_button_down = false;
                    }
                } else {
                    handle_mouse(ui, gpus, screen_x, screen_y, cols, gpu_count,
                                 pressed, mouse_button_down);
                }
            }
            position = end + 1;
            continue;
        }
        if (data[position + 1] == '[' && data[position + 2] == 'M') {
            if (input->length - position < 6) break;
            int button = (int)data[position + 3] - 32;
            int x = (int)data[position + 4] - 32;
            int y = (int)data[position + 5] - 32;
            if ((button & 3) == 0) {
                int screen_x = x - 1;
                int screen_y = y - 1;
                unsigned char clicked_key = clickable_key_at(ui, screen_x, screen_y);
                if (clicked_key) {
                    if (!*mouse_button_down)
                        dispatch_key(clicked_key, ui, paused, help_visible, interval,
                                     vram, gpu, ram, swap, network, gpus, gpu_count, quit);
                    *mouse_button_down = true;
                } else {
                    handle_mouse(ui, gpus, screen_x, screen_y, cols, gpu_count,
                                 true, mouse_button_down);
                }
            }
            position += 6;
            continue;
        }
        /* Unknown escape sequence (arrows, function keys, etc.). */
        position += 2;
    }
    if (position) {
        memmove(input->data, input->data + position, input->length - position);
        input->length -= position;
    }
}

/* ------------------------------------------------------------------------- */
/* Main                                                                     */

static int collect_snapshot(GpuInfo *gpus, int *gpu_count, SystemInfo *system,
                            NetworkInfo *network, unsigned long long *total_ticks) {
    unsigned int count = 0;
    nvmlReturn_t result = nvmlDeviceGetCount_v2(&count);
    if (result != NVML_SUCCESS) return -1;
    if (count > MAX_GPUS) count = MAX_GPUS;
    for (unsigned int i = 0; i < count; i++) {
        GpuInfo *gpu = &gpus[i];
        memset(gpu, 0, sizeof(*gpu));
        gpu->index = (int)i;
        if (nvmlDeviceGetHandleByIndex_v2(i, &gpu->handle) != NVML_SUCCESS) continue;
        nvmlDeviceGetName(gpu->handle, gpu->name, sizeof(gpu->name));
        unsigned int value = 0;
        if (nvmlDeviceGetTemperature(gpu->handle, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) {
            gpu->temperature = value;
            gpu->temperature_valid = true;
        }
        if (nvmlDeviceGetPowerUsage(gpu->handle, &value) == NVML_SUCCESS) {
            gpu->power = (double)value / 1000.0;
            gpu->power_valid = true;
        }
        if (nvmlDeviceGetEnforcedPowerLimit(gpu->handle, &value) == NVML_SUCCESS) {
            gpu->power_limit = (double)value / 1000.0;
            gpu->power_limit_valid = true;
        }
        nvmlMemory_t memory;
        if (nvmlDeviceGetMemoryInfo(gpu->handle, &memory) == NVML_SUCCESS) {
            gpu->memory_used = memory.used;
            gpu->memory_total = memory.total;
            gpu->memory_valid = true;
        }
        nvmlUtilization_t utilization;
        if (nvmlDeviceGetUtilizationRates(gpu->handle, &utilization) == NVML_SUCCESS) {
            gpu->utilization = utilization.gpu;
            gpu->utilization_valid = true;
        }
        collect_processes(gpu);
        collect_process_sm(gpu);
    }
    *gpu_count = (int)count;

    if (!read_system_info(system)) return -2;
    if (!read_network_info(network)) memset(network, 0, sizeof(*network));
    if (!read_cpu_total(total_ticks)) *total_ticks = 0;
    double uptime = read_uptime();
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    cpu_current_count = 0;
    for (int i = 0; i < *gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            fill_process_info(&gpus[i].processes[j], *total_ticks, system->ram_total,
                              uptime, ticks_per_second);
        }
    }
    return 0;
}

static void usage(const char *program) {
    printf("Usage: %s [-i seconds] [--no-color] [--debug-log FILE] [-a|--alternate-screen] [-r|--restore-screen] [-v|--version]\n", program);
    printf("  -i, --interval N       sample interval, default 1 second\n");
    printf("  --no-color             disable ANSI colors\n");
    printf("  --debug-log FILE       append snapshot/render diagnostics to FILE\n");
    printf("  -a, --alternate-screen     restore the previous screen on exit\n");
    printf("  -r, --restore-screen       alias for --alternate-screen\n");
    printf("  -v, --version              show version\n");
}

int main(int argc, char **argv) {
    double interval = 1.0;
    const char *debug_log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("ctop %s\n", CTOP_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            color_enabled = false;
        } else if (strcmp(argv[i], "--debug-log") == 0) {
            if (++i >= argc || !argv[i][0]) { usage(argv[0]); return 2; }
            debug_log_path = argv[i];
        } else if (strcmp(argv[i], "--alternate-screen") == 0 ||
                   strcmp(argv[i], "--restore-screen") == 0 ||
                   strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-r") == 0) {
            alternate_screen = true;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            interval = strtod(argv[i], NULL);
            if (interval <= 0.0) { fprintf(stderr, "interval must be greater than zero\n"); return 2; }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (debug_log_path) {
        debug_log_file = fopen(debug_log_path, "a");
        if (!debug_log_file) {
            fprintf(stderr, "cannot open debug log %s: %s\n", debug_log_path, strerror(errno));
            return 1;
        }
        setvbuf(debug_log_file, NULL, _IOLBF, 0);
        if (atexit(close_debug_log) != 0) {
            fprintf(stderr, "could not register debug log cleanup\n");
            close_debug_log();
            return 1;
        }
        debug_log("started pid=%ld version=%s interval=%.3f", (long)getpid(), CTOP_VERSION, interval);
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        debug_log("startup failed: stdin/stdout is not a TTY");
        fprintf(stderr, "ctop requires an interactive terminal\n");
        return 2;
    }

    nvmlReturn_t nvml_result = nvmlInit_v2();
    if (nvml_result != NVML_SUCCESS) {
        debug_log("NVML initialization failed: %s", nvmlErrorString(nvml_result));
        fprintf(stderr, "NVML initialization failed: %s\n", nvmlErrorString(nvml_result));
        return 1;
    }
    debug_log("NVML initialized");
    char driver[96] = "N/A";
    nvmlSystemGetDriverVersion(driver, sizeof(driver));
    int cuda_driver = 0;
    nvmlSystemGetCudaDriverVersion_v2(&cuda_driver);
    char cuda_library[64], cuda_runtime[64];
    detect_cuda_library(cuda_library, sizeof(cuda_library), cuda_runtime, sizeof(cuda_runtime));
    debug_log("driver=%s cuda_library=%s cuda_runtime=%s cuda_driver=%d",
              driver, cuda_library, cuda_runtime, cuda_driver);

    History vram = {0}, gpu_util = {0}, ram = {0}, swap = {0}, network_history = {0};
    if (!history_init(&vram) || !history_init(&gpu_util) || !history_init(&ram) ||
        !history_init(&swap) || !history_init(&network_history)) {
        debug_log("startup failed: history allocation");
        fprintf(stderr, "history allocation failed\n");
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&network_history);
        nvmlShutdown();
        return 1;
    }

    if (!enter_terminal()) {
        debug_log("startup failed: could not enter raw terminal mode (%s)", strerror(errno));
        fprintf(stderr, "could not enter raw terminal mode\n");
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&network_history);
        nvmlShutdown();
        return 1;
    }

    int rows = 24, cols = 120;
    terminal_size(&rows, &cols);
    debug_log("terminal size rows=%d cols=%d", rows, cols);
    Screen screen = {0};
    if (!screen_init(&screen, rows, cols)) {
        debug_log("startup failed: screen allocation rows=%d cols=%d", rows, cols);
        restore_terminal();
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&network_history);
        nvmlShutdown();
        return 1;
    }
    GpuInfo *gpus = calloc(MAX_GPUS, sizeof(*gpus));
    if (!gpus) {
        debug_log("startup failed: GPU state allocation");
        screen_free(&screen);
        restore_terminal();
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&network_history);
        nvmlShutdown();
        return 1;
    }
    int gpu_count = 0;
    SystemInfo system = {0};
    NetworkInfo network = {0};
    unsigned long long total_ticks = 0;
    UiState ui = {0};
    bool paused = false, help_visible = false, quit = false;
    bool mouse_button_down = false;
    char error[256] = "";
    double next_sample = monotonic_seconds();
    double next_clock = next_sample;
    bool needs_render = true;
    InputBuffer input = {0};

    while (!stop_requested && !quit) {
        int new_rows, new_cols;
        terminal_size(&new_rows, &new_cols);
        if (new_rows != screen.rows || new_cols != screen.cols) {
            screen_resize(&screen, new_rows, new_cols);
            rows = new_rows;
            cols = new_cols;
            needs_render = true;
        }
        double now = monotonic_seconds();
        if (!paused && now >= next_sample) {
            double sample_started = monotonic_seconds();
            debug_log("snapshot begin");
            int result = collect_snapshot(gpus, &gpu_count, &system, &network, &total_ticks);
            double sample_duration = monotonic_seconds() - sample_started;
            if (result != 0) {
                debug_log("snapshot failed result=%d duration=%.3fs", result, sample_duration);
                snprintf(error, sizeof(error), "snapshot error (%d)", result);
            } else {
                debug_log("snapshot done duration=%.3fs gpus=%d ram=%.1f%% swap=%.1f%% net_valid=%d net=%.1fbps net_load=%.1f%%",
                          sample_duration, gpu_count, system.ram_percent, system.swap_percent,
                          network.valid, network.total_bps, network.utilization_percent);
                for (int i = 0; i < gpu_count; i++)
                    debug_log("gpu[%d] mem_valid=%d mem=%llu/%llu util_valid=%d util=%.1f",
                              gpus[i].index, gpus[i].memory_valid,
                              gpus[i].memory_used, gpus[i].memory_total,
                              gpus[i].utilization_valid, gpus[i].utilization);
                error[0] = '\0';
                ui.network_interface_count = network.interface_count;
                double average_vram = 0.0, average_gpu = 0.0;
                for (int i = 0; i < gpu_count; i++) {
                    if (gpus[i].memory_valid && gpus[i].memory_total)
                        average_vram += 100.0 * (double)gpus[i].memory_used / (double)gpus[i].memory_total;
                    if (gpus[i].utilization_valid) average_gpu += gpus[i].utilization;
                }
                if (gpu_count) {
                    average_vram /= gpu_count;
                    average_gpu /= gpu_count;
                }
                double timestamp = monotonic_seconds();
                history_add(&vram, timestamp, average_vram);
                history_add(&gpu_util, timestamp, average_gpu);
                history_add(&ram, timestamp, system.ram_percent);
                history_add(&swap, timestamp, system.swap_percent);
                if (network.utilization_valid)
                    history_add(&network_history, timestamp, network.utilization_percent);
            }
            next_sample = now + interval;
            needs_render = true;
        } else if (paused && now >= next_sample) {
            next_sample = now + interval;
            needs_render = true;
        }
        if (now >= next_clock) {
            next_clock = now + 1.0;
            needs_render = true;
        }

        if (!needs_render) {
            /* No metric, clock, resize or input change: leave the terminal
               untouched instead of repainting it several times per second. */
        } else {
        debug_log("render begin rows=%d cols=%d error=%s", screen.rows, screen.cols,
                  error[0] ? error : "none");
        screen_clear(&screen);
        if (screen.rows < 13 || screen.cols < 76) {
            exit_prompt_row = screen.rows;
            screen_putf(&screen, 0, 0, screen.cols, "ctop %s - terminal too small (need at least 76x13)", CTOP_VERSION);
            screen_putf(&screen, 2, 0, screen.cols, "Current terminal: %dx%d. Resize the terminal or press q.", screen.cols, screen.rows);
        } else if (error[0]) {
            exit_prompt_row = screen.rows;
            draw_header(&screen, driver, cuda_library, cuda_runtime, cuda_driver, interval, paused, &ui);
            screen_putf(&screen, 3, 0, screen.cols, "ERROR: %s", error);
        } else {
            render_screen(&screen, gpus, gpu_count, &system, &vram, &gpu_util, &ram, &swap,
                          &network_history, &network, &ui, driver, cuda_library, cuda_runtime, cuda_driver,
                          interval, paused, help_visible);
        }
        if (screen.rows >= 13 && screen.cols >= 76 && !error[0] && !help_visible) {
            /* render_screen already flushed. */
        } else {
            screen_flush(&screen, NULL, 0);
        }
        debug_log("render end");
        needs_render = false;
        }

        double deadline = next_sample < next_clock ? next_sample : next_clock;
        double wait_seconds = deadline - monotonic_seconds();
        if (wait_seconds < 0.02) wait_seconds = 0.02;
        if (wait_seconds > 1.0) wait_seconds = 1.0;
        struct pollfd descriptor = {.fd = STDIN_FILENO, .events = POLLIN};
        int poll_result = poll(&descriptor, 1, (int)(wait_seconds * 1000.0));
        if (poll_result < 0 && errno != EINTR)
            debug_log("poll failed: %s", strerror(errno));
        if (poll_result > 0) {
            debug_log("poll event revents=0x%x", descriptor.revents);
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                debug_log("terminal input closed or invalid");
                quit = true;
            }
        }
        if (poll_result > 0 && (descriptor.revents & POLLIN)) {
            ssize_t read_count = read(STDIN_FILENO, input.data + input.length,
                                      sizeof(input.data) - input.length);
            if (read_count > 0) {
                debug_log("terminal input bytes=%zd", read_count);
                input.length += (size_t)read_count;
                parse_input(&input, &ui, &paused, &help_visible, &interval,
                            &vram, &gpu_util, &ram, &swap, &network_history,
                            gpus, cols, gpu_count, &quit, &mouse_button_down);
                next_sample = monotonic_seconds() + (paused ? interval : 0.0);
                next_clock = monotonic_seconds() + 1.0;
                needs_render = true;
            } else if (read_count == 0) {
                debug_log("terminal input reached EOF");
                quit = true;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                debug_log("terminal read failed: %s", strerror(errno));
                quit = true;
            }
        }
    }

    debug_log("main loop ended stop_requested=%d quit=%d", stop_requested, quit);
    free(gpus);
    screen_free(&screen);
    history_free(&vram);
    history_free(&gpu_util);
    history_free(&ram);
    history_free(&swap);
    history_free(&network_history);
    restore_terminal();
    nvmlShutdown();
    debug_log("shutdown complete");
    return 0;
}
