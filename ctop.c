#define _GNU_SOURCE

#if defined(CTOP_ENABLE_NVML)
#include <nvml.h>
#endif
#if defined(CTOP_ENABLE_VULKAN)
#include <vulkan/vulkan.h>
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <mntent.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "web_assets.h"

#define CTOP_VERSION "0.4.5"
#define GRAPH_ROWS 5
#define HISTORY_LIMIT 16384
#define MAX_GPUS 32
#define MAX_REMOTE_MONITORS 4
#define MAX_TUI_GPUS (MAX_GPUS * (MAX_REMOTE_MONITORS + 1))
#define MAX_DASHBOARD_SECTIONS (1 + (MAX_REMOTE_MONITORS + 1) * 3)
#define REMOTE_ADDRESS_SIZE 1024
#define REMOTE_TOKEN_SIZE 129
#define REMOTE_RESPONSE_LIMIT (16 * 1024 * 1024)
#define MAX_GPU_PROCESSES 512
#define MAX_CPU_PROCESSES 2048
#define MAX_SCREEN_COLS 2048
#define MAX_OVERLAYS 8192
#define MAX_NETWORK_INTERFACES 32
#define MAX_DISK_DEVICES 128
#define MAX_CLICK_TARGETS 64
#define INPUT_BUFFER_SIZE 8192
#define UI_KEY_FOCUS_MONITOR 0x10
#define UI_KEY_SUBMIT_MONITOR 0x11
#define UI_KEY_FOCUS_TOKEN 0x12
#define UI_KEY_MOVE_SECTION_UP_BASE 0xE0
#define UI_KEY_MOVE_SECTION_DOWN_BASE 0xF0

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
static int active_web_port = 0;
static char active_web_url[64] = "";
static const char *web_token = NULL;
static double sample_interval_seconds = 1.0;
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
    char elapsed_display[32];
    unsigned long long drm_gfx_engine_ns;
    unsigned long long drm_memory_vram_kib;
    unsigned long long drm_memory_gtt_kib;
    bool drm_gfx_engine_valid;
    bool drm_memory_vram_valid;
    bool drm_memory_gtt_valid;
} ProcessInfo;

typedef struct {
    int index;
    int source_id;
    char source_prefix[4];
#if defined(CTOP_ENABLE_NVML)
    nvmlDevice_t handle;
#endif
    bool is_amd;
    bool prefer_gtt;
    char pci_bus_id[24];
    char sysfs_path[PATH_MAX];
    char name[96];
    char data_source[16];
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

#if defined(CTOP_ENABLE_NVML)
typedef struct {
    void *library;
    nvmlReturn_t (*init_v2)(void);
    nvmlReturn_t (*shutdown)(void);
    const char *(*error_string)(nvmlReturn_t);
    nvmlReturn_t (*device_get_count_v2)(unsigned int *);
    nvmlReturn_t (*device_get_handle_by_index_v2)(unsigned int, nvmlDevice_t *);
    nvmlReturn_t (*device_get_name)(nvmlDevice_t, char *, unsigned int);
    nvmlReturn_t (*device_get_temperature)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int *);
    nvmlReturn_t (*device_get_power_usage)(nvmlDevice_t, unsigned int *);
    nvmlReturn_t (*device_get_enforced_power_limit)(nvmlDevice_t, unsigned int *);
    nvmlReturn_t (*device_get_memory_info)(nvmlDevice_t, nvmlMemory_t *);
    nvmlReturn_t (*device_get_utilization_rates)(nvmlDevice_t, nvmlUtilization_t *);
    nvmlReturn_t (*device_get_compute_processes_v3)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
    nvmlReturn_t (*device_get_graphics_processes_v3)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
    nvmlReturn_t (*device_get_process_utilization)(nvmlDevice_t, nvmlProcessUtilizationSample_t *, unsigned int *, unsigned long long);
    nvmlReturn_t (*system_get_driver_version)(char *, unsigned int);
    nvmlReturn_t (*system_get_cuda_driver_version_v2)(int *);
} NvmlApi;

static NvmlApi nvml_api;
static bool nvml_initialized;
#endif

#if defined(CTOP_ENABLE_VULKAN)
typedef struct {
    VkPhysicalDevice physical_device;
    uint32_t device_id;
    char name[96];
    char pci_bus_id[24];
    char sysfs_path[PATH_MAX];
    bool prefer_gtt;
} VulkanGpu;

static VkInstance vulkan_instance;
static VulkanGpu vulkan_gpus[MAX_GPUS];
static int vulkan_gpu_count;
#endif

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
    double read_bps;
    double write_bps;
    double total_bps;
    unsigned long long used_bytes;
    unsigned long long total_bytes;
    bool capacity_valid;
    bool valid;
} DiskInfo;

typedef struct {
    char name[64];
    unsigned long long read_sectors;
    unsigned long long write_sectors;
    double timestamp;
    bool valid;
} DiskPrevious;

typedef struct {
    pid_t pid;
    unsigned long long process_ticks;
    unsigned long long total_ticks;
    double value;
    bool valid;
} CpuPrevious;

#if defined(CTOP_ENABLE_VULKAN)
typedef struct {
    pid_t pid;
    char pci_bus_id[24];
    unsigned long long engine_ns;
    double timestamp;
    unsigned long long generation;
} DrmProcessPrevious;
#endif

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

typedef enum {
    SECTION_GPU_LIST,
    SECTION_GPU,
    SECTION_SYSTEM,
    SECTION_GPU_PROCESSES,
} DashboardSectionType;

typedef struct {
    int source_id;
    DashboardSectionType type;
} DashboardSection;

typedef struct {
    int row;
    int left;
    int right;
    unsigned char key;
} ClickTarget;

typedef struct {
    bool usage_collapsed;
    bool system_collapsed;
    bool processes_collapsed;
    int selected_gpu;
    int selected_source_id;
    pid_t selected_pid;
    bool kill_prompt;
    pid_t prompt_pid;
    char notice[128];
    bool notice_error;
    ClickTarget click_targets[MAX_CLICK_TARGETS];
    size_t click_target_count;
    int network_interface_count;
    DashboardSection sections[MAX_DASHBOARD_SECTIONS];
    size_t section_count;
    int selected_section;
    bool monitor_prompt;
    bool monitor_token_focus;
    bool monitor_add_focus;
    char monitor_input[REMOTE_ADDRESS_SIZE];
    size_t monitor_input_length;
    char monitor_token_input[REMOTE_TOKEN_SIZE];
    size_t monitor_token_input_length;
} UiState;

typedef struct {
    int row;
    bool expanded;
} Marker;

typedef struct {
    int id;
    char address[REMOTE_ADDRESS_SIZE];
    char host[256];
    char path[512];
    int port;
    char token[REMOTE_TOKEN_SIZE];
    char prefix[4];
    GpuInfo *gpus;
    int gpu_count;
    int gpu_start;
    int gpu_display_count;
    SystemInfo system;
    DiskInfo disk;
    NetworkInfo network;
    History ram_history;
    History swap_history;
    History disk_history;
    History network_history;
    History vram_history;
    History gpu_history;
    bool connected;
    char status[96];
    bool gpu_collapsed;
    bool system_collapsed;
    bool processes_collapsed;
} RemoteMonitor;

typedef struct {
    int id;
    int gpu_start;
    int gpu_count;
    const SystemInfo *system;
    const DiskInfo *disk;
    const NetworkInfo *network;
    const History *ram_history;
    const History *swap_history;
    const History *disk_history;
    const History *network_history;
    const History *vram_history;
    const History *gpu_history;
    bool gpu_collapsed;
    bool system_collapsed;
    bool processes_collapsed;
} MonitorView;

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
#if defined(CTOP_ENABLE_VULKAN)
static DrmProcessPrevious drm_process_previous[MAX_GPU_PROCESSES * MAX_GPUS];
static size_t drm_process_previous_count;
static unsigned long long drm_sample_generation;
#endif
static NetworkPrevious network_previous[MAX_NETWORK_INTERFACES];
static size_t network_previous_count = 0;
static DiskPrevious disk_previous[MAX_DISK_DEVICES];
static size_t disk_previous_count = 0;
static RemoteMonitor remote_monitors[MAX_REMOTE_MONITORS];
static int remote_monitor_count;
static int next_remote_monitor_id = 1;
static MonitorView tui_views[MAX_REMOTE_MONITORS + 1];
static int tui_view_count;

static int add_remote_monitor(const char *address, const char *token,
                              char *error, size_t error_size);
static bool remote_http_get(const RemoteMonitor *monitor, char **body_out, size_t *body_size,
                            char *error, size_t error_size);
static bool remote_parse_metrics(RemoteMonitor *monitor, const char *json);
static bool remove_remote_monitor(int id);
static RemoteMonitor *remote_monitor_by_id(int id);
static void rebuild_dashboard_sections(UiState *ui);
static void append_remote_snapshots(GpuInfo *gpus, int local_gpu_count, int *total_gpu_count, UiState *ui);

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
        snprintf(out, size, "%.0fM", bps / 1000000.0);
    } else if (bps >= 1000.0) {
        snprintf(out, size, "%.0fK", bps / 1000.0);
    } else {
        snprintf(out, size, "%.0fb/s", bps);
    }
}

static void format_disk_rate(char *out, size_t size, double bytes_per_second) {
    static const char *units[] = {"B/s", "K/s", "M/s", "G/s", "T/s"};
    if (!isfinite(bytes_per_second) || bytes_per_second < 0.0) {
        snprintf(out, size, "N/A");
        return;
    }
    if (bytes_per_second == 0.0) {
        snprintf(out, size, "0");
        return;
    }
    double value = bytes_per_second;
    size_t unit = 0;
    while (value >= 1024.0 && unit < ARRAY_LEN(units) - 1) {
        value /= 1024.0;
        unit++;
    }
    if (unit <= 2 || value >= 100.0) snprintf(out, size, "%.0f%s", value, units[unit]);
    else snprintf(out, size, "%.1f%s", value, units[unit]);
}

static void format_disk_rate_inline(char *out, size_t size, double bytes_per_second) {
    char rate[32];
    format_disk_rate(rate, sizeof(rate), bytes_per_second);
    char *per_second = strstr(rate, "/s");
    if (per_second) *per_second = '\0';
    snprintf(out, size, "%s", rate);
}

static void format_disk_axis_rate(char *out, size_t size, double value, double maximum) {
    const double megabyte = 1024.0 * 1024.0;
    bool use_megabytes = maximum >= megabyte;
    double divisor = use_megabytes ? megabyte : 1024.0;
    const char *unit = use_megabytes ? "M/s" : "K/s";
    double scaled = value > 0.0 ? value / divisor : 0.0;
    if (!isfinite(scaled) || scaled < 0.0) scaled = 0.0;
    unsigned long long integer_part = (unsigned long long)scaled;
    snprintf(out, size, "%llu%s", integer_part, unit);
}

static void format_network_axis_rate(char *out, size_t size, double value, double maximum) {
    double divisor;
    const char *prefix;
    if (maximum >= 1e9) { divisor = 1e9; prefix = "G"; }
    else if (maximum >= 1e6) { divisor = 1e6; prefix = "M"; }
    else if (maximum >= 1e3) { divisor = 1e3; prefix = "K"; }
    else { divisor = 1.0; prefix = ""; }
    double scaled = value > 0.0 ? value / divisor : 0.0;
    if (!isfinite(scaled) || scaled < 0.0) scaled = 0.0;
    if (scaled > 0.0 && scaled < 10.0) snprintf(out, size, "%.1f%s", scaled, prefix);
    else snprintf(out, size, "%.0f%s", scaled, prefix);
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

#if defined(CTOP_ENABLE_NVML)
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

static bool initialize_nvml_backend(char *error, size_t error_size) {
    const char *libraries[] = {"libnvidia-ml.so.1", "libnvidia-ml.so"};
    for (size_t i = 0; i < ARRAY_LEN(libraries) && !nvml_api.library; i++)
        nvml_api.library = dlopen(libraries[i], RTLD_NOW | RTLD_LOCAL);
    if (!nvml_api.library) {
        snprintf(error, error_size, "NVML library unavailable: %s", dlerror());
        return false;
    }
#define LOAD_NVML(member, symbol) do { \
        void *address = dlsym(nvml_api.library, symbol); \
        if (!address) { \
            snprintf(error, error_size, "NVML symbol %s unavailable", symbol); \
            goto failed; \
        } \
        _Static_assert(sizeof(nvml_api.member) == sizeof(address), "POSIX function pointer size mismatch"); \
        memcpy(&nvml_api.member, &address, sizeof(address)); \
    } while (0)
#define LOAD_NVML_OPTIONAL(member, symbol) do { \
        void *address = dlsym(nvml_api.library, symbol); \
        if (address) memcpy(&nvml_api.member, &address, sizeof(address)); \
    } while (0)
    LOAD_NVML(init_v2, "nvmlInit_v2");
    LOAD_NVML(shutdown, "nvmlShutdown");
    LOAD_NVML(error_string, "nvmlErrorString");
    LOAD_NVML(device_get_count_v2, "nvmlDeviceGetCount_v2");
    LOAD_NVML(device_get_handle_by_index_v2, "nvmlDeviceGetHandleByIndex_v2");
    LOAD_NVML(device_get_name, "nvmlDeviceGetName");
    LOAD_NVML(device_get_temperature, "nvmlDeviceGetTemperature");
    LOAD_NVML(device_get_power_usage, "nvmlDeviceGetPowerUsage");
    LOAD_NVML(device_get_enforced_power_limit, "nvmlDeviceGetEnforcedPowerLimit");
    LOAD_NVML(device_get_memory_info, "nvmlDeviceGetMemoryInfo");
    LOAD_NVML(device_get_utilization_rates, "nvmlDeviceGetUtilizationRates");
    LOAD_NVML_OPTIONAL(device_get_compute_processes_v3, "nvmlDeviceGetComputeRunningProcesses_v3");
    LOAD_NVML_OPTIONAL(device_get_graphics_processes_v3, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    LOAD_NVML_OPTIONAL(device_get_process_utilization, "nvmlDeviceGetProcessUtilization");
    LOAD_NVML_OPTIONAL(system_get_driver_version, "nvmlSystemGetDriverVersion");
    LOAD_NVML_OPTIONAL(system_get_cuda_driver_version_v2, "nvmlSystemGetCudaDriverVersion_v2");
#undef LOAD_NVML
#undef LOAD_NVML_OPTIONAL

    nvmlReturn_t result = nvml_api.init_v2();
    if (result != NVML_SUCCESS) {
        snprintf(error, error_size, "NVML initialization failed: %s", nvml_api.error_string(result));
        goto failed;
    }
    nvml_initialized = true;
    return true;

failed:
#undef LOAD_NVML
#undef LOAD_NVML_OPTIONAL
    dlclose(nvml_api.library);
    memset(&nvml_api, 0, sizeof(nvml_api));
    return false;
}

static void shutdown_nvml_backend(void) {
    if (nvml_initialized && nvml_api.shutdown) nvml_api.shutdown();
    nvml_initialized = false;
    if (nvml_api.library) dlclose(nvml_api.library);
    memset(&nvml_api, 0, sizeof(nvml_api));
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
    if (item->pid == 0 || item->pid == (unsigned int)getpid()) return;
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
    if (nvml_api.device_get_compute_processes_v3 &&
        query_process_api(gpu->handle, nvml_api.device_get_compute_processes_v3, items, &count)) {
        for (unsigned int i = 0; i < count; i++) add_nvml_process(gpu, &items[i]);
    }
    count = ARRAY_LEN(items);
    if (nvml_api.device_get_graphics_processes_v3 &&
        query_process_api(gpu->handle, nvml_api.device_get_graphics_processes_v3, items, &count)) {
        for (unsigned int i = 0; i < count; i++) add_nvml_process(gpu, &items[i]);
    }
}

static void mark_idle_gpu_processes(GpuInfo *gpu) {
    if (!gpu->utilization_valid || gpu->utilization > 0.0) return;
    for (size_t i = 0; i < gpu->process_count; i++) {
        if (!gpu->processes[i].sm_valid) {
            gpu->processes[i].sm = 0.0;
            gpu->processes[i].sm_valid = true;
        }
    }
}

static void collect_process_sm(GpuInfo *gpu) {
    if (!nvml_api.device_get_process_utilization) {
        mark_idle_gpu_processes(gpu);
        return;
    }
    nvmlProcessUtilizationSample_t *samples = NULL;
    unsigned int count = 32;
    for (int attempt = 0; attempt < 4; attempt++) {
        samples = calloc(count, sizeof(*samples));
        if (!samples) {
            mark_idle_gpu_processes(gpu);
            return;
        }
        unsigned int requested = count;
        nvmlReturn_t result = nvml_api.device_get_process_utilization(gpu->handle, samples, &requested, 0);
        if (result == NVML_SUCCESS) {
            mark_idle_gpu_processes(gpu);
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
        if (result != NVML_ERROR_INSUFFICIENT_SIZE) {
            mark_idle_gpu_processes(gpu);
            return;
        }
        count = requested + 16;
    }
    mark_idle_gpu_processes(gpu);
}
#endif

#if defined(CTOP_ENABLE_VULKAN)
/* ------------------------------------------------------------------------- */
/* ROCm (AMD SMI) backend — optional runtime data source for AMD telemetry   */
/*                                                                           */
/* Loaded with dlopen(); no build-time headers or link libraries are needed. */
/* libamd_smi.so (ROCm 6.4+) also exports the legacy rsmi_* API, so one      */
/* symbol list covers both candidates. Enum arguments are int-sized at the   */
/* SysV x86-64 ABI; the constants below match the installed ROCm headers.    */
/* Every metric degrades back to the Vulkan/sysfs path when unavailable.     */

#define ROCM_STATUS_SUCCESS 0
#define ROCM_TEMP_METRIC_CURRENT 0
#define ROCM_TEMP_SENSOR_EDGE 0
#define ROCM_TEMP_SENSOR_JUNCTION 1
#define ROCM_MEM_TYPE_VRAM 0
#define ROCM_MEM_TYPE_GTT 2

typedef struct {
    void *library;
    int (*init)(uint64_t);
    int (*shut_down)(void);
    int (*num_monitor_devices)(uint32_t *);
    int (*dev_pci_id_get)(uint32_t, uint64_t *);
    int (*status_string)(int, const char **);
    int (*dev_temp_metric_get)(uint32_t, uint32_t, int, int64_t *);
    int (*dev_power_ave_get)(uint32_t, uint32_t, uint64_t *);
    int (*dev_power_cap_get)(uint32_t, uint32_t, uint64_t *);
    int (*dev_memory_total_get)(uint32_t, int, uint64_t *);
    int (*dev_memory_usage_get)(uint32_t, int, uint64_t *);
    int (*dev_busy_percent_get)(uint32_t, uint32_t *);
} RocmApi;

static RocmApi rocm_api;
static bool rocm_initialized;

static const char *rocm_error_string(int status) {
    static char fallback[48];
    const char *text = NULL;
    if (rocm_api.status_string &&
        rocm_api.status_string(status, &text) == ROCM_STATUS_SUCCESS && text)
        return text;
    snprintf(fallback, sizeof(fallback), "status=%d", status);
    return fallback;
}

static bool initialize_rocm_backend(char *error, size_t error_size) {
    const char *libraries[] = {
        "/opt/rocm/lib/libamd_smi.so",
        "/opt/rocm/lib/librocm_smi64.so.1",
        "libamd_smi.so",
        "librocm_smi64.so.1",
    };
    for (size_t i = 0; i < ARRAY_LEN(libraries) && !rocm_api.library; i++)
        rocm_api.library = dlopen(libraries[i], RTLD_NOW | RTLD_LOCAL);
    if (!rocm_api.library) {
        snprintf(error, error_size, "ROCm library unavailable: %s", dlerror());
        return false;
    }
#define LOAD_ROCM(member, symbol) do { \
        void *address = dlsym(rocm_api.library, symbol); \
        if (!address) { \
            snprintf(error, error_size, "ROCm symbol %s unavailable", symbol); \
            goto failed; \
        } \
        _Static_assert(sizeof(rocm_api.member) == sizeof(address), "POSIX function pointer size mismatch"); \
        memcpy(&rocm_api.member, &address, sizeof(address)); \
    } while (0)
#define LOAD_ROCM_OPTIONAL(member, symbol) do { \
        void *address = dlsym(rocm_api.library, symbol); \
        if (address) memcpy(&rocm_api.member, &address, sizeof(address)); \
    } while (0)
    LOAD_ROCM(init, "rsmi_init");
    LOAD_ROCM(shut_down, "rsmi_shut_down");
    LOAD_ROCM(num_monitor_devices, "rsmi_num_monitor_devices");
    LOAD_ROCM(dev_pci_id_get, "rsmi_dev_pci_id_get");
    LOAD_ROCM_OPTIONAL(status_string, "rsmi_status_string");
    LOAD_ROCM_OPTIONAL(dev_temp_metric_get, "rsmi_dev_temp_metric_get");
    LOAD_ROCM_OPTIONAL(dev_power_ave_get, "rsmi_dev_power_ave_get");
    LOAD_ROCM_OPTIONAL(dev_power_cap_get, "rsmi_dev_power_cap_get");
    LOAD_ROCM_OPTIONAL(dev_memory_total_get, "rsmi_dev_memory_total_get");
    LOAD_ROCM_OPTIONAL(dev_memory_usage_get, "rsmi_dev_memory_usage_get");
    LOAD_ROCM_OPTIONAL(dev_busy_percent_get, "rsmi_dev_busy_percent_get");

    int result = rocm_api.init(0);
    if (result != ROCM_STATUS_SUCCESS) {
        snprintf(error, error_size, "ROCm initialization failed: %s", rocm_error_string(result));
        goto failed;
    }
    rocm_initialized = true;
    return true;

failed:
    dlclose(rocm_api.library);
    memset(&rocm_api, 0, sizeof(rocm_api));
    return false;
}

static void shutdown_rocm_backend(void) {
    if (rocm_initialized && rocm_api.shut_down) rocm_api.shut_down();
    rocm_initialized = false;
    if (rocm_api.library) dlclose(rocm_api.library);
    memset(&rocm_api, 0, sizeof(rocm_api));
}

/* ROCm BDFID layout: domain<<32 | partition<<28 | bus<<8 | device<<3 | function.
 * Single-partition desktop and APUs carry a zero partition, matching the
 * domain:bus:device.function slot name used for the Vulkan/sysfs path. */
static bool rocm_pci_bus_id_to_bdfid(const char *pci_bus_id, uint64_t *bdfid) {
    unsigned int domain = 0, bus = 0, device = 0, function = 0;
    if (sscanf(pci_bus_id, "%4x:%2x:%2x.%1u", &domain, &bus, &device, &function) != 4)
        return false;
    *bdfid = ((uint64_t)domain << 32) | ((uint64_t)bus << 8) | ((uint64_t)device << 3) | function;
    return true;
}

static uint32_t rocm_match_device_index(const GpuInfo *gpu) {
    uint64_t wanted = 0;
    if (!gpu->pci_bus_id[0] || !rocm_pci_bus_id_to_bdfid(gpu->pci_bus_id, &wanted))
        return UINT32_MAX;
    uint32_t count = 0;
    if (rocm_api.num_monitor_devices(&count) != ROCM_STATUS_SUCCESS)
        return UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t bdfid = 0;
        if (rocm_api.dev_pci_id_get(i, &bdfid) == ROCM_STATUS_SUCCESS && bdfid == wanted)
            return i;
    }
    return UINT32_MAX;
}

static void rocm_enrich_amd_gpu(GpuInfo *gpu) {
    uint32_t dv_ind = rocm_match_device_index(gpu);
    if (dv_ind == UINT32_MAX) return;
    bool used = false;

    int64_t millidegrees = 0;
    if (rocm_api.dev_temp_metric_get &&
        (rocm_api.dev_temp_metric_get(dv_ind, ROCM_TEMP_SENSOR_EDGE,
                                      ROCM_TEMP_METRIC_CURRENT, &millidegrees) == ROCM_STATUS_SUCCESS ||
         rocm_api.dev_temp_metric_get(dv_ind, ROCM_TEMP_SENSOR_JUNCTION,
                                      ROCM_TEMP_METRIC_CURRENT, &millidegrees) == ROCM_STATUS_SUCCESS)) {
        gpu->temperature = (double)millidegrees / 1000.0;
        gpu->temperature_valid = true;
        used = true;
    }
    uint64_t microwatts = 0;
    if (rocm_api.dev_power_ave_get &&
        rocm_api.dev_power_ave_get(dv_ind, 0, &microwatts) == ROCM_STATUS_SUCCESS) {
        gpu->power = (double)microwatts / 1000000.0;
        gpu->power_valid = true;
        used = true;
    }
    if (rocm_api.dev_power_cap_get &&
        rocm_api.dev_power_cap_get(dv_ind, 0, &microwatts) == ROCM_STATUS_SUCCESS) {
        gpu->power_limit = (double)microwatts / 1000000.0;
        gpu->power_limit_valid = true;
        used = true;
    }
    if (rocm_api.dev_memory_total_get && rocm_api.dev_memory_usage_get) {
        int mem_type = gpu->prefer_gtt ? ROCM_MEM_TYPE_GTT : ROCM_MEM_TYPE_VRAM;
        uint64_t total = 0, used_bytes = 0;
        if (rocm_api.dev_memory_total_get(dv_ind, mem_type, &total) == ROCM_STATUS_SUCCESS &&
            rocm_api.dev_memory_usage_get(dv_ind, mem_type, &used_bytes) == ROCM_STATUS_SUCCESS && total) {
            gpu->memory_total = total;
            gpu->memory_used = used_bytes;
            gpu->memory_valid = true;
            used = true;
        }
    }
    uint32_t busy_percent = 0;
    if (rocm_api.dev_busy_percent_get &&
        rocm_api.dev_busy_percent_get(dv_ind, &busy_percent) == ROCM_STATUS_SUCCESS &&
        busy_percent <= 100) {
        gpu->utilization = busy_percent;
        gpu->utilization_valid = true;
        used = true;
    }
    if (used) snprintf(gpu->data_source, sizeof(gpu->data_source), "rocm");
}
#endif

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

static bool is_ctop_command(const char *command) {
    if (!command) return false;
    while (isspace((unsigned char)*command)) command++;
    const char *end = command;
    while (*end && !isspace((unsigned char)*end)) end++;
    const char *base = command;
    for (const char *p = command; p < end; p++)
        if (*p == '/') base = p + 1;
    return (size_t)(end - base) == 4 && strncasecmp(base, "ctop", 4) == 0;
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

#if defined(CTOP_ENABLE_VULKAN)
typedef struct {
    char pci_bus_id[24];
    char sysfs_path[PATH_MAX];
    uint32_t device_id;
    bool used;
} DrmDeviceInfo;

typedef struct {
    char driver[32];
    char pci_bus_id[24];
    unsigned long long gfx_engine_ns;
    unsigned long long memory_vram_kib;
    unsigned long long memory_gtt_kib;
    bool gfx_engine_valid;
    bool memory_vram_valid;
    bool memory_gtt_valid;
} DrmFdInfo;

static bool read_unsigned_file(const char *path, unsigned long long *value) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool valid = fscanf(file, "%llu", value) == 1;
    fclose(file);
    return valid;
}

static bool read_hex_file(const char *path, uint32_t *value) {
    char text[64];
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool read_ok = fgets(text, sizeof(text), file) != NULL;
    fclose(file);
    if (!read_ok) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool read_pci_slot_name(const char *path, char *output, size_t output_size) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
            const char *slot = line + 14;
            size_t length = strcspn(slot, "\r\n");
            if (length >= output_size) break;
            memcpy(output, slot, length);
            output[length] = '\0';
            found = length > 0;
            break;
        }
    }
    fclose(file);
    return found;
}

static int compare_drm_devices(const void *left, const void *right) {
    const DrmDeviceInfo *a = left;
    const DrmDeviceInfo *b = right;
    return strcmp(a->pci_bus_id, b->pci_bus_id);
}

static int enumerate_amd_drm_devices(DrmDeviceInfo *devices, int capacity) {
    DIR *drm_directory = opendir("/sys/class/drm");
    if (!drm_directory) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(drm_directory)) != NULL && count < capacity) {
        if (strncmp(entry->d_name, "card", 4) != 0 || !isdigit((unsigned char)entry->d_name[4])) continue;
        bool card_number = true;
        for (const char *p = entry->d_name + 4; *p; p++)
            if (!isdigit((unsigned char)*p)) { card_number = false; break; }
        if (!card_number) continue;

        char device_path[PATH_MAX], vendor_path[PATH_MAX + 16], id_path[PATH_MAX + 16], uevent_path[PATH_MAX + 16];
        uint32_t vendor = 0, device_id = 0;
        snprintf(device_path, sizeof(device_path), "/sys/class/drm/%s/device", entry->d_name);
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_path);
        snprintf(id_path, sizeof(id_path), "%s/device", device_path);
        snprintf(uevent_path, sizeof(uevent_path), "%s/uevent", device_path);
        if (!read_hex_file(vendor_path, &vendor) || vendor != 0x1002 ||
            !read_hex_file(id_path, &device_id)) continue;

        DrmDeviceInfo *device = &devices[count];
        memset(device, 0, sizeof(*device));
        char resolved[PATH_MAX];
        if (!realpath(device_path, resolved)) continue;
        snprintf(device->sysfs_path, sizeof(device->sysfs_path), "%s", resolved);
        if (!read_pci_slot_name(uevent_path, device->pci_bus_id, sizeof(device->pci_bus_id))) {
            const char *basename = strrchr(resolved, '/');
            if (!basename || strlen(basename + 1) >= sizeof(device->pci_bus_id)) continue;
            snprintf(device->pci_bus_id, sizeof(device->pci_bus_id), "%s", basename + 1);
        }
        device->device_id = device_id;
        count++;
    }
    closedir(drm_directory);
    qsort(devices, (size_t)count, sizeof(*devices), compare_drm_devices);
    return count;
}

static bool vulkan_device_has_extension(VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL) != VK_SUCCESS || !count)
        return false;
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    if (!extensions) return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(device, NULL, &count, extensions) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(extensions[i].extensionName, name) == 0) { found = true; break; }
        }
    }
    free(extensions);
    return found;
}

static bool initialize_vulkan_backend(char *error, size_t error_size) {
    VkApplicationInfo application = {0};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "ctop";
    application.applicationVersion = VK_MAKE_VERSION(0, 3, 2);
    application.pEngineName = "ctop";
    application.engineVersion = VK_MAKE_VERSION(0, 3, 2);
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create_info = {0};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    VkResult result = vkCreateInstance(&create_info, NULL, &vulkan_instance);
    if (result != VK_SUCCESS) {
        snprintf(error, error_size, "Vulkan instance creation failed (%d)", result);
        return false;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(vulkan_instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        snprintf(error, error_size, "Vulkan found no physical devices (%d)", result);
        return false;
    }
    VkPhysicalDevice *physical_devices = calloc(count, sizeof(*physical_devices));
    if (!physical_devices) {
        snprintf(error, error_size, "Vulkan device list allocation failed");
        return false;
    }
    result = vkEnumeratePhysicalDevices(vulkan_instance, &count, physical_devices);
    if (result != VK_SUCCESS) {
        snprintf(error, error_size, "Vulkan device enumeration failed (%d)", result);
        free(physical_devices);
        return false;
    }

    DrmDeviceInfo drm_devices[MAX_GPUS] = {0};
    int drm_count = enumerate_amd_drm_devices(drm_devices, MAX_GPUS);
    for (uint32_t i = 0; i < count && vulkan_gpu_count < MAX_GPUS; i++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
        if (properties.vendorID != 0x1002) continue;

        VulkanGpu *gpu = &vulkan_gpus[vulkan_gpu_count];
        memset(gpu, 0, sizeof(*gpu));
        gpu->physical_device = physical_devices[i];
        gpu->device_id = properties.deviceID;
        size_t name_length = strnlen(properties.deviceName, sizeof(properties.deviceName));
        if (name_length >= sizeof(gpu->name)) name_length = sizeof(gpu->name) - 1;
        memcpy(gpu->name, properties.deviceName, name_length);
        gpu->name[name_length] = '\0';

        if (vulkan_device_has_extension(physical_devices[i], VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
            VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {0};
            pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties2 = {0};
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &pci;
            vkGetPhysicalDeviceProperties2(physical_devices[i], &properties2);
            snprintf(gpu->pci_bus_id, sizeof(gpu->pci_bus_id), "%04x:%02x:%02x.%x",
                     pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction);
        }

        DrmDeviceInfo *matched = NULL;
        for (int d = 0; d < drm_count; d++) {
            if (!drm_devices[d].used && gpu->pci_bus_id[0] &&
                strcasecmp(gpu->pci_bus_id, drm_devices[d].pci_bus_id) == 0) {
                matched = &drm_devices[d];
                break;
            }
        }
        if (!matched) {
            for (int d = 0; d < drm_count; d++) {
                if (!drm_devices[d].used && drm_devices[d].device_id == gpu->device_id) {
                    matched = &drm_devices[d];
                    break;
                }
            }
        }
        if (matched) {
            matched->used = true;
            snprintf(gpu->pci_bus_id, sizeof(gpu->pci_bus_id), "%s", matched->pci_bus_id);
            snprintf(gpu->sysfs_path, sizeof(gpu->sysfs_path), "%s", matched->sysfs_path);
        }
        gpu->prefer_gtt = gpu->device_id == 0x13fe;
        vulkan_gpu_count++;
    }
    free(physical_devices);
    return true;
}

static void read_amd_hwmon(GpuInfo *gpu) {
    if (!gpu->sysfs_path[0]) return;
    char hwmon_path[PATH_MAX + 64];
    snprintf(hwmon_path, sizeof(hwmon_path), "%s/hwmon", gpu->sysfs_path);
    DIR *directory = opendir(hwmon_path);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
        char sensor_path[PATH_MAX + 512];
        for (int sensor = 1; sensor <= 8 && !gpu->temperature_valid; sensor++) {
            unsigned long long value = 0;
            snprintf(sensor_path, sizeof(sensor_path), "%s/%s/temp%d_input", hwmon_path, entry->d_name, sensor);
            if (read_unsigned_file(sensor_path, &value) && value <= 200000) {
                gpu->temperature = (double)value / 1000.0;
                gpu->temperature_valid = true;
            }
        }
        for (int sensor = 1; sensor <= 8 && !gpu->power_valid; sensor++) {
            unsigned long long value = 0;
            snprintf(sensor_path, sizeof(sensor_path), "%s/%s/power%d_average", hwmon_path, entry->d_name, sensor);
            if (!read_unsigned_file(sensor_path, &value)) {
                snprintf(sensor_path, sizeof(sensor_path), "%s/%s/power%d_input", hwmon_path, entry->d_name, sensor);
                if (!read_unsigned_file(sensor_path, &value)) continue;
            }
            gpu->power = (double)value / 1000000.0;
            gpu->power_valid = true;
        }
        if (!gpu->power_limit_valid) {
            unsigned long long value = 0;
            snprintf(sensor_path, sizeof(sensor_path), "%s/%s/power1_cap", hwmon_path, entry->d_name);
            if (!read_unsigned_file(sensor_path, &value)) {
                snprintf(sensor_path, sizeof(sensor_path), "%s/%s/power1_cap_max", hwmon_path, entry->d_name);
                if (!read_unsigned_file(sensor_path, &value)) continue;
            }
            gpu->power_limit = (double)value / 1000000.0;
            gpu->power_limit_valid = true;
        }
    }
    closedir(directory);
}

static void read_amd_gpu_metrics(GpuInfo *gpu) {
    if (!gpu->sysfs_path[0]) return;
    char path[PATH_MAX + 64];
    unsigned long long vram_used = 0, vram_total = 0, gtt_used = 0, gtt_total = 0;
    snprintf(path, sizeof(path), "%s/mem_info_vram_used", gpu->sysfs_path);
    bool have_vram_used = read_unsigned_file(path, &vram_used);
    snprintf(path, sizeof(path), "%s/mem_info_vram_total", gpu->sysfs_path);
    bool have_vram_total = read_unsigned_file(path, &vram_total);
    snprintf(path, sizeof(path), "%s/mem_info_gtt_used", gpu->sysfs_path);
    bool have_gtt_used = read_unsigned_file(path, &gtt_used);
    snprintf(path, sizeof(path), "%s/mem_info_gtt_total", gpu->sysfs_path);
    bool have_gtt_total = read_unsigned_file(path, &gtt_total);

    if (gpu->prefer_gtt && have_gtt_used && have_gtt_total && gtt_total) {
        gpu->memory_used = gtt_used;
        gpu->memory_total = gtt_total;
        gpu->memory_valid = true;
    } else if (have_vram_used && have_vram_total && vram_total) {
        gpu->memory_used = vram_used;
        gpu->memory_total = vram_total;
        gpu->memory_valid = true;
    } else if (have_gtt_used && have_gtt_total && gtt_total) {
        gpu->memory_used = gtt_used;
        gpu->memory_total = gtt_total;
        gpu->memory_valid = true;
    }

    unsigned long long busy = 0;
    snprintf(path, sizeof(path), "%s/gpu_busy_percent", gpu->sysfs_path);
    if (read_unsigned_file(path, &busy) && busy <= 100) {
        gpu->utilization = (double)busy;
        gpu->utilization_valid = true;
    }
    read_amd_hwmon(gpu);
}

static GpuInfo *find_amd_gpu_by_pci(GpuInfo *gpus, int gpu_count, const char *pci_bus_id) {
    if (!pci_bus_id || !pci_bus_id[0]) return NULL;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].is_amd && strcasecmp(gpus[i].pci_bus_id, pci_bus_id) == 0)
            return &gpus[i];
    }
    return NULL;
}

static ProcessInfo *find_or_add_gpu_process(GpuInfo *gpu, pid_t pid) {
    for (size_t i = 0; i < gpu->process_count; i++)
        if (gpu->processes[i].pid == pid) return &gpu->processes[i];
    if (gpu->process_count >= MAX_GPU_PROCESSES) return NULL;
    ProcessInfo *process = &gpu->processes[gpu->process_count++];
    memset(process, 0, sizeof(*process));
    process->pid = pid;
    return process;
}

static bool read_drm_fdinfo(const char *path, DrmFdInfo *info) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "drm-driver:", 11) == 0) {
            sscanf(line + 11, "%31s", info->driver);
        } else if (strncmp(line, "drm-pdev:", 9) == 0) {
            sscanf(line + 9, "%23s", info->pci_bus_id);
        } else if (strncmp(line, "drm-engine-gfx:", 15) == 0) {
            info->gfx_engine_valid = sscanf(line + 15, "%llu", &info->gfx_engine_ns) == 1;
        } else if (strncmp(line, "drm-memory-vram:", 16) == 0) {
            info->memory_vram_valid = sscanf(line + 16, "%llu", &info->memory_vram_kib) == 1;
        } else if (strncmp(line, "drm-memory-gtt:", 15) == 0) {
            info->memory_gtt_valid = sscanf(line + 15, "%llu", &info->memory_gtt_kib) == 1;
        }
    }
    fclose(file);
    return true;
}

static DrmProcessPrevious *find_drm_process_previous(pid_t pid, const char *pci_bus_id) {
    for (size_t i = 0; i < drm_process_previous_count; i++) {
        if (drm_process_previous[i].pid == pid &&
            strcasecmp(drm_process_previous[i].pci_bus_id, pci_bus_id) == 0)
            return &drm_process_previous[i];
    }
    if (drm_process_previous_count >= ARRAY_LEN(drm_process_previous)) return NULL;
    DrmProcessPrevious *previous = &drm_process_previous[drm_process_previous_count++];
    memset(previous, 0, sizeof(*previous));
    previous->pid = pid;
    snprintf(previous->pci_bus_id, sizeof(previous->pci_bus_id), "%s", pci_bus_id);
    return previous;
}

static void collect_amd_processes(GpuInfo *gpus, int gpu_count) {
    DIR *proc_directory = opendir("/proc");
    if (!proc_directory) return;
    double now = monotonic_seconds();
    if (++drm_sample_generation == 0) drm_sample_generation = 1;
    struct dirent *process_entry;
    while ((process_entry = readdir(proc_directory)) != NULL) {
        char *end = NULL;
        long raw_pid = strtol(process_entry->d_name, &end, 10);
        if (end == process_entry->d_name || *end || raw_pid <= 0 || raw_pid > INT_MAX) continue;
        pid_t pid = (pid_t)raw_pid;
        if (pid == getpid()) continue;
        char fdinfo_path[PATH_MAX];
        snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%ld/fdinfo", raw_pid);
        DIR *fdinfo_directory = opendir(fdinfo_path);
        if (!fdinfo_directory) continue;
        struct dirent *fdinfo_entry;
        while ((fdinfo_entry = readdir(fdinfo_directory)) != NULL) {
            if (fdinfo_entry->d_name[0] == '.') continue;
            char path[PATH_MAX + 256];
            snprintf(path, sizeof(path), "%s/%s", fdinfo_path, fdinfo_entry->d_name);
            DrmFdInfo info = {0};
            if (!read_drm_fdinfo(path, &info) || strcmp(info.driver, "amdgpu") != 0) continue;
            GpuInfo *gpu = find_amd_gpu_by_pci(gpus, gpu_count, info.pci_bus_id);
            if (!gpu) continue;
            if (!info.gfx_engine_valid && !info.memory_vram_valid && !info.memory_gtt_valid) continue;
            ProcessInfo *process = find_or_add_gpu_process(gpu, pid);
            if (!process) continue;
            if (info.gfx_engine_valid) {
                process->drm_gfx_engine_ns += info.gfx_engine_ns;
                process->drm_gfx_engine_valid = true;
            }
            if (info.memory_vram_valid) {
                process->drm_memory_vram_kib += info.memory_vram_kib;
                process->drm_memory_vram_valid = true;
            }
            if (info.memory_gtt_valid) {
                process->drm_memory_gtt_kib += info.memory_gtt_kib;
                process->drm_memory_gtt_valid = true;
            }
        }
        closedir(fdinfo_directory);
    }
    closedir(proc_directory);

    for (int i = 0; i < gpu_count; i++) {
        GpuInfo *gpu = &gpus[i];
        if (!gpu->is_amd) continue;
        double process_utilization = 0.0;
        bool process_utilization_valid = false;
        for (size_t j = 0; j < gpu->process_count; j++) {
            ProcessInfo *process = &gpu->processes[j];
            if (process->drm_memory_gtt_valid && gpu->prefer_gtt &&
                process->drm_memory_gtt_kib <= ULLONG_MAX / 1024ULL) {
                process->gpu_memory = process->drm_memory_gtt_kib * 1024ULL;
                process->gpu_memory_valid = true;
            } else if (process->drm_memory_vram_valid &&
                       process->drm_memory_vram_kib <= ULLONG_MAX / 1024ULL) {
                process->gpu_memory = process->drm_memory_vram_kib * 1024ULL;
                process->gpu_memory_valid = true;
            } else if (process->drm_memory_gtt_valid &&
                       process->drm_memory_gtt_kib <= ULLONG_MAX / 1024ULL) {
                process->gpu_memory = process->drm_memory_gtt_kib * 1024ULL;
                process->gpu_memory_valid = true;
            }
            if (process->drm_gfx_engine_valid) {
                DrmProcessPrevious *previous = find_drm_process_previous(process->pid, gpu->pci_bus_id);
                if (previous) {
                    if (previous->generation && now > previous->timestamp &&
                        process->drm_gfx_engine_ns >= previous->engine_ns) {
                        double elapsed = now - previous->timestamp;
                        process->sm = 100.0 * (double)(process->drm_gfx_engine_ns - previous->engine_ns) /
                                      (elapsed * 1000000000.0);
                        clamp_percent(&process->sm);
                        process->sm_valid = true;
                    }
                    previous->engine_ns = process->drm_gfx_engine_ns;
                    previous->timestamp = now;
                    previous->generation = drm_sample_generation;
                }
            }
            if (process->sm_valid) {
                process_utilization += process->sm;
                process_utilization_valid = true;
            }
        }
        if (!gpu->utilization_valid && process_utilization_valid) {
            clamp_percent(&process_utilization);
            gpu->utilization = process_utilization;
            gpu->utilization_valid = true;
        }
    }

    size_t kept = 0;
    for (size_t i = 0; i < drm_process_previous_count; i++) {
        if (drm_process_previous[i].generation == drm_sample_generation)
            drm_process_previous[kept++] = drm_process_previous[i];
    }
    drm_process_previous_count = kept;
}

static void append_vulkan_gpus(GpuInfo *gpus, int *gpu_count) {
    for (int i = 0; i < vulkan_gpu_count && *gpu_count < MAX_GPUS; i++) {
        VulkanGpu *device = &vulkan_gpus[i];
        GpuInfo *gpu = &gpus[*gpu_count];
        memset(gpu, 0, sizeof(*gpu));
        gpu->index = *gpu_count;
        gpu->is_amd = true;
        gpu->prefer_gtt = device->prefer_gtt;
        snprintf(gpu->name, sizeof(gpu->name), "%s", device->name);
        snprintf(gpu->pci_bus_id, sizeof(gpu->pci_bus_id), "%s", device->pci_bus_id);
        snprintf(gpu->sysfs_path, sizeof(gpu->sysfs_path), "%s", device->sysfs_path);
        read_amd_gpu_metrics(gpu);
        snprintf(gpu->data_source, sizeof(gpu->data_source), "sysfs");
        rocm_enrich_amd_gpu(gpu);
        (*gpu_count)++;
    }
}
#endif

static void shutdown_gpu_backends(void) {
#if defined(CTOP_ENABLE_NVML)
    shutdown_nvml_backend();
#endif
#if defined(CTOP_ENABLE_VULKAN)
    shutdown_rocm_backend();
    if (vulkan_instance) vkDestroyInstance(vulkan_instance, NULL);
    vulkan_instance = VK_NULL_HANDLE;
    vulkan_gpu_count = 0;
#endif
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

static DiskPrevious *find_disk_previous(const char *name) {
    for (size_t i = 0; i < disk_previous_count; i++) {
        if (strcmp(disk_previous[i].name, name) == 0) return &disk_previous[i];
    }
    if (disk_previous_count >= ARRAY_LEN(disk_previous)) return NULL;
    DiskPrevious *previous = &disk_previous[disk_previous_count++];
    memset(previous, 0, sizeof(*previous));
    snprintf(previous->name, sizeof(previous->name), "%s", name);
    return previous;
}

static bool ignored_disk_device(const char *name) {
    return strncmp(name, "loop", 4) == 0 ||
           strncmp(name, "ram", 3) == 0 ||
           strncmp(name, "zram", 4) == 0;
}

static bool disk_device_is_partition(const char *name) {
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    FILE *file = fopen(path, "r");
    if (!file) return false;
    fclose(file);
    return true;
}

static bool disk_filesystem_is_virtual(const char *type) {
    static const char *const virtual_types[] = {
        "aufs", "autofs", "binfmt_misc", "bpf", "cgroup", "cgroup2",
        "configfs", "composefs", "debugfs", "devpts", "devtmpfs", "efivarfs",
        "fusectl", "hugetlbfs", "mqueue", "nsfs", "overlay", "proc", "pstore",
        "ramfs", "rootfs", "rpc_pipefs", "securityfs", "selinuxfs", "squashfs",
        "sysfs", "tmpfs", "tracefs",
    };
    for (size_t i = 0; i < ARRAY_LEN(virtual_types); i++)
        if (strcmp(type, virtual_types[i]) == 0) return true;
    return false;
}

static bool read_disk_capacity(DiskInfo *disk, const char *path) {
    struct statvfs filesystem;
    if (statvfs(path, &filesystem) != 0 || filesystem.f_frsize == 0) return false;
    unsigned long long block_size = (unsigned long long)filesystem.f_frsize;
    unsigned long long total = (unsigned long long)filesystem.f_blocks * block_size;
    unsigned long long free_bytes = (unsigned long long)filesystem.f_bfree * block_size;
    disk->total_bytes = total;
    disk->used_bytes = total > free_bytes ? total - free_bytes : 0;
    disk->capacity_valid = total > 0;
    return disk->capacity_valid;
}

static bool read_disk_capacity_from_mounts(DiskInfo *disk) {
    FILE *mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) return read_disk_capacity(disk, "/");

    char root_type[64] = "";
    struct mntent *entry;
    while ((entry = getmntent(mounts))) {
        if (strcmp(entry->mnt_dir, "/") == 0) {
            snprintf(root_type, sizeof(root_type), "%s", entry->mnt_type);
            break;
        }
    }
    endmntent(mounts);
    if (!root_type[0] || !disk_filesystem_is_virtual(root_type))
        return read_disk_capacity(disk, "/");

    /* A virtual root (composefs/overlay/etc.) is not the backing storage.
       Choose the largest real filesystem visible in this mount namespace. */
    mounts = setmntent("/proc/self/mounts", "r");
    if (!mounts) return false;
    DiskInfo best = {0};
    while ((entry = getmntent(mounts))) {
        if (strcmp(entry->mnt_dir, "/") == 0 ||
            disk_filesystem_is_virtual(entry->mnt_type)) continue;
        DiskInfo candidate = {0};
        if (read_disk_capacity(&candidate, entry->mnt_dir) &&
            candidate.total_bytes > best.total_bytes) {
            best = candidate;
        }
    }
    endmntent(mounts);
    if (!best.capacity_valid) return false;
    disk->used_bytes = best.used_bytes;
    disk->total_bytes = best.total_bytes;
    disk->capacity_valid = true;
    return true;
}

static bool read_disk_info(DiskInfo *disk) {
    memset(disk, 0, sizeof(*disk));

    const char *configured = getenv("CTOP_DISK_PATH");
    if (configured && configured[0] && read_disk_capacity(disk, configured)) {
        /* Explicit path takes precedence over automatic mount discovery. */
    } else {
        read_disk_capacity_from_mounts(disk);
    }

    FILE *file = fopen("/proc/diskstats", "r");
    if (!file) return disk->capacity_valid;

    double timestamp = monotonic_seconds();
    bool have_valid_sample = false;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        unsigned int major = 0, minor = 0;
        char name[64];
        unsigned long long statistics[11] = {0};
        int fields = sscanf(line,
                            " %u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &major, &minor, name,
                            &statistics[0], &statistics[1], &statistics[2], &statistics[3],
                            &statistics[4], &statistics[5], &statistics[6], &statistics[7],
                            &statistics[8], &statistics[9], &statistics[10]);
        (void)major;
        (void)minor;
        if (fields < 10 || ignored_disk_device(name) || disk_device_is_partition(name)) continue;
        unsigned long long read_sectors = statistics[2];
        unsigned long long write_sectors = statistics[6];

        DiskPrevious *previous = find_disk_previous(name);
        bool valid = previous && previous->valid && timestamp > previous->timestamp &&
                     read_sectors >= previous->read_sectors &&
                     write_sectors >= previous->write_sectors;
        if (valid) {
            double elapsed = timestamp - previous->timestamp;
            disk->read_bps += (double)(read_sectors - previous->read_sectors) * 512.0 / elapsed;
            disk->write_bps += (double)(write_sectors - previous->write_sectors) * 512.0 / elapsed;
            have_valid_sample = true;
        }
        if (previous) {
            previous->read_sectors = read_sectors;
            previous->write_sectors = write_sectors;
            previous->timestamp = timestamp;
            previous->valid = true;
        }
    }
    fclose(file);
    disk->total_bps = disk->read_bps + disk->write_bps;
    disk->valid = have_valid_sample;
    return disk->capacity_valid || disk->valid;
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
    const char *period_labels[] = {"-400s", "-120s", "-30s", "-10s"};
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
    snprintf(labels[(*label_count)++], 16, "%s", period_labels[0]);
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
            snprintf(labels[*label_count], 16, "%s", period_labels[segment + 1]);
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
        /* Erase before drawing.  Erasing after a full-width write can erase
           the character in the terminal's last column while its wrap flag is
           pending, making the right edge look one cell short. */
        fputs("\033[K", stream);
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
    }
    free(overlay_at);
    for (size_t i = 0; i < marker_count; i++) {
        if (color_enabled) fputs("\033[1;36m", stream);
        fprintf(stream, "\033[%d;%dH%s", markers[i].row + 1, screen->cols,
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
                      bool graph, bool expanded, bool can_move_up, bool can_move_down,
                      Marker *markers, size_t *marker_count) {
    if (row < 0 || row >= screen->rows) return;
    for (int col = 0; col < screen->cols; col++) screen_ch(screen, row, col, '-');
    if (screen->cols > 0) screen_overlay(screen, row, 0, "▲", can_move_up ? ATTR_CYAN_BOLD : ATTR_DIM);
    if (screen->cols > 1) screen_ch(screen, row, 1, ' ');
    if (screen->cols > 2) screen_overlay(screen, row, 2, "▼", can_move_down ? ATTR_CYAN_BOLD : ATTR_DIM);
    if (screen->cols > 3) screen_ch(screen, row, 3, ' ');
    if (screen->cols > 1) screen_ch(screen, row, screen->cols - 1, '+');
    char title_text[256];
    snprintf(title_text, sizeof(title_text), "[ %s ]", title);
    screen_put_attr(screen, row, 4, title_text, screen->cols - 6, ATTR_CYAN_BOLD);
    if (graph && marker_count && *marker_count < MAX_DASHBOARD_SECTIONS) {
        markers[*marker_count] = (Marker){row, expanded};
        (*marker_count)++;
    }
}

static double history_peak_400(const History *history) {
    if (!history || !history->count) return 0.0;
    double reference = history_at(history, history->count - 1).timestamp;
    double peak = 0.0;
    for (size_t i = 0; i < history->count; i++) {
        Sample sample = history_at(history, i);
        double age = reference - sample.timestamp;
        if (age >= 0.0 && age < 400.0 && isfinite(sample.value) && sample.value > peak)
            peak = sample.value;
    }
    return peak;
}

typedef enum {
    GRAPH_AXIS_PERCENT,
    GRAPH_AXIS_DISK_RATE,
    GRAPH_AXIS_NETWORK_RATE,
} GraphAxis;

static void draw_plot_graph_scaled(Screen *screen, int top, int left, int width,
                                   const History *history, int graph_rows,
                                   double maximum, GraphAxis axis_type,
                                   const char *footer_label) {
    /* Use one common Y-axis width so all four graph plots have the same
       horizontal geometry. */
    int label_width = 8;
    if (width < label_width + 3 || graph_rows <= 0) return;
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

    double scale = maximum > 0.0 && isfinite(maximum) ? maximum : 1.0;
    for (int row = 0; row < graph_rows; row++) {
        double fraction = graph_rows == 1
                              ? 0.0
                              : (double)(graph_rows - 1 - row) / (double)(graph_rows - 1);
        double level = maximum > 0.0 ? maximum * fraction : 0.0;
        if (axis_type == GRAPH_AXIS_DISK_RATE || axis_type == GRAPH_AXIS_NETWORK_RATE) {
            char axis_label[32];
            if (axis_type == GRAPH_AXIS_NETWORK_RATE)
                format_network_axis_rate(axis_label, sizeof(axis_label), level, maximum);
            else
                format_disk_axis_rate(axis_label, sizeof(axis_label), level, maximum);
            screen_putf_attr(screen, top + row, left, label_width, ATTR_DIM,
                             "%*s ", label_width - 1, axis_label);
        } else {
            screen_putf_attr(screen, top + row, left, label_width, ATTR_DIM,
                             "%7d ", (int)llround(100.0 * fraction));
        }
        screen_ch_attr(screen, top + row, axis_col, '|', ATTR_DIM);
    }
    int bottom_row = top + graph_rows - 1;
    for (int x = 0; x < plot_width; x++) {
        screen_overlay(screen, bottom_row, plot_left + x, "_", ATTR_DIM);
        if (!valid[x]) continue;

        double value = values[x];
        double normalized = value / scale;
        if (!isfinite(normalized) || normalized < 0.0) normalized = 0.0;
        if (normalized > 1.0) normalized = 1.0;
        double level = normalized * graph_rows;
        int full_rows = (int)floor(level);
        double fraction = level - full_rows;
        if (full_rows > graph_rows) full_rows = graph_rows;
        unsigned char attr = load_attr(normalized * 100.0);

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
    if (footer_label && footer_label[0]) {
        /* Align the marker's final character with the final character in the
           right-aligned Y-axis label field. */
        int marker_length = (int)strlen(footer_label);
        int marker_left = left + label_width - 1 - marker_length;
        if (marker_left < left) marker_left = left;
        screen_put_attr(screen, footer, marker_left, footer_label,
                        marker_length, ATTR_DIM);
    }
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
                      const char *title_info, const char *footer_label) {
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
    draw_plot_graph_scaled(screen, top + 1, left, width, history, GRAPH_ROWS,
                           100.0, GRAPH_AXIS_PERCENT, footer_label);
}

static void draw_disk_plot(Screen *screen, int top, int left, int width,
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
    draw_plot_graph_scaled(screen, top + 1, left, width, history, GRAPH_ROWS,
                           history_peak_400(history), GRAPH_AXIS_DISK_RATE, "I/O Spd");
}

static void draw_load_bar(Screen *screen, int row, int start, double memory, double gpu) {
    int width = screen->cols - start;
    if (width < 10) return;
    int inner = width - 5;  /* two brackets and the center divider */
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

    int divider = start + 2 + left_width;
    screen_ch_attr(screen, row, start, '[', ATTR_DIM);
    screen_ch_attr(screen, row, start + 1 + left_width, ']', ATTR_NORMAL);
    screen_overlay(screen, row, divider, "\xE2\x94\x82", ATTR_CYAN_BOLD);
    screen_ch_attr(screen, row, divider + 1, '[', ATTR_NORMAL);
    screen_ch_attr(screen, row, screen->cols - 1, ']', ATTR_DIM);

    for (int i = 0; i < left_width; i++) {
        bool filled = i >= left_width - memory_cells;
        if (filled)
            screen_overlay(screen, row, start + 1 + i, "\xE2\x96\x88", memory_attr);
        else
            screen_ch_attr(screen, row, start + 1 + i, ' ', ATTR_NORMAL);
    }
    for (int i = 0; i < right_width; i++) {
        bool filled = i < gpu_cells;
        if (filled)
            screen_overlay(screen, row, divider + 2 + i, "\xE2\x96\x88", gpu_attr);
        else
            screen_ch_attr(screen, row, divider + 2 + i, ' ', ATTR_NORMAL);
    }
}

/* ------------------------------------------------------------------------- */
/* GPU and process rendering                                                 */

static void add_click_target(UiState *ui, int row, int left, int right, unsigned char key);

static void format_gpu_identifier(char *out, size_t size, const GpuInfo *gpu) {
    if (gpu->source_prefix[0]) snprintf(out, size, "%s%d", gpu->source_prefix, gpu->index);
    else snprintf(out, size, "%d", gpu->index);
}

static void draw_gpu_status(Screen *screen, int top, const GpuInfo *gpus, int gpu_count) {
    const int bar_start = 86;
    screen_put_attr(screen, top, 0, "GPU", 5, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 5, "NAME", 20, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 27, "TEMP", 5, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 34, "POWER DRAW/LIMIT", 16, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 52, "GPU MEM USED/TOTAL", 22, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 76, "GPU", 8, ATTR_CYAN_BOLD);
    int load_width = screen->cols - bar_start;
    if (load_width >= 20) {
        char load_header[MAX_SCREEN_COLS];
        memset(load_header, ' ', (size_t)load_width);
        load_header[load_width] = '\0';
        int divider = (load_width - 5) / 2 + 2;
        memcpy(load_header, "100%", 4);
        memcpy(load_header + divider - 1 - 4, "MEM ", 4);
        memcpy(load_header + divider + 2, "GPU", 3);
        memcpy(load_header + load_width - 4, "100%", 4);
        screen_put_attr(screen, top, bar_start, load_header, load_width, ATTR_CYAN_BOLD);
        screen_overlay(screen, top, bar_start + divider, "\xE2\x94\x82", ATTR_CYAN_BOLD);
    }

    for (int i = 0; i < gpu_count; i++) {
        const GpuInfo *gpu = &gpus[i];
        int row = top + 1 + i;
        char gpu_identifier[16];
        format_gpu_identifier(gpu_identifier, sizeof(gpu_identifier), gpu);
        screen_put_attr(screen, row, 0, gpu_identifier, 4, ATTR_CYAN);
        char name_label[128];
        if (gpu->data_source[0] &&
            strlen(gpu->name) + strlen(gpu->data_source) + 3 <= 20)
            snprintf(name_label, sizeof(name_label), "%s (%s)", gpu->name, gpu->data_source);
        else
            snprintf(name_label, sizeof(name_label), "%s", gpu->name);
        screen_put(screen, row, 5, name_label, 20);
        unsigned char temperature_attr = threshold_attr_value(gpu->temperature, gpu->temperature_valid, 70.0, 85.0);
        if (gpu->temperature_valid) screen_putf_attr(screen, row, 27, 5, temperature_attr, "%.0fC", gpu->temperature);
        else screen_put_attr(screen, row, 27, "N/A", 5, ATTR_DIM);
        char power[64], used[32], total[32];
        format_watts(power, sizeof(power), gpu->power, gpu->power_valid);
        char limit[32];
        format_watts(limit, sizeof(limit), gpu->power_limit, gpu->power_limit_valid);
        double power_ratio = (gpu->power_valid && gpu->power_limit_valid && gpu->power_limit > 0.0)
                                  ? 100.0 * gpu->power / gpu->power_limit : 0.0;
        bool power_ratio_valid = gpu->power_valid && gpu->power_limit_valid && gpu->power_limit > 0.0;
        unsigned char power_attr = power_ratio_valid
                                       ? threshold_attr_value(power_ratio, true, 90.0, 99.0)
                                       : (gpu->power_valid ? ATTR_TEXT_GREEN : ATTR_DIM);
        unsigned char limit_attr = power_ratio_valid
                                       ? power_attr
                                       : (gpu->power_limit_valid ? ATTR_TEXT_GREEN : ATTR_DIM);
        int power_length = (int)strlen(power);
        screen_put_attr(screen, row, 34, power, 8, power_attr);
        screen_put_attr(screen, row, 34 + power_length, "/", 1, ATTR_DIM);
        screen_put_attr(screen, row, 35 + power_length, limit, 8, limit_attr);
        bool memory_valid = gpu->memory_valid && gpu->memory_total > 0;
        if (memory_valid) {
            format_gib(used, sizeof(used), gpu->memory_used);
            format_gib(total, sizeof(total), gpu->memory_total);
            double memory = 100.0 * (double)gpu->memory_used / (double)gpu->memory_total;
            screen_putf_attr(screen, row, 52, 22, text_load_attr(memory), "%s/%s %3.0f%%", used, total, memory);
        } else screen_put_attr(screen, row, 52, "N/A", 22, ATTR_DIM);
        double gpu_util = gpu->utilization_valid ? gpu->utilization : 0.0;
        if (gpu->utilization_valid) screen_putf_attr(screen, row, 76, 8, text_load_attr(gpu_util), "%.1f%%", gpu_util);
        else screen_put_attr(screen, row, 76, "N/A", 8, ATTR_DIM);
        double memory = memory_valid
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
    screen_put_attr(screen, top, 36, "GPU-LOAD", 10, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 46, "CPU-AVG", 8, ATTR_CYAN_BOLD);
    screen_put_attr(screen, top, 54, "MEM", 7, ATTR_CYAN_BOLD);
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
            if (p->elapsed_display[0]) snprintf(elapsed, sizeof(elapsed), "%s", p->elapsed_display);
            else format_duration(elapsed, sizeof(elapsed), p->elapsed, p->elapsed_valid);
            char gpu_identifier[16];
            format_gpu_identifier(gpu_identifier, sizeof(gpu_identifier), &gpus[i]);
            screen_put_attr(screen, row, 0, gpu_identifier, 7, ATTR_CYAN);
            screen_putf(screen, row, 7, 8, "%-7ld", (long)p->pid);
            screen_put(screen, row, 15, p->user, 10);
            screen_put(screen, row, 25, memory, 9);
            if (p->sm_valid) screen_putf_attr(screen, row, 36, 10, text_load_attr(p->sm), "%5.1f%%", p->sm);
            else screen_put_attr(screen, row, 36, "--", 10, ATTR_DIM);
            if (p->cpu_valid) screen_putf(screen, row, 46, 8, "%5.1f%%", p->cpu);
            else screen_put_attr(screen, row, 46, "--", 8, ATTR_DIM);
            if (p->memory_valid) screen_putf(screen, row, 54, 7, "%5.1f%%", p->memory);
            else screen_put_attr(screen, row, 54, "--", 7, ATTR_DIM);
            screen_put(screen, row, 61, elapsed, 10);
            screen_put(screen, row, 71, p->command, screen->cols - 71);
            bool selected = ui && ui->selected_source_id == gpus[i].source_id &&
                            ui->selected_gpu == gpus[i].index && ui->selected_pid == p->pid;
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
        screen_put_attr(screen, row, 0, "No GPU processes reported by the active backend.", screen->cols, ATTR_DIM);
}

/* ------------------------------------------------------------------------- */
/* Layout and rendering                                                      */

static int system_content_rows(int interface_count, bool collapsed);

static const MonitorView *monitor_view_by_id(const MonitorView *views, int view_count, int id) {
    for (int i = 0; i < view_count; i++) if (views[i].id == id) return &views[i];
    return NULL;
}

static bool dashboard_section_collapsed(const DashboardSection *section, const UiState *ui) {
    if (section->type == SECTION_GPU) {
        if (section->source_id == 0) return ui->usage_collapsed;
        RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
        return monitor ? monitor->gpu_collapsed : false;
    }
    if (section->type == SECTION_SYSTEM) {
        if (section->source_id == 0) return ui->system_collapsed;
        RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
        return monitor ? monitor->system_collapsed : false;
    }
    if (section->type == SECTION_GPU_PROCESSES) {
        if (section->source_id == 0) return ui->processes_collapsed;
        RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
        return monitor ? monitor->processes_collapsed : false;
    }
    return false;
}

static void toggle_dashboard_section(const DashboardSection *section, UiState *ui) {
    if (section->type == SECTION_GPU) {
        if (section->source_id == 0) ui->usage_collapsed = !ui->usage_collapsed;
        else {
            RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
            if (monitor) monitor->gpu_collapsed = !monitor->gpu_collapsed;
        }
    } else if (section->type == SECTION_SYSTEM) {
        if (section->source_id == 0) ui->system_collapsed = !ui->system_collapsed;
        else {
            RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
            if (monitor) monitor->system_collapsed = !monitor->system_collapsed;
        }
    } else if (section->type == SECTION_GPU_PROCESSES) {
        if (section->source_id == 0) ui->processes_collapsed = !ui->processes_collapsed;
        else {
            RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
            if (monitor) monitor->processes_collapsed = !monitor->processes_collapsed;
        }
    }
}

static int source_process_count(const GpuInfo *gpus, const MonitorView *view) {
    int count = 0;
    for (int i = 0; i < view->gpu_count; i++)
        count += (int)gpus[view->gpu_start + i].process_count;
    return count;
}

static int dashboard_section_height(const DashboardSection *section, const UiState *ui,
                                    const GpuInfo *gpus, int gpu_count,
                                    const MonitorView *views, int view_count) {
    if (section->type == SECTION_GPU_LIST) return 2 + gpu_count;
    const MonitorView *view = monitor_view_by_id(views, view_count, section->source_id);
    if (!view) return 2;
    bool collapsed = dashboard_section_collapsed(section, ui);
    if (section->type == SECTION_GPU) return collapsed ? 2 : 3 + GRAPH_ROWS;
    if (section->type == SECTION_SYSTEM)
        return 1 + system_content_rows(view->network->interface_count, collapsed);
    if (section->type == SECTION_GPU_PROCESSES)
        return collapsed ? 2 : 2 + source_process_count(gpus, view);
    return 1;
}

static bool process_at_screen_row(int y, int gpu_count, const UiState *ui,
                                  const GpuInfo *gpus, const MonitorView *views,
                                  int view_count, int *gpu_index, size_t *process_index) {
    int row = y - 3;
    for (size_t s = 0; s < ui->section_count; s++) {
        const DashboardSection *section = &ui->sections[s];
        int height = dashboard_section_height(section, ui, gpus, gpu_count, views, view_count);
        if (section->type == SECTION_GPU_PROCESSES && !dashboard_section_collapsed(section, ui) && row >= 2) {
            const MonitorView *view = monitor_view_by_id(views, view_count, section->source_id);
            if (view) {
                int process_row = row - 2;
                for (int i = 0; i < view->gpu_count; i++) {
                    int global_index = view->gpu_start + i;
                    for (size_t j = 0; j < gpus[global_index].process_count; j++) {
                        if (process_row == 0) {
                            if (gpu_index) *gpu_index = global_index;
                            if (process_index) *process_index = j;
                            return true;
                        }
                        process_row--;
                    }
                }
            }
        }
        if (row < height) return false;
        row -= height;
    }
    return false;
}

static void select_process_at(UiState *ui, int y, int gpu_count, const GpuInfo *gpus,
                              const MonitorView *views, int view_count) {
    int gpu_index = -1;
    size_t process_index = 0;
    if (process_at_screen_row(y, gpu_count, ui, gpus, views, view_count,
                              &gpu_index, &process_index)) {
        ui->selected_source_id = gpus[gpu_index].source_id;
        ui->selected_gpu = gpus[gpu_index].index;
        ui->selected_pid = gpus[gpu_index].processes[process_index].pid;
        ui->notice[0] = '\0';
        return;
    }
    ui->selected_gpu = -1;
    ui->selected_source_id = 0;
    ui->selected_pid = 0;
}

static void move_process_selection(UiState *ui, const GpuInfo *gpus, int gpu_count, int direction) {
    int total = 0;
    int current = -1;
    for (int i = 0; i < gpu_count; i++) {
        for (size_t j = 0; j < gpus[i].process_count; j++) {
            if (gpus[i].source_id == ui->selected_source_id &&
                gpus[i].index == ui->selected_gpu && gpus[i].processes[j].pid == ui->selected_pid)
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
                ui->selected_source_id = gpus[i].source_id;
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
    if (include_name) snprintf(prefix, sizeof(prefix), "NET(%s)", name);
    else snprintf(prefix, sizeof(prefix), "NET");
    snprintf(line, line_size, "%s | %s(%s) | TOTAL:%s | d:%s | u:%s",
             prefix, capacity, load, total, rx, tx);
    char *down = strstr(line, "d:");
    char *up = strstr(line, "u:");
    *down_offset = down ? (int)(down - line) : -1;
    *up_offset = up ? (int)(up - line) : -1;
}

static void format_network_line_compact(const NetworkInterfaceInfo *interface,
                                        bool include_name, char *line, size_t line_size,
                                        int *down_offset, int *up_offset) {
    char capacity[32], load[32], total[32], rx[32], tx[32];
    bool valid = interface && interface->valid;
    bool capacity_valid = interface && interface->capacity_valid;
    bool utilization_valid = interface && interface->utilization_valid;
    format_network_rate(capacity, sizeof(capacity),
                        interface ? interface->capacity_bps : 0.0,
                        capacity_valid);
    if (utilization_valid)
        snprintf(load, sizeof(load), "%.0f%%", interface->utilization_percent);
    else
        snprintf(load, sizeof(load), "N/A");
    format_network_rate(total, sizeof(total), interface ? interface->total_bps : 0.0, valid);
    format_network_rate(rx, sizeof(rx), interface ? interface->rx_bps : 0.0, valid);
    format_network_rate(tx, sizeof(tx), interface ? interface->tx_bps : 0.0, valid);
    const char *name = interface && interface->name[0] ? interface->name : "N/A";
    char prefix[96];
    if (include_name) snprintf(prefix, sizeof(prefix), "NET(%s)", name);
    else snprintf(prefix, sizeof(prefix), "NET");
    snprintf(line, line_size, "%s %s(%s) Tot:%s d:%s u:%s",
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
    if (width < 48)
        format_network_line_compact(interface, include_name, line, sizeof(line),
                                    &down_offset, &up_offset);
    else
        format_network_line(interface, include_name, line, sizeof(line),
                            &down_offset, &up_offset);
    bool valid = interface && interface->valid;
    unsigned char info_attr = valid ? ATTR_TEXT_GREEN : ATTR_DIM;
    screen_put_attr(screen, row, left, line, width, info_attr);
    char *separator = strstr(line, " | TOTAL:");
    if (!separator) separator = strstr(line, " T:");
    int cyan_length = separator ? (int)(separator - line) : (int)strlen(line);
    screen_put_attr(screen, row, left, line, cyan_length, ATTR_CYAN_BOLD);
    draw_network_arrows(screen, row, left, width, down_offset, up_offset, info_attr);
}

static void draw_network_graph(Screen *screen, int top, int left, int width,
                               const History *history, int graph_rows) {
    draw_plot_graph_scaled(screen, top, left, width, history, graph_rows,
                           history_peak_400(history), GRAPH_AXIS_NETWORK_RATE, "bps");
}

static void draw_system(Screen *screen, int top, const SystemInfo *system,
                        const History *ram, const History *swap,
                        const History *disk_history, const History *network_history,
                        const DiskInfo *disk, const NetworkInfo *network, bool collapsed) {
    char ram_title[32], swap_title[32], ram_info[64], swap_info[64];
    char disk_title[32], disk_info[128];
    snprintf(ram_title, sizeof(ram_title), "RAM(%.0f%%)", system->ram_percent);
    snprintf(ram_info, sizeof(ram_info), "%.1fG/%.1fG",
             (double)system->ram_used / (1024.0 * 1024.0 * 1024.0),
             (double)system->ram_total / (1024.0 * 1024.0 * 1024.0));
    snprintf(swap_title, sizeof(swap_title), "SWAP(%.0f%%)", system->swap_percent);
    snprintf(swap_info, sizeof(swap_info), "%.1fG/%.1fG",
             (double)system->swap_used / (1024.0 * 1024.0 * 1024.0),
             (double)system->swap_total / (1024.0 * 1024.0 * 1024.0));
    char read_rate[32] = "N/A", write_rate[32] = "N/A";
    if (disk && disk->valid) {
        format_disk_rate_inline(read_rate, sizeof(read_rate), disk->read_bps);
        format_disk_rate_inline(write_rate, sizeof(write_rate), disk->write_bps);
    }
    if (disk && disk->capacity_valid && disk->total_bytes) {
        double used_percent = 100.0 * (double)disk->used_bytes / (double)disk->total_bytes;
        snprintf(disk_title, sizeof(disk_title), "DISK(%.0f%%)", used_percent);
        snprintf(disk_info, sizeof(disk_info), "%.1fGB/%.1fGB R:%s W:%s",
                 (double)disk->used_bytes / (1024.0 * 1024.0 * 1024.0),
                 (double)disk->total_bytes / (1024.0 * 1024.0 * 1024.0),
                 read_rate, write_rate);
    } else {
        snprintf(disk_title, sizeof(disk_title), "DISK(N/A)");
        snprintf(disk_info, sizeof(disk_info), "N/A/N/A R:%s W:%s", read_rate, write_rate);
    }

    int gap = 2;
    /* Use one common Y-axis width so RAM, SWAP, DISK, and NETWORK panels
       have equal graph widths (within one cell for odd terminal widths). */
    int panel_space = screen->cols - 3 * gap;
    int panel_width = panel_space / 4;
    int remainder = panel_space % 4;
    int disk_width = panel_width + (remainder > 0 ? 1 : 0);
    int left_width = panel_width + (remainder > 1 ? 1 : 0);
    int middle_width = panel_width + (remainder > 2 ? 1 : 0);
    int middle_left = left_width + gap;
    int disk_left = middle_left + middle_width + gap;
    int network_left = disk_left + disk_width + gap;
    int right_width = screen->cols - network_left;
    int line_count = network_line_count(network);
    bool include_name = line_count > 1;

    if (collapsed) {
        char ram_summary[128], swap_summary[128], disk_summary[192];
        snprintf(ram_summary, sizeof(ram_summary), "%s %s", ram_title, ram_info);
        snprintf(swap_summary, sizeof(swap_summary), "%s %s", swap_title, swap_info);
        snprintf(disk_summary, sizeof(disk_summary), "%s %s", disk_title, disk_info);
        screen_put_attr(screen, top, 0, ram_summary, left_width, text_load_attr(system->ram_percent));
        screen_put_attr(screen, top, middle_left, swap_summary, middle_width, text_load_attr(system->swap_percent));
        screen_put_attr(screen, top, disk_left, disk_summary, disk_width,
                        disk && disk->capacity_valid ? ATTR_TEXT_GREEN : ATTR_DIM);
        for (int i = 0; i < line_count; i++) {
            const NetworkInterfaceInfo *interface = network->interface_count > 0
                                                         ? &network->interfaces[i]
                                                         : NULL;
            draw_network_line(screen, top + i, network_left, right_width, interface, include_name);
        }
        return;
    }
    draw_plot(screen, top, 0, left_width, ram, ram_title, ram_info, "%");
    draw_plot(screen, top, middle_left, middle_width, swap, swap_title, swap_info, "%");
    draw_disk_plot(screen, top, disk_left, disk_width, disk_history,
                   disk_title, disk_info);
    for (int i = 0; i < line_count; i++) {
        const NetworkInterfaceInfo *interface = network->interface_count > 0
                                                     ? &network->interfaces[i]
                                                     : NULL;
        draw_network_line(screen, top + i, network_left, right_width, interface, include_name);
    }
    int graph_rows = network_graph_rows(line_count);
    if (graph_rows > 0)
        draw_network_graph(screen, top + line_count, network_left, right_width,
                           network_history, graph_rows);
}

static void add_click_target(UiState *ui, int row, int left, int right, unsigned char key) {
    if (!ui || left >= right || ui->click_target_count >= MAX_CLICK_TARGETS) return;
    ui->click_targets[ui->click_target_count++] = (ClickTarget){row, left, right, key};
}

static bool usable_web_ipv4(struct in_addr address) {
    uint32_t host = ntohl(address.s_addr);
    return host != INADDR_ANY && (host >> 24) != 127 && (host & 0xffff0000U) != 0xa9fe0000U;
}

static void format_active_web_url(char *out, size_t size, int port, bool local_only) {
    char host[INET_ADDRSTRLEN] = "127.0.0.1";
    if (!local_only) {
        bool found = false;
        /* A UDP connect selects the outbound interface without sending a packet. */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct sockaddr_in destination = {0};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &destination.sin_addr);
            if (connect(fd, (struct sockaddr *)&destination, sizeof(destination)) == 0) {
                struct sockaddr_in local = {0};
                socklen_t local_size = sizeof(local);
                if (getsockname(fd, (struct sockaddr *)&local, &local_size) == 0 &&
                    usable_web_ipv4(local.sin_addr) &&
                    inet_ntop(AF_INET, &local.sin_addr, host, sizeof(host)))
                    found = true;
            }
            close(fd);
        }
        if (!found) {
            struct ifaddrs *interfaces = NULL;
            if (getifaddrs(&interfaces) == 0) {
                for (struct ifaddrs *interface = interfaces; interface; interface = interface->ifa_next) {
                    if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET ||
                        !(interface->ifa_flags & IFF_UP) || (interface->ifa_flags & IFF_LOOPBACK))
                        continue;
                    struct in_addr address = ((struct sockaddr_in *)interface->ifa_addr)->sin_addr;
                    if (usable_web_ipv4(address) && inet_ntop(AF_INET, &address, host, sizeof(host))) {
                        found = true;
                        break;
                    }
                }
                freeifaddrs(interfaces);
            }
        }
    }
    snprintf(out, size, "http://%s:%d", host, port);
}

static void draw_header(Screen *screen, const char *driver, const char *cuda_library,
                        const char *cuda_runtime, int cuda_driver, double interval,
                        bool paused, UiState *ui) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    char time_text[64];
    strftime(time_text, sizeof(time_text), "%a %b %d %H:%M:%S %Y", &local_time);
    if (active_web_port && active_web_url[0]) {
        screen_put_attr(screen, 0, 0, time_text, (int)strlen(time_text), ATTR_CYAN);
        char web_address[128];
        snprintf(web_address, sizeof(web_address), "Web monitor: %s", active_web_url);
        int address_length = (int)strlen(web_address);
        if (address_length < screen->cols)
            screen_put_attr(screen, 0, screen->cols - address_length, web_address,
                            address_length, ATTR_NORMAL);
    } else {
        screen_putf_attr(screen, 0, 0, screen->cols, ATTR_CYAN,
                         "%s  (Press h for help or q to quit)", time_text);
    }
    char cuda_api[32];
    format_cuda_api(cuda_api, sizeof(cuda_api), cuda_driver);
    if (strcmp(cuda_runtime, "N/A") != 0)
        screen_putf_attr(screen, 1, 0, screen->cols, ATTR_CYAN_BOLD,
                         "ctop %s  |  Driver %s  |  CUDA lib %s (runtime %s)  |  Driver API %s",
                         CTOP_VERSION, driver, cuda_library, cuda_runtime, cuda_api);
    else
        screen_putf_attr(screen, 1, 0, screen->cols, ATTR_CYAN_BOLD,
                         "ctop %s  |  Driver %s  |  CUDA lib %s  |  Driver API %s",
                         CTOP_VERSION, driver, cuda_library, cuda_api);

    ui->click_target_count = 0;
    char controls[256];
    snprintf(controls, sizeof(controls),
             "%s %.1fs | q quit h help a monitor [ ] select(>) u/d move x remove c fold selected g GPU m SYS space pause +/- rate r reset j next k kill",
             paused ? "PAUSED" : "LIVE", interval);
    screen_put_attr(screen, 2, 0, controls, screen->cols, ATTR_DIM);
    const struct { const char *label; unsigned char key; int offset; } hits[] = {
        {"q quit", 'q', 0}, {"h help", 'h', 0}, {"a monitor", 'a', 0},
        {"[ ] select", '[', 0}, {"[ ] select", ']', 2},
        {"u/d move", 'u', 0}, {"u/d move", 'd', 2},
        {"x remove", 'x', 0}, {"c fold", 'c', 0},
        {"g GPU", 'g', 0}, {"m SYS", 'm', 0}, {"space pause", ' ', 0},
        {"+/- rate", '+', 0}, {"+/- rate", '-', 2},
        {"r reset", 'r', 0}, {"j next", 'j', 0}, {"k kill", 'k', 0},
    };
    if (!ui->monitor_prompt) {
        for (size_t i = 0; i < ARRAY_LEN(hits); i++) {
            const char *hit = strstr(controls, hits[i].label);
            if (hit) {
                int col = (int)(hit - controls) + hits[i].offset;
                add_click_target(ui, 2, col, col + 1, hits[i].key);
            }
        }
    }
    if (ui->notice[0]) {
        int start = (int)strlen(controls) + 2;
        if (start < screen->cols) {
            char notice[144];
            snprintf(notice, sizeof(notice), "  %s", ui->notice);
            screen_put_attr(screen, 2, start, notice, screen->cols - start,
                            ui->notice_error ? ATTR_RED : ATTR_TEXT_GREEN);
        }
    }
}

typedef struct {
    int row;
    int address_start;
    int address_width;
    int token_label_start;
    int token_start;
    int token_width;
    int button_start;
} MonitorFormLayout;

static bool monitor_form_layout(const Screen *screen, MonitorFormLayout *layout) {
    if (screen->rows < 5 || screen->cols < 76) return false;
    layout->row = screen->rows - 2;
    layout->address_start = 9;
    layout->button_start = screen->cols - 8;
    int flexible = layout->button_start - layout->address_start - 9;
    layout->address_width = flexible * 58 / 100;
    if (layout->address_width < 20) layout->address_width = 20;
    layout->token_label_start = layout->address_start + layout->address_width + 1;
    layout->token_start = layout->token_label_start + 7;
    layout->token_width = layout->button_start - layout->token_start - 1;
    return layout->token_width >= 8;
}

static void draw_form_field(Screen *screen, int row, int start, int width,
                            const char *value, const char *placeholder,
                            bool focused, bool mask_value) {
    screen_put_attr(screen, row, start, "[", 1, ATTR_DIM);
    int inner_width = width - 2;
    if (value[0]) {
        char masked[REMOTE_ADDRESS_SIZE];
        snprintf(masked, sizeof(masked), "%s", value);
        if (mask_value) memset(masked, '*', strlen(masked));
        else {
            char *query_token = strcasestr(masked, "?token=");
            if (!query_token) query_token = strcasestr(masked, "&token=");
            if (query_token) {
                char *token = query_token + 7;
                char *end = strpbrk(token, "&#");
                memset(token, '*', end ? (size_t)(end - token) : strlen(token));
            }
        }
        screen_put_attr(screen, row, start + 1, masked, inner_width,
                        focused ? ATTR_YELLOW : ATTR_TEXT_GREEN);
    } else {
        screen_put_attr(screen, row, start + 1, placeholder, inner_width, ATTR_DIM);
    }
    screen_put_attr(screen, row, start + width - 1, "]", 1, ATTR_DIM);
}

static void draw_monitor_form(Screen *screen, UiState *ui) {
    MonitorFormLayout layout;
    if (!monitor_form_layout(screen, &layout)) return;
    screen_put_attr(screen, layout.row, 0, "REMOTE:", 9, ATTR_CYAN_BOLD);
    draw_form_field(screen, layout.row, layout.address_start, layout.address_width,
                    ui->monitor_input, "host:port",
                    ui->monitor_prompt && !ui->monitor_token_focus && !ui->monitor_add_focus,
                    false);
    screen_put_attr(screen, layout.row, layout.token_label_start, "TOKEN:", 6, ATTR_CYAN_BOLD);
    draw_form_field(screen, layout.row, layout.token_start, layout.token_width,
                    ui->monitor_token_input, "token (optional)",
                    ui->monitor_prompt && ui->monitor_token_focus,
                    true);
    screen_put_attr(screen, layout.row, layout.button_start, "[ ADD ]", 7,
                    ui->monitor_add_focus ? ATTR_TEXT_GREEN : ATTR_CYAN);
    add_click_target(ui, layout.row, layout.address_start,
                     layout.address_start + layout.address_width, UI_KEY_FOCUS_MONITOR);
    add_click_target(ui, layout.row, layout.token_label_start,
                     layout.token_start + layout.token_width, UI_KEY_FOCUS_TOKEN);
    add_click_target(ui, layout.row, layout.button_start,
                     layout.button_start + 7, UI_KEY_SUBMIT_MONITOR);
    if (ui->notice_error && ui->notice[0] && screen->rows > 1)
        screen_put_attr(screen, screen->rows - 1, 0, ui->notice, screen->cols, ATTR_RED);
}

static void dashboard_section_title(char *out, size_t size, const DashboardSection *section) {
    if (section->type == SECTION_GPU_LIST) {
        snprintf(out, size, "GPU LIST");
        return;
    }
    const char *prefix = "";
    if (remote_monitor_count > 0) {
        if (section->source_id == 0) prefix = "A";
        else {
            RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
            if (monitor) prefix = monitor->prefix;
        }
    }
    const char *name = section->type == SECTION_GPU ? "GPU" :
                       section->type == SECTION_SYSTEM ? "SYSTEM" : "GPU PROCESSES";
    if (prefix[0]) snprintf(out, size, "%s(%s)", name, prefix);
    else snprintf(out, size, "%s", name);
}

static void render_screen(Screen *screen, const GpuInfo *gpus, int gpu_count,
                          const SystemInfo *system, const History *vram,
                          const History *gpu_util, const History *ram,
                          const History *swap, const History *disk_history,
                          const History *network_history, const DiskInfo *disk,
                          const NetworkInfo *network, UiState *ui,
                          const char *driver, const char *cuda_library,
                          const char *cuda_runtime, int cuda_driver,
                          double interval, bool paused, bool help_visible) {
    screen_clear(screen);
    Marker markers[MAX_DASHBOARD_SECTIONS] = {0};
    size_t marker_count = 0;
    draw_header(screen, driver, cuda_library, cuda_runtime, cuda_driver, interval, paused, ui);
    if (help_visible) {
        const char *lines[] = {
            "KEYS",
            "q or Esc     quit",
            "h            show/hide this help",
            "click ▲ / ▼ before a section title to move it up/down",
            "click right marker     collapse/expand that section",
            "a            focus the bottom REMOTE address field",
            "Enter address in REMOTE; optional credentials go in TOKEN",
            "Click ADD to connect; Tab/Enter moves focus to TOKEN and ADD",
            "[ / ]        select a dashboard section (marked with >)",
            "u / d        move selected section up/down",
            "x            remove the selected remote monitor",
            "g / m        collapse/expand local GPU / SYSTEM",
            "Space        pause/resume sampling",
            "+ / -        increase/decrease refresh interval",
            "j            select next GPU process",
            "k / K        send SIGTERM to a selected local process",
            "r            clear graph history; c folds/unfolds only the selected section",
            "",
            "Remote monitors use unencrypted HTTP; do not send tokens over untrusted networks.",
        };
        for (size_t i = 0; i < ARRAY_LEN(lines); i++) screen_put(screen, 4 + (int)i, 0, lines[i], screen->cols);
        exit_prompt_row = 4 + (int)ARRAY_LEN(lines);
        if (exit_prompt_row > screen->rows) exit_prompt_row = screen->rows;
        screen_flush(screen, markers, 0);
        return;
    }

    int y = 3;
    for (size_t i = 0; i < ui->section_count; i++) {
        const DashboardSection *section = &ui->sections[i];
        int section_start = y;
        char title[128], marked_title[132];
        dashboard_section_title(title, sizeof(title), section);
        if (section->type != SECTION_GPU_LIST && section->source_id != 0) {
            RemoteMonitor *monitor = remote_monitor_by_id(section->source_id);
            if (monitor && !monitor->connected) {
                size_t used = strlen(title);
                snprintf(title + used, sizeof(title) - used, " [OFFLINE]");
            }
        }
        bool can_move_up = i > 0;
        bool can_move_down = i + 1 < ui->section_count;
        if ((int)i == ui->selected_section) {
            snprintf(marked_title, sizeof(marked_title), "> %s", title);
            draw_rule(screen, y, marked_title, section->type != SECTION_GPU_LIST,
                      !dashboard_section_collapsed(section, ui), can_move_up, can_move_down,
                      markers, &marker_count);
        } else {
            draw_rule(screen, y, title, section->type != SECTION_GPU_LIST,
                      !dashboard_section_collapsed(section, ui), can_move_up, can_move_down,
                      markers, &marker_count);
        }
        if (can_move_up)
            add_click_target(ui, y, 0, 1, (unsigned char)(UI_KEY_MOVE_SECTION_UP_BASE + i));
        if (can_move_down)
            add_click_target(ui, y, 2, 3, (unsigned char)(UI_KEY_MOVE_SECTION_DOWN_BASE + i));
        y++;

        if (section->type == SECTION_GPU_LIST) {
            draw_gpu_status(screen, y, gpus, gpu_count);
            y += 1 + gpu_count;
            continue;
        }
        const MonitorView *view = monitor_view_by_id(tui_views, tui_view_count, section->source_id);
        if (!view) { y++; continue; }
        bool collapsed = dashboard_section_collapsed(section, ui);
        if (section->type == SECTION_GPU) {
            double memory_average = 0.0, gpu_average = 0.0;
            int memory_count = 0, gpu_count_valid = 0;
            for (int j = 0; j < view->gpu_count; j++) {
                const GpuInfo *gpu = &gpus[view->gpu_start + j];
                if (gpu->memory_valid && gpu->memory_total) {
                    memory_average += 100.0 * (double)gpu->memory_used / (double)gpu->memory_total;
                    memory_count++;
                }
                if (gpu->utilization_valid) { gpu_average += gpu->utilization; gpu_count_valid++; }
            }
            if (memory_count) memory_average /= memory_count;
            if (gpu_count_valid) gpu_average /= gpu_count_valid;
            if (collapsed) {
                int half = (screen->cols - 2) / 2;
                screen_putf(screen, y, 0, half, "GPU MEM AVG(%.1f%%)", memory_average);
                screen_putf(screen, y, half + 2, screen->cols - half - 2, "GPU AVG(%.1f%%)", gpu_average);
                y++;
            } else {
                int gap = 2;
                int left_width = (screen->cols - gap) / 2;
                int right_left = left_width + gap;
                int right_width = screen->cols - right_left;
                char memory_title[64], gpu_title[64];
                snprintf(memory_title, sizeof(memory_title), "GPU MEM AVG(%.1f%%) [30s|10s|3s|1s]", memory_average);
                snprintf(gpu_title, sizeof(gpu_title), "GPU AVG(%.1f%%) [30s|10s|3s|1s]", gpu_average);
                draw_plot(screen, y, 0, left_width, view->vram_history, memory_title, NULL, NULL);
                draw_plot(screen, y, right_left, right_width, view->gpu_history, gpu_title, NULL, NULL);
                y += 2 + GRAPH_ROWS;
            }
        } else if (section->type == SECTION_SYSTEM) {
            draw_system(screen, y, view->system, view->ram_history, view->swap_history,
                        view->disk_history, view->network_history, view->disk,
                        view->network, collapsed);
            y += system_content_rows(view->network->interface_count, collapsed);
        } else if (section->type == SECTION_GPU_PROCESSES) {
            if (collapsed) {
                screen_putf(screen, y, 0, screen->cols, "%d GPU processes",
                            source_process_count(gpus, view));
                y++;
            } else if (view->gpu_count > 0) {
                draw_processes(screen, y, &gpus[view->gpu_start], view->gpu_count, ui);
                y += 1 + source_process_count(gpus, view);
            } else {
                screen_put_attr(screen, y, 0, "No GPU processes reported by this monitor.", screen->cols, ATTR_DIM);
                y++;
            }
        }
        if (y <= section_start) y = section_start + 1;
    }
    draw_monitor_form(screen, ui);
    exit_prompt_row = screen->rows;
    screen_flush(screen, markers, marker_count);
    MonitorFormLayout form_layout;
    if (ui->monitor_prompt && !ui->monitor_add_focus &&
        monitor_form_layout(screen, &form_layout)) {
        bool token_field = ui->monitor_token_focus;
        int field_start = token_field ? form_layout.token_start : form_layout.address_start;
        int field_width = token_field ? form_layout.token_width : form_layout.address_width;
        size_t field_length = token_field ? ui->monitor_token_input_length : ui->monitor_input_length;
        int inner_width = field_width - 2;
        int cursor_col = field_start + 1 + (int)field_length;
        if (field_length >= (size_t)inner_width)
            cursor_col = field_start + field_width - 2;
        printf("\033[%d;%dH\033[?25h", form_layout.row + 1, cursor_col + 1);
        fflush(stdout);
    }
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

static void toggle_mouse_at(UiState *ui, int x, int y, int cols,
                            const GpuInfo *gpus, int gpu_count) {
    if (x < cols - 6) return;
    int row = 3;
    for (size_t i = 0; i < ui->section_count; i++) {
        DashboardSection *section = &ui->sections[i];
        if (row == y && section->type != SECTION_GPU_LIST) {
            toggle_dashboard_section(section, ui);
            return;
        }
        row += dashboard_section_height(section, ui, gpus, gpu_count, tui_views, tui_view_count);
    }
}

static void handle_mouse(UiState *ui, const GpuInfo *gpus, int x, int y, int cols,
                         int gpu_count, bool pressed, bool *button_down) {
    if (pressed) ui->monitor_prompt = false;
    bool process_row = process_at_screen_row(y, gpu_count, ui, gpus,
                                             tui_views, tui_view_count, NULL, NULL);
    if (process_row) {
        if (pressed) {
            if (!*button_down) select_process_at(ui, y, gpu_count, gpus, tui_views, tui_view_count);
            *button_down = true;
        } else {
            if (!*button_down) select_process_at(ui, y, gpu_count, gpus, tui_views, tui_view_count);
            *button_down = false;
        }
        return;
    }

    /* Only the left button toggles.  A press toggles immediately; a
       release-only terminal event also toggles, but press+release does not
       toggle twice. */
    if (pressed) {
        if (!*button_down) {
            toggle_mouse_at(ui, x, y, cols, gpus, gpu_count);
            *button_down = true;
        }
    } else {
        if (!*button_down) toggle_mouse_at(ui, x, y, cols, gpus, gpu_count);
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

static bool dashboard_section_equal(DashboardSection a, DashboardSection b) {
    return a.type == b.type && a.source_id == b.source_id;
}

static void rebuild_dashboard_sections(UiState *ui) {
    DashboardSection defaults[MAX_DASHBOARD_SECTIONS];
    size_t default_count = 0;
    defaults[default_count++] = (DashboardSection){-1, SECTION_GPU_LIST};
    for (int i = 0; i <= remote_monitor_count; i++)
        defaults[default_count++] = (DashboardSection){i == 0 ? 0 : remote_monitors[i - 1].id, SECTION_GPU};
    for (int i = 0; i <= remote_monitor_count; i++)
        defaults[default_count++] = (DashboardSection){i == 0 ? 0 : remote_monitors[i - 1].id, SECTION_SYSTEM};
    for (int i = 0; i <= remote_monitor_count; i++)
        defaults[default_count++] = (DashboardSection){i == 0 ? 0 : remote_monitors[i - 1].id, SECTION_GPU_PROCESSES};

    DashboardSection previous_selected = {0};
    bool had_selection = ui->selected_section >= 0 && (size_t)ui->selected_section < ui->section_count;
    if (had_selection) previous_selected = ui->sections[ui->selected_section];
    DashboardSection reconciled[MAX_DASHBOARD_SECTIONS];
    size_t reconciled_count = 0;
    for (size_t i = 0; i < ui->section_count; i++) {
        for (size_t j = 0; j < default_count; j++) {
            if (dashboard_section_equal(ui->sections[i], defaults[j])) {
                reconciled[reconciled_count++] = ui->sections[i];
                break;
            }
        }
    }
    for (size_t d = 0; d < default_count; d++) {
        bool found = false;
        for (size_t i = 0; i < reconciled_count; i++)
            if (dashboard_section_equal(reconciled[i], defaults[d])) { found = true; break; }
        if (found) continue;
        size_t insert_at = reconciled_count;
        for (size_t i = 0; i < reconciled_count; i++) {
            size_t rank = 0;
            while (rank < default_count && !dashboard_section_equal(reconciled[i], defaults[rank])) rank++;
            if (rank > d) { insert_at = i; break; }
        }
        memmove(&reconciled[insert_at + 1], &reconciled[insert_at],
                (reconciled_count - insert_at) * sizeof(reconciled[0]));
        reconciled[insert_at] = defaults[d];
        reconciled_count++;
    }
    memcpy(ui->sections, reconciled, reconciled_count * sizeof(reconciled[0]));
    ui->section_count = reconciled_count;
    ui->selected_section = 0;
    if (had_selection) {
        for (size_t i = 0; i < reconciled_count; i++) {
            if (dashboard_section_equal(reconciled[i], previous_selected)) {
                ui->selected_section = (int)i;
                break;
            }
        }
    }
}

static void submit_monitor_input(UiState *ui) {
    char address[sizeof(ui->monitor_input)];
    snprintf(address, sizeof(address), "%s", ui->monitor_input);
    char *trimmed = address;
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    if (trimmed != address) memmove(address, trimmed, strlen(trimmed) + 1);
    size_t address_length = strlen(address);
    while (address_length && (address[address_length - 1] == ' ' || address[address_length - 1] == '\t'))
        address[--address_length] = '\0';

    const char *token = ui->monitor_token_input[0] ? ui->monitor_token_input : NULL;
    int monitor_count_before = remote_monitor_count;
    char error[128] = "";
    int id = add_remote_monitor(address, token, error, sizeof(error));
    if (id >= 0) {
        rebuild_dashboard_sections(ui);
        RemoteMonitor *monitor = remote_monitor_by_id(id);
        if (remote_monitor_count == monitor_count_before)
            snprintf(ui->notice, sizeof(ui->notice), "Monitor %s already configured", monitor ? monitor->prefix : "");
        else
            snprintf(ui->notice, sizeof(ui->notice), "Added monitor %s", monitor ? monitor->prefix : "");
        ui->notice_error = false;
        ui->monitor_prompt = false;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
        ui->monitor_input_length = 0;
        ui->monitor_input[0] = '\0';
        ui->monitor_token_input_length = 0;
        ui->monitor_token_input[0] = '\0';
    } else {
        if (strncmp(error, "HTTP 401", 8) == 0 || strncmp(error, "HTTP 403", 8) == 0)
            snprintf(ui->notice, sizeof(ui->notice), "ADD failed: authentication denied; check TOKEN");
        else if (strncmp(error, "HTTP 404", 8) == 0)
            snprintf(ui->notice, sizeof(ui->notice), "ADD failed: HTTP 404; check address/API path");
        else if (error[0])
            snprintf(ui->notice, sizeof(ui->notice), "ADD failed: %s", error);
        else
            snprintf(ui->notice, sizeof(ui->notice), "Could not add monitor");
        ui->notice_error = true;
        ui->monitor_prompt = false;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
    }
}

static void handle_monitor_prompt_key(unsigned char key, UiState *ui) {
    if (key == 0x1b) {
        ui->monitor_prompt = false;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
        snprintf(ui->notice, sizeof(ui->notice), "Monitor editing cancelled");
        ui->notice_error = false;
        return;
    }
    if (key == '\t' || key == '\r' || key == '\n') {
        if (ui->monitor_add_focus && (key == '\r' || key == '\n')) {
            submit_monitor_input(ui);
            return;
        } else if (ui->monitor_token_focus) {
            ui->monitor_token_focus = false;
            ui->monitor_add_focus = true;
        } else if (ui->monitor_add_focus) {
            ui->monitor_add_focus = false;
            ui->monitor_token_focus = false;
        } else {
            ui->monitor_token_focus = true;
            ui->monitor_add_focus = false;
        }
        ui->notice_error = false;
        ui->notice[0] = '\0';
        return;
    }

    if (ui->monitor_add_focus) return;
    char *field = ui->monitor_token_focus ? ui->monitor_token_input : ui->monitor_input;
    size_t *length = ui->monitor_token_focus ? &ui->monitor_token_input_length : &ui->monitor_input_length;
    size_t capacity = ui->monitor_token_focus ? sizeof(ui->monitor_token_input) : sizeof(ui->monitor_input);
    if (key == 0x08 || key == 0x7f) {
        if (*length) field[--(*length)] = '\0';
        ui->notice_error = false;
        ui->notice[0] = '\0';
    } else if (key >= 0x20 && key < 0x7f && *length + 1 < capacity) {
        field[(*length)++] = (char)key;
        field[*length] = '\0';
        ui->notice_error = false;
        ui->notice[0] = '\0';
    }
}

static void move_dashboard_section(UiState *ui, int direction) {
    int target = ui->selected_section + direction;
    if (target < 0 || (size_t)target >= ui->section_count) return;
    DashboardSection current = ui->sections[ui->selected_section];
    ui->sections[ui->selected_section] = ui->sections[target];
    ui->sections[target] = current;
    ui->selected_section = target;
}

static void handle_key(unsigned char key, UiState *ui, bool *paused, bool *help_visible,
                       double *interval, History *vram, History *gpu,
                       History *ram, History *swap, History *disk, History *network,
                       const GpuInfo *gpus, int gpu_count) {
    if (key == UI_KEY_FOCUS_MONITOR || key == UI_KEY_FOCUS_TOKEN) {
        ui->monitor_prompt = true;
        ui->monitor_token_focus = key == UI_KEY_FOCUS_TOKEN;
        ui->monitor_add_focus = false;
        ui->notice[0] = '\0';
        ui->notice_error = false;
        return;
    }
    if (key == UI_KEY_SUBMIT_MONITOR) {
        submit_monitor_input(ui);
        return;
    }
    if (ui->monitor_prompt) {
        handle_monitor_prompt_key(key, ui);
        return;
    }
    if (ui->kill_prompt) {
        if (key == 'y' || key == 'Y') {
            if (kill(ui->prompt_pid, SIGTERM) == 0) {
                snprintf(ui->notice, sizeof(ui->notice), "SIGTERM sent to PID %ld", (long)ui->prompt_pid);
                ui->notice_error = false;
                ui->selected_gpu = -1;
                ui->selected_source_id = 0;
                ui->selected_pid = 0;
            } else {
                snprintf(ui->notice, sizeof(ui->notice), "kill PID %ld failed: %s",
                         (long)ui->prompt_pid, strerror(errno));
                ui->notice_error = true;
            }
            ui->kill_prompt = false;
        } else if (key == 'n' || key == 'N' || key == 0x1b) {
            snprintf(ui->notice, sizeof(ui->notice), "Kill cancelled");
            ui->notice_error = false;
            ui->kill_prompt = false;
        }
        return;
    }

    ui->notice[0] = '\0';
    ui->notice_error = false;
    if (key == 'h' || key == 'H') *help_visible = !*help_visible;
    else if (key == 'a' || key == 'A') {
        ui->monitor_prompt = true;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
    } else if (key == '[') {
        if (ui->section_count && ui->selected_section > 0) ui->selected_section--;
    } else if (key == ']') {
        if ((size_t)(ui->selected_section + 1) < ui->section_count) ui->selected_section++;
    } else if (key == 'u' || key == 'U') move_dashboard_section(ui, -1);
    else if (key == 'd' || key == 'D') move_dashboard_section(ui, 1);
    else if (key == 'c' || key == 'C') {
        if (ui->section_count) toggle_dashboard_section(&ui->sections[ui->selected_section], ui);
    } else if (key == 'x' || key == 'X') {
        if (ui->section_count && ui->sections[ui->selected_section].source_id != 0 &&
            ui->sections[ui->selected_section].source_id != -1) {
            int source_id = ui->sections[ui->selected_section].source_id;
            if (remove_remote_monitor(source_id)) {
                rebuild_dashboard_sections(ui);
                snprintf(ui->notice, sizeof(ui->notice), "Remote monitor removed");
            }
        } else {
            snprintf(ui->notice, sizeof(ui->notice), "Select a remote monitor section first");
            ui->notice_error = true;
        }
    } else if (key == 'g' || key == 'G') ui->usage_collapsed = !ui->usage_collapsed;
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
        history_clear(disk);
        history_clear(network);
        ui->selected_gpu = -1;
        ui->selected_source_id = 0;
        ui->selected_pid = 0;
        ui->prompt_pid = 0;
        ui->kill_prompt = false;
    } else if (key == 'j') {
        move_process_selection(ui, gpus, gpu_count, 1);
    } else if (key == 'k' || key == 'K') {
        if (ui->selected_pid > 0 && ui->selected_source_id != 0) {
            snprintf(ui->notice, sizeof(ui->notice), "Cannot signal a remote process from this TUI");
            ui->notice_error = true;
        } else if (ui->selected_pid > 0) {
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
                         History *ram, History *swap, History *disk, History *network,
                         const GpuInfo *gpus, int gpu_count, bool *quit) {
    if (key >= UI_KEY_MOVE_SECTION_UP_BASE && key < UI_KEY_MOVE_SECTION_DOWN_BASE) {
        int section = key - UI_KEY_MOVE_SECTION_UP_BASE;
        ui->monitor_prompt = false;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
        ui->notice[0] = '\0';
        ui->notice_error = false;
        if (section >= 0 && (size_t)section < ui->section_count) {
            ui->selected_section = section;
            move_dashboard_section(ui, -1);
        }
    } else if (key >= UI_KEY_MOVE_SECTION_DOWN_BASE) {
        int section = key - UI_KEY_MOVE_SECTION_DOWN_BASE;
        ui->monitor_prompt = false;
        ui->monitor_token_focus = false;
        ui->monitor_add_focus = false;
        ui->notice[0] = '\0';
        ui->notice_error = false;
        if (section >= 0 && (size_t)section < ui->section_count) {
            ui->selected_section = section;
            move_dashboard_section(ui, 1);
        }
    } else if (key == UI_KEY_FOCUS_MONITOR || key == UI_KEY_FOCUS_TOKEN) {
        ui->monitor_prompt = true;
        ui->monitor_token_focus = key == UI_KEY_FOCUS_TOKEN;
        ui->monitor_add_focus = false;
        ui->notice[0] = '\0';
        ui->notice_error = false;
    } else if (key == UI_KEY_SUBMIT_MONITOR) {
        submit_monitor_input(ui);
    } else if (ui->monitor_prompt) handle_monitor_prompt_key(key, ui);
    else if (key == 'q' || key == 'Q' || (key == 0x1b && !ui->kill_prompt)) *quit = true;
    else handle_key(key, ui, paused, help_visible, interval, vram, gpu, ram, swap,
                    disk, network, gpus, gpu_count);
}

static void parse_input(InputBuffer *input, UiState *ui, bool *paused, bool *help_visible,
                        double *interval, History *vram, History *gpu,
                        History *ram, History *swap, History *disk, History *network,
                        const GpuInfo *gpus, int cols, int gpu_count,
                        bool *quit, bool *mouse_button_down) {
    size_t position = 0;
    while (position < input->length) {
        unsigned char *data = (unsigned char *)input->data;
        if (data[position] != 0x1b) {
            dispatch_key(data[position], ui, paused, help_visible, interval,
                         vram, gpu, ram, swap, disk, network, gpus, gpu_count, quit);
            position++;
            continue;
        }
        if (input->length - position == 1 || data[position + 1] != '[') {
            dispatch_key(0x1b, ui, paused, help_visible, interval,
                         vram, gpu, ram, swap, disk, network, gpus, gpu_count, quit);
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
                                         vram, gpu, ram, swap, disk, network, gpus, gpu_count, quit);
                        *mouse_button_down = true;
                    } else {
                        if (!*mouse_button_down)
                            dispatch_key(clicked_key, ui, paused, help_visible, interval,
                                         vram, gpu, ram, swap, disk, network, gpus, gpu_count, quit);
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
                                     vram, gpu, ram, swap, disk, network, gpus, gpu_count, quit);
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
                            DiskInfo *disk, NetworkInfo *network,
                            unsigned long long *total_ticks) {
    memset(gpus, 0, sizeof(*gpus) * MAX_GPUS);
    *gpu_count = 0;
#if defined(CTOP_ENABLE_NVML)
    if (nvml_initialized) {
        unsigned int count = 0;
        nvmlReturn_t result = nvml_api.device_get_count_v2(&count);
        if (result != NVML_SUCCESS) {
#if !defined(CTOP_ENABLE_VULKAN)
            return -1;
#else
            debug_log("NVML device enumeration failed: %s", nvml_api.error_string(result));
#endif
        } else {
            if (count > MAX_GPUS) count = MAX_GPUS;
            for (unsigned int i = 0; i < count && *gpu_count < MAX_GPUS; i++) {
                GpuInfo *gpu = &gpus[*gpu_count];
                if (nvml_api.device_get_handle_by_index_v2(i, &gpu->handle) != NVML_SUCCESS) continue;
                gpu->index = *gpu_count;
                nvml_api.device_get_name(gpu->handle, gpu->name, sizeof(gpu->name));
                snprintf(gpu->data_source, sizeof(gpu->data_source), "nvml");
                unsigned int value = 0;
                if (nvml_api.device_get_temperature(gpu->handle, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS) {
                    gpu->temperature = value;
                    gpu->temperature_valid = true;
                }
                if (nvml_api.device_get_power_usage(gpu->handle, &value) == NVML_SUCCESS) {
                    gpu->power = (double)value / 1000.0;
                    gpu->power_valid = true;
                }
                if (nvml_api.device_get_enforced_power_limit(gpu->handle, &value) == NVML_SUCCESS) {
                    gpu->power_limit = (double)value / 1000.0;
                    gpu->power_limit_valid = true;
                }
                nvmlMemory_t memory;
                if (nvml_api.device_get_memory_info(gpu->handle, &memory) == NVML_SUCCESS) {
                    gpu->memory_used = memory.used;
                    gpu->memory_total = memory.total;
                    gpu->memory_valid = true;
                }
                nvmlUtilization_t utilization;
                if (nvml_api.device_get_utilization_rates(gpu->handle, &utilization) == NVML_SUCCESS) {
                    gpu->utilization = utilization.gpu;
                    gpu->utilization_valid = true;
                }
                collect_processes(gpu);
                collect_process_sm(gpu);
                (*gpu_count)++;
            }
        }
    }
#endif
#if defined(CTOP_ENABLE_VULKAN)
    append_vulkan_gpus(gpus, gpu_count);
    collect_amd_processes(gpus, *gpu_count);
#endif

    if (!read_system_info(system)) return -2;
    if (!read_disk_info(disk)) memset(disk, 0, sizeof(*disk));
    if (!read_network_info(network)) memset(network, 0, sizeof(*network));
    if (!read_cpu_total(total_ticks)) *total_ticks = 0;
    double uptime = read_uptime();
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    cpu_current_count = 0;
    for (int i = 0; i < *gpu_count; i++) {
        size_t j = 0;
        while (j < gpus[i].process_count) {
            fill_process_info(&gpus[i].processes[j], *total_ticks, system->ram_total,
                              uptime, ticks_per_second);
            if (is_ctop_command(gpus[i].processes[j].command)) {
                memmove(&gpus[i].processes[j], &gpus[i].processes[j + 1],
                        (gpus[i].process_count - j - 1) * sizeof(gpus[i].processes[0]));
                gpus[i].process_count--;
            } else {
                j++;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* TUI remote monitors                                                       */

static void json_skip_space(const char **cursor) {
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n') (*cursor)++;
}

static const char *json_string_end(const char *start) {
    if (*start != '"') return NULL;
    const char *cursor = start + 1;
    while (*cursor) {
        if (*cursor == '"') return cursor + 1;
        if (*cursor == '\\' && cursor[1]) cursor++;
        cursor++;
    }
    return NULL;
}

static const char *json_value_end(const char *start, int depth) {
    if (depth > 32) return NULL;
    const char *cursor = start;
    json_skip_space(&cursor);
    if (*cursor == '"') return json_string_end(cursor);
    if (*cursor == '{') {
        cursor++;
        json_skip_space(&cursor);
        while (*cursor && *cursor != '}') {
            const char *key_end = json_string_end(cursor);
            if (!key_end) return NULL;
            cursor = key_end;
            json_skip_space(&cursor);
            if (*cursor++ != ':') return NULL;
            const char *value_end = json_value_end(cursor, depth + 1);
            if (!value_end) return NULL;
            cursor = value_end;
            json_skip_space(&cursor);
            if (*cursor == ',') { cursor++; json_skip_space(&cursor); }
            else if (*cursor != '}') return NULL;
        }
        return *cursor == '}' ? cursor + 1 : NULL;
    }
    if (*cursor == '[') {
        cursor++;
        json_skip_space(&cursor);
        while (*cursor && *cursor != ']') {
            const char *value_end = json_value_end(cursor, depth + 1);
            if (!value_end) return NULL;
            cursor = value_end;
            json_skip_space(&cursor);
            if (*cursor == ',') { cursor++; json_skip_space(&cursor); }
            else if (*cursor != ']') return NULL;
        }
        return *cursor == ']' ? cursor + 1 : NULL;
    }
    while (*cursor && *cursor != ',' && *cursor != '}' && *cursor != ']' &&
           *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') cursor++;
    return cursor > start ? cursor : NULL;
}

static bool json_object_field(const char *object, const char *key,
                              const char **value, const char **value_end) {
    if (!object || *object != '{') return false;
    const char *cursor = object + 1;
    json_skip_space(&cursor);
    while (*cursor && *cursor != '}') {
        const char *key_end = json_string_end(cursor);
        if (!key_end) return false;
        size_t key_length = (size_t)(key_end - cursor - 2);
        bool matches = key_length == strlen(key) && memcmp(cursor + 1, key, key_length) == 0;
        cursor = key_end;
        json_skip_space(&cursor);
        if (*cursor++ != ':') return false;
        json_skip_space(&cursor);
        const char *start = cursor;
        const char *end = json_value_end(start, 0);
        if (!end) return false;
        if (matches) {
            if (value) *value = start;
            if (value_end) *value_end = end;
            return true;
        }
        cursor = end;
        json_skip_space(&cursor);
        if (*cursor == ',') { cursor++; json_skip_space(&cursor); }
        else if (*cursor != '}') return false;
    }
    return false;
}

static bool json_array_next(const char **cursor, const char *array_end,
                            const char **value, const char **value_end) {
    json_skip_space(cursor);
    if (**cursor == ']') return false;
    if (!**cursor || (array_end && *cursor >= array_end)) return false;
    const char *start = *cursor;
    const char *end = json_value_end(start, 0);
    if (!end || (array_end && end > array_end)) return false;
    if (value) *value = start;
    if (value_end) *value_end = end;
    *cursor = end;
    json_skip_space(cursor);
    if (**cursor == ',') (*cursor)++;
    return true;
}

static bool json_get_double(const char *object, const char *key, double *number) {
    const char *value, *end;
    if (!json_object_field(object, key, &value, &end) || *value == 'n') return false;
    char *parsed_end = NULL;
    errno = 0;
    double parsed = strtod(value, &parsed_end);
    if (errno || parsed_end == value || parsed_end > end || !isfinite(parsed)) return false;
    *number = parsed;
    return true;
}

static bool json_get_uint(const char *object, const char *key, unsigned long long *number) {
    const char *value, *end;
    if (!json_object_field(object, key, &value, &end) || *value == 'n' || *value == '-') return false;
    char *parsed_end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &parsed_end, 10);
    if (errno || parsed_end == value || parsed_end > end) return false;
    *number = parsed;
    return true;
}

static bool json_get_int(const char *object, const char *key, int *number) {
    double parsed;
    if (!json_get_double(object, key, &parsed) || parsed < INT_MIN || parsed > INT_MAX || floor(parsed) != parsed)
        return false;
    *number = (int)parsed;
    return true;
}

static bool json_get_bool(const char *object, const char *key, bool *value) {
    const char *field, *end;
    if (!json_object_field(object, key, &field, &end)) return false;
    if (end - field == 4 && memcmp(field, "true", 4) == 0) { *value = true; return true; }
    if (end - field == 5 && memcmp(field, "false", 5) == 0) { *value = false; return true; }
    return false;
}

static bool json_get_string(const char *object, const char *key, char *out, size_t size) {
    const char *value, *end;
    if (!size || !json_object_field(object, key, &value, &end) || *value != '"') return false;
    size_t written = 0;
    for (const char *cursor = value + 1; cursor < end - 1 && written + 1 < size; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '\\' && cursor + 1 < end - 1) {
            cursor++;
            switch (*cursor) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u':
                    if (cursor + 4 < end - 1) cursor += 4;
                    ch = '?';
                    break;
                default: ch = (unsigned char)*cursor; break;
            }
        }
        out[written++] = ch >= 0x20 ? (char)ch : ' ';
    }
    out[written] = '\0';
    return true;
}

static bool remote_address_configure(RemoteMonitor *monitor, const char *address,
                                     const char *token, char *error, size_t error_size) {
    if (!address || !*address) {
        snprintf(error, error_size, "Enter an HTTP host:port address.");
        return false;
    }
    const char *authority = address;
    if (strncmp(authority, "https://", 8) == 0) {
        snprintf(error, error_size, "TUI remote monitors currently support HTTP only.");
        return false;
    }
    if (strncmp(authority, "http://", 7) == 0) authority += 7;
    else if (strncmp(authority, "//", 2) == 0) authority += 2;
    char authority_text[320];
    size_t authority_length = strcspn(authority, "/?#");
    if (!authority_length || authority_length >= sizeof(authority_text)) {
        snprintf(error, error_size, "Invalid monitor address.");
        return false;
    }
    memcpy(authority_text, authority, authority_length);
    authority_text[authority_length] = '\0';
    if (strchr(authority_text, '@')) {
        snprintf(error, error_size, "Credentials must be entered after the address, separated by a space.");
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)authority_text; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7f) {
            snprintf(error, error_size, "Monitor host contains an invalid character.");
            return false;
        }
    }

    char *port_text = NULL;
    if (authority_text[0] == '[') {
        char *closing = strchr(authority_text, ']');
        if (!closing) { snprintf(error, error_size, "Invalid IPv6 monitor address."); return false; }
        *closing = '\0';
        memmove(authority_text, authority_text + 1, strlen(authority_text));
        if (closing[1] == ':') port_text = closing + 2;
        else if (!closing[1]) port_text = NULL;
        else { snprintf(error, error_size, "Invalid monitor port."); return false; }
    } else {
        char *colon = strrchr(authority_text, ':');
        if (colon) { *colon = '\0'; port_text = colon + 1; }
    }
    if (!authority_text[0] || strlen(authority_text) >= sizeof(monitor->host)) {
        snprintf(error, error_size, "Invalid monitor host.");
        return false;
    }
    int port = 80;
    if (port_text && *port_text) {
        char *end = NULL;
        long parsed = strtol(port_text, &end, 10);
        if (!end || *end || parsed < 1 || parsed > 65535) {
            snprintf(error, error_size, "Port must be between 1 and 65535.");
            return false;
        }
        port = (int)parsed;
    } else if (port_text || strncmp(address, "http://", 7) != 0) {
        snprintf(error, error_size, "Address must include a port, for example 192.168.0.177:8079.");
        return false;
    }
    snprintf(monitor->host, sizeof(monitor->host), "%s", authority_text);
    monitor->port = port;

    const char *path = authority + authority_length;
    char path_text[512];
    if (!*path || *path == '#') snprintf(path_text, sizeof(path_text), "/api/v1/metrics");
    else if (*path == '?') snprintf(path_text, sizeof(path_text), "/api/v1/metrics%s", path);
    else snprintf(path_text, sizeof(path_text), "%s", path);
    if (path_text[0] != '/') {
        snprintf(error, error_size, "Invalid metrics path.");
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path_text; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) {
            snprintf(error, error_size, "Metrics path contains an invalid character.");
            return false;
        }
    }
    size_t path_length = strcspn(path_text, "?#");
    while (path_length > 1 && path_text[path_length - 1] == '/')
        path_text[--path_length] = '\0';
    bool metrics_path = (path_length >= 15 && strcmp(path_text + path_length - 15, "/api/v1/metrics") == 0) ||
                        (path_length >= 12 && strcmp(path_text + path_length - 12, "/api/metrics") == 0);
    if (!metrics_path) {
        char expanded[512];
        const char *base_path = path_length > 1 ? path_text : "";
        if (path_text[path_length] == '?')
            snprintf(expanded, sizeof(expanded), "%.*s/api/v1/metrics%s", (int)path_length, base_path, path_text + path_length);
        else
            snprintf(expanded, sizeof(expanded), "%.*s/api/v1/metrics", (int)path_length, base_path);
        snprintf(path_text, sizeof(path_text), "%s", expanded);
    }
    if (strlen(path_text) >= sizeof(monitor->path)) {
        snprintf(error, error_size, "Metrics path is too long.");
        return false;
    }
    snprintf(monitor->path, sizeof(monitor->path), "%s", path_text);
    if (token && *token) {
        size_t token_length = strlen(token);
        if (token_length < 8 || token_length >= sizeof(monitor->token)) {
            snprintf(error, error_size, "Token must be 8-128 characters.");
            return false;
        }
        for (size_t i = 0; i < token_length; i++) {
            unsigned char ch = (unsigned char)token[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '~')) {
                snprintf(error, error_size, "Token contains an unsupported character.");
                return false;
            }
        }
        snprintf(monitor->token, sizeof(monitor->token), "%s", token);
    }
    if (strchr(monitor->host, ':'))
        snprintf(monitor->address, sizeof(monitor->address), "http://[%s]:%d%s", monitor->host, port, monitor->path);
    else
        snprintf(monitor->address, sizeof(monitor->address), "http://%s:%d%s", monitor->host, port, monitor->path);
    return true;
}

static RemoteMonitor *remote_monitor_by_id(int id) {
    for (int i = 0; i < remote_monitor_count; i++)
        if (remote_monitors[i].id == id) return &remote_monitors[i];
    return NULL;
}

static void remote_monitor_public_address(const RemoteMonitor *monitor, char *out, size_t out_size) {
    if (!monitor || !out_size) return;
    size_t path_length = strcspn(monitor->path, "?#");
    if (path_length >= sizeof(monitor->path)) path_length = sizeof(monitor->path) - 1;
    char path[sizeof(monitor->path)];
    memcpy(path, monitor->path, path_length);
    path[path_length] = '\0';
    if (strchr(monitor->host, ':'))
        snprintf(out, out_size, "http://[%s]:%d%s", monitor->host, monitor->port, path);
    else
        snprintf(out, out_size, "http://%s:%d%s", monitor->host, monitor->port, path);
}

static void remote_monitor_free(RemoteMonitor *monitor) {
    free(monitor->gpus);
    history_free(&monitor->ram_history);
    history_free(&monitor->swap_history);
    history_free(&monitor->disk_history);
    history_free(&monitor->network_history);
    history_free(&monitor->vram_history);
    history_free(&monitor->gpu_history);
    memset(monitor, 0, sizeof(*monitor));
}

static bool fetch_initial_remote_metrics(RemoteMonitor *monitor, char *error, size_t error_size) {
    char *body = NULL;
    size_t body_size = 0;
    if (!remote_http_get(monitor, &body, &body_size, error, error_size)) return false;
    (void)body_size;
    if (!history_init(&monitor->ram_history) || !history_init(&monitor->swap_history) ||
        !history_init(&monitor->disk_history) || !history_init(&monitor->network_history) ||
        !history_init(&monitor->vram_history) || !history_init(&monitor->gpu_history)) {
        free(body);
        snprintf(error, error_size, "Could not allocate monitor history.");
        return false;
    }
    bool valid = remote_parse_metrics(monitor, body);
    free(body);
    if (!valid) snprintf(error, error_size, "Invalid metrics response");
    return valid;
}

static int add_remote_monitor(const char *address, const char *token,
                              char *error, size_t error_size) {
    RemoteMonitor candidate = {0};
    if (!remote_address_configure(&candidate, address, token, error, error_size)) return -1;
    for (int i = 0; i < remote_monitor_count; i++) {
        RemoteMonitor *existing = &remote_monitors[i];
        if (strcasecmp(candidate.host, existing->host) != 0 ||
            candidate.port != existing->port || strcmp(candidate.token, existing->token) != 0)
            continue;
        error[0] = '\0';
        return existing->id;
    }
    if (remote_monitor_count >= MAX_REMOTE_MONITORS) {
        snprintf(error, error_size, "At most %d remote monitors are supported.", MAX_REMOTE_MONITORS);
        return -1;
    }
    candidate.id = next_remote_monitor_id;
    snprintf(candidate.prefix, sizeof(candidate.prefix), "%c", 'B' + remote_monitor_count);
    if (!fetch_initial_remote_metrics(&candidate, error, error_size)) {
        remote_monitor_free(&candidate);
        return -1;
    }
    remote_monitors[remote_monitor_count++] = candidate;
    next_remote_monitor_id++;
    error[0] = '\0';
    return candidate.id;
}

static bool remove_remote_monitor(int id) {
    for (int i = 0; i < remote_monitor_count; i++) {
        if (remote_monitors[i].id != id) continue;
        remote_monitor_free(&remote_monitors[i]);
        if (i + 1 < remote_monitor_count)
            memmove(&remote_monitors[i], &remote_monitors[i + 1],
                    (size_t)(remote_monitor_count - i - 1) * sizeof(remote_monitors[0]));
        remote_monitor_count--;
        memset(&remote_monitors[remote_monitor_count], 0, sizeof(remote_monitors[0]));
        for (int j = 0; j < remote_monitor_count; j++)
            snprintf(remote_monitors[j].prefix, sizeof(remote_monitors[j].prefix), "%c", 'B' + j);
        return true;
    }
    return false;
}

static bool remote_send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result > 0) sent += (size_t)result;
        else if (result < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

static int remote_connect(const char *host, int port, char *error, size_t error_size) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int lookup_result = getaddrinfo(host, port_text, &hints, &addresses);
    if (lookup_result != 0) {
        snprintf(error, error_size, "DNS lookup failed: %s", gai_strerror(lookup_result));
        return -1;
    }
    int connected = -1;
    int last_error = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) { last_error = errno; continue; }
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = errno;
            close(fd);
            continue;
        }
        int result = connect(fd, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT};
            result = poll(&descriptor, 1, 700);
            if (result == 0) last_error = ETIMEDOUT;
            else if (result < 0) last_error = errno;
            else {
                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 && !socket_error)
                    result = 0;
                else { last_error = socket_error ? socket_error : errno; result = -1; }
            }
        } else if (result < 0) last_error = errno;
        if (result == 0) {
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            connected = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    if (connected < 0) snprintf(error, error_size, "Connection failed: %s", strerror(last_error));
    return connected;
}

static bool remote_http_get(const RemoteMonitor *monitor, char **body_out, size_t *body_size,
                            char *error, size_t error_size) {
    *body_out = NULL;
    *body_size = 0;
    int fd = remote_connect(monitor->host, monitor->port, error, error_size);
    if (fd < 0) return false;
    char host_header[300];
    if (strchr(monitor->host, ':'))
        snprintf(host_header, sizeof(host_header), "[%s]:%d", monitor->host, monitor->port);
    else
        snprintf(host_header, sizeof(host_header), "%s:%d", monitor->host, monitor->port);
    char request[2048];
    int request_size;
    if (monitor->token[0])
        request_size = snprintf(request, sizeof(request),
            "GET %s HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\nAuthorization: Bearer %s\r\nConnection: close\r\n\r\n",
            monitor->path, host_header, monitor->token);
    else
        request_size = snprintf(request, sizeof(request),
            "GET %s HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
            monitor->path, host_header);
    if (request_size < 0 || (size_t)request_size >= sizeof(request) ||
        !remote_send_all(fd, request, (size_t)request_size)) {
        close(fd); snprintf(error, error_size, "Request failed"); return false;
    }
    size_t capacity = 8192, used = 0;
    char *response = malloc(capacity + 1);
    if (!response) { close(fd); snprintf(error, error_size, "Out of memory"); return false; }
    bool read_ok = true;
    for (;;) {
        if (used == capacity) {
            if (capacity >= REMOTE_RESPONSE_LIMIT) { read_ok = false; break; }
            size_t next = capacity * 2;
            if (next > REMOTE_RESPONSE_LIMIT) next = REMOTE_RESPONSE_LIMIT;
            char *grown = realloc(response, next + 1);
            if (!grown) { read_ok = false; break; }
            response = grown;
            capacity = next;
        }
        ssize_t count = recv(fd, response + used, capacity - used, 0);
        if (count > 0) used += (size_t)count;
        else if (count == 0) break;
        else if (errno == EINTR) continue;
        else { read_ok = false; break; }
    }
    close(fd);
    response[used] = '\0';
    if (!read_ok) {
        free(response); snprintf(error, error_size, "Response timed out or exceeded 16 MiB"); return false;
    }
    char *header_end = strstr(response, "\r\n\r\n");
    if (!header_end) { free(response); snprintf(error, error_size, "Invalid HTTP response"); return false; }
    int status = 0;
    if (sscanf(response, "HTTP/%*u.%*u %d", &status) != 1) {
        free(response); snprintf(error, error_size, "Invalid HTTP status"); return false;
    }
    if (status < 200 || status >= 300) {
        snprintf(error, error_size, "HTTP %d", status);
        free(response);
        return false;
    }
    size_t header_length = (size_t)(header_end + 4 - response);
    char *content_length_header = strcasestr(response, "\r\nContent-Length:");
    if (content_length_header && content_length_header < header_end) {
        unsigned long long expected = strtoull(content_length_header + 17, NULL, 10);
        if (expected > REMOTE_RESPONSE_LIMIT || expected > used - header_length) {
            free(response); snprintf(error, error_size, "Incomplete HTTP response"); return false;
        }
    }
    if (strcasestr(response, "\r\nTransfer-Encoding: chunked") &&
        strcasestr(response, "\r\nTransfer-Encoding: chunked") < header_end) {
        free(response); snprintf(error, error_size, "Chunked HTTP responses are not supported"); return false;
    }
    size_t length = used - header_length;
    char *body = malloc(length + 1);
    if (!body) { free(response); snprintf(error, error_size, "Out of memory"); return false; }
    memcpy(body, header_end + 4, length);
    body[length] = '\0';
    free(response);
    *body_out = body;
    *body_size = length;
    return true;
}

static int remote_gpu_slot(const GpuInfo *gpus, int count, int index) {
    for (int i = 0; i < count; i++) if (gpus[i].index == index) return i;
    return -1;
}

static bool remote_parse_metrics(RemoteMonitor *monitor, const char *json) {
    const char *array, *array_end;
    if (!json_object_field(json, "gpus", &array, &array_end) || *array != '[') return false;
    int gpu_count = 0;
    const char *cursor = array + 1, *item, *item_end;
    while (json_array_next(&cursor, array_end - 1, &item, &item_end)) {
        (void)item_end;
        if (++gpu_count > MAX_GPUS) return false;
    }
    GpuInfo *gpus = gpu_count ? calloc((size_t)gpu_count, sizeof(*gpus)) : NULL;
    if (gpu_count && !gpus) return false;
    cursor = array + 1;
    for (int i = 0; i < gpu_count && json_array_next(&cursor, array_end - 1, &item, &item_end); i++) {
        GpuInfo *gpu = &gpus[i];
        gpu->source_id = monitor->id;
        gpu->index = i;
        json_get_int(item, "index", &gpu->index);
        json_get_string(item, "name", gpu->name, sizeof(gpu->name));
        json_get_string(item, "source", gpu->data_source, sizeof(gpu->data_source));
        json_get_bool(item, "temperature_valid", &gpu->temperature_valid);
        json_get_double(item, "temperature", &gpu->temperature);
        json_get_bool(item, "power_valid", &gpu->power_valid);
        json_get_double(item, "power", &gpu->power);
        json_get_bool(item, "power_limit_valid", &gpu->power_limit_valid);
        json_get_double(item, "power_limit", &gpu->power_limit);
        json_get_bool(item, "memory_valid", &gpu->memory_valid);
        json_get_uint(item, "memory_used", &gpu->memory_used);
        json_get_uint(item, "memory_total", &gpu->memory_total);
        json_get_bool(item, "utilization_valid", &gpu->utilization_valid);
        json_get_double(item, "utilization", &gpu->utilization);
    }

    SystemInfo system = {0};
    DiskInfo disk = {0};
    NetworkInfo network = {0};
    const char *object;
    if (json_object_field(json, "ram", &object, NULL)) {
        json_get_uint(object, "used", &system.ram_used);
        json_get_uint(object, "total", &system.ram_total);
        system.ram_percent = system.ram_total ? 100.0 * (double)system.ram_used / (double)system.ram_total : 0.0;
    }
    if (json_object_field(json, "swap", &object, NULL)) {
        json_get_uint(object, "used", &system.swap_used);
        json_get_uint(object, "total", &system.swap_total);
        system.swap_percent = system.swap_total ? 100.0 * (double)system.swap_used / (double)system.swap_total : 0.0;
    }
    if (json_object_field(json, "disk", &object, NULL)) {
        json_get_bool(object, "capacity_valid", &disk.capacity_valid);
        json_get_bool(object, "valid", &disk.valid);
        json_get_uint(object, "used", &disk.used_bytes);
        json_get_uint(object, "total", &disk.total_bytes);
        json_get_double(object, "read_bps", &disk.read_bps);
        json_get_double(object, "write_bps", &disk.write_bps);
        json_get_double(object, "total_bps", &disk.total_bps);
    }
    if (json_object_field(json, "network", &object, NULL)) {
        json_get_bool(object, "valid", &network.valid);
        json_get_double(object, "rx_bps", &network.rx_bps);
        json_get_double(object, "tx_bps", &network.tx_bps);
        json_get_double(object, "total_bps", &network.total_bps);
        json_get_bool(object, "utilization_valid", &network.utilization_valid);
        json_get_double(object, "utilization_percent", &network.utilization_percent);
        const char *interfaces, *interfaces_end;
        if (json_object_field(object, "interfaces", &interfaces, &interfaces_end) && *interfaces == '[') {
            const char *interface_cursor = interfaces + 1, *interface_json, *interface_end;
            while (network.interface_count < MAX_NETWORK_INTERFACES &&
                   json_array_next(&interface_cursor, interfaces_end - 1, &interface_json, &interface_end)) {
                (void)interface_end;
                NetworkInterfaceInfo *interface = &network.interfaces[network.interface_count++];
                json_get_string(interface_json, "name", interface->name, sizeof(interface->name));
                json_get_bool(interface_json, "valid", &interface->valid);
                json_get_double(interface_json, "rx_bps", &interface->rx_bps);
                json_get_double(interface_json, "tx_bps", &interface->tx_bps);
                interface->total_bps = interface->rx_bps + interface->tx_bps;
                json_get_double(interface_json, "capacity_bps", &interface->capacity_bps);
                interface->capacity_valid = interface->capacity_bps > 0.0;
                json_get_bool(interface_json, "utilization_valid", &interface->utilization_valid);
                json_get_double(interface_json, "utilization_percent", &interface->utilization_percent);
            }
        }
    }

    const char *processes, *processes_end;
    if (json_object_field(json, "processes", &processes, &processes_end) && *processes == '[') {
        const char *process_cursor = processes + 1, *process_json, *process_end;
        while (json_array_next(&process_cursor, processes_end - 1, &process_json, &process_end)) {
            (void)process_end;
            int gpu_index = -1;
            unsigned long long raw_pid = 0;
            if (!json_get_int(process_json, "gpu", &gpu_index) ||
                !json_get_uint(process_json, "pid", &raw_pid) || raw_pid > INT_MAX) continue;
            int slot = remote_gpu_slot(gpus, gpu_count, gpu_index);
            if (slot < 0 || gpus[slot].process_count >= MAX_GPU_PROCESSES) continue;
            char command[512] = "";
            json_get_string(process_json, "command", command, sizeof(command));
            if (is_ctop_command(command)) continue;
            ProcessInfo *process = &gpus[slot].processes[gpus[slot].process_count++];
            process->pid = (pid_t)raw_pid;
            json_get_string(process_json, "user", process->user, sizeof(process->user));
            snprintf(process->command, sizeof(process->command), "%s", command);
            json_get_bool(process_json, "gpu_memory_valid", &process->gpu_memory_valid);
            json_get_uint(process_json, "gpu_memory", &process->gpu_memory);
            json_get_bool(process_json, "sm_valid", &process->sm_valid);
            json_get_double(process_json, "sm", &process->sm);
            json_get_bool(process_json, "cpu_valid", &process->cpu_valid);
            json_get_double(process_json, "cpu", &process->cpu);
            json_get_bool(process_json, "memory_valid", &process->memory_valid);
            json_get_double(process_json, "memory", &process->memory);
            json_get_string(process_json, "elapsed", process->elapsed_display, sizeof(process->elapsed_display));
        }
    }

    free(monitor->gpus);
    monitor->gpus = gpus;
    monitor->gpu_count = gpu_count;
    monitor->system = system;
    monitor->disk = disk;
    monitor->network = network;
    monitor->connected = true;
    snprintf(monitor->status, sizeof(monitor->status), "Connected");

    double now = monotonic_seconds();
    double memory_average = 0.0, gpu_average = 0.0;
    int memory_count = 0, gpu_count_valid = 0;
    for (int i = 0; i < monitor->gpu_count; i++) {
        GpuInfo *gpu = &monitor->gpus[i];
        if (gpu->memory_valid && gpu->memory_total) {
            memory_average += 100.0 * (double)gpu->memory_used / (double)gpu->memory_total;
            memory_count++;
        }
        if (gpu->utilization_valid) { gpu_average += gpu->utilization; gpu_count_valid++; }
    }
    if (memory_count) history_add(&monitor->vram_history, now, memory_average / memory_count);
    if (gpu_count_valid) history_add(&monitor->gpu_history, now, gpu_average / gpu_count_valid);
    if (monitor->system.ram_total) history_add(&monitor->ram_history, now, monitor->system.ram_percent);
    if (monitor->system.swap_total) history_add(&monitor->swap_history, now, monitor->system.swap_percent);
    if (monitor->disk.valid) history_add(&monitor->disk_history, now, monitor->disk.total_bps);
    if (monitor->network.valid) history_add(&monitor->network_history, now, monitor->network.total_bps);
    return true;
}

static void compose_tui_snapshots(GpuInfo *gpus, int local_gpu_count, int *total_gpu_count) {
    int total = local_gpu_count;
    for (int i = 0; i < local_gpu_count; i++) {
        gpus[i].source_id = 0;
        snprintf(gpus[i].source_prefix, sizeof(gpus[i].source_prefix), "%s",
                 remote_monitor_count ? "A" : "");
    }
    for (int source = 0; source < remote_monitor_count; source++) {
        RemoteMonitor *monitor = &remote_monitors[source];
        monitor->gpu_start = total;
        int available = MAX_TUI_GPUS - total;
        int source_gpu_count = monitor->gpu_count;
        int count = source_gpu_count < available ? source_gpu_count : available;
        if (count < 0) count = 0;
        monitor->gpu_display_count = count;
        for (int i = 0; i < count; i++) {
            gpus[total] = monitor->gpus[i];
            gpus[total].source_id = monitor->id;
            snprintf(gpus[total].source_prefix, sizeof(gpus[total].source_prefix), "%s", monitor->prefix);
            total++;
        }
        if (count < source_gpu_count)
            snprintf(monitor->status, sizeof(monitor->status), "GPU limit reached");
    }
    *total_gpu_count = total;
}

static void set_remote_error_notice(UiState *ui, const RemoteMonitor *monitor, const char *error) {
    if (!ui || !monitor || !error) return;
    if (strncmp(error, "HTTP 401", 8) == 0 || strncmp(error, "HTTP 403", 8) == 0)
        snprintf(ui->notice, sizeof(ui->notice), "Monitor %s: unauthorized; check TOKEN", monitor->prefix);
    else if (strncmp(error, "HTTP 404", 8) == 0)
        snprintf(ui->notice, sizeof(ui->notice), "Monitor %s: HTTP 404; check metrics URL", monitor->prefix);
    else
        snprintf(ui->notice, sizeof(ui->notice), "Monitor %s: %s", monitor->prefix, error);
    ui->notice_error = true;
}

static void clear_remote_error_notice(UiState *ui, const RemoteMonitor *monitor) {
    if (!ui || !monitor) return;
    char prefix[32], retry[32];
    snprintf(prefix, sizeof(prefix), "Monitor %s:", monitor->prefix);
    snprintf(retry, sizeof(retry), "Retrying monitor %s", monitor->prefix);
    if ((ui->notice_error && strncmp(ui->notice, prefix, strlen(prefix)) == 0) ||
        strncmp(ui->notice, retry, strlen(retry)) == 0) {
        ui->notice[0] = '\0';
        ui->notice_error = false;
    }
}

static void refresh_remote_monitors(UiState *ui) {
    for (int source = 0; source < remote_monitor_count; source++) {
        RemoteMonitor *monitor = &remote_monitors[source];
        char *body = NULL, error[128] = "";
        size_t body_size = 0;
        if (remote_http_get(monitor, &body, &body_size, error, sizeof(error))) {
            (void)body_size;
            if (!remote_parse_metrics(monitor, body)) {
                monitor->connected = false;
                snprintf(monitor->status, sizeof(monitor->status), "Invalid metrics response");
                set_remote_error_notice(ui, monitor, monitor->status);
            } else {
                clear_remote_error_notice(ui, monitor);
            }
            free(body);
        } else {
            monitor->connected = false;
            snprintf(monitor->status, sizeof(monitor->status), "%s", error);
            set_remote_error_notice(ui, monitor, error);
        }
    }
}

static void append_remote_snapshots(GpuInfo *gpus, int local_gpu_count,
                                    int *total_gpu_count, UiState *ui) {
    refresh_remote_monitors(ui);
    compose_tui_snapshots(gpus, local_gpu_count, total_gpu_count);
}

static void build_monitor_views(const SystemInfo *local_system, const DiskInfo *local_disk,
                                const NetworkInfo *local_network, const History *local_ram,
                                const History *local_swap, const History *local_disk_history,
                                const History *local_network_history, const History *local_vram,
                                const History *local_gpu_history, int local_gpu_count) {
    memset(tui_views, 0, sizeof(tui_views));
    tui_view_count = 0;
    tui_views[tui_view_count++] = (MonitorView){
        .id = 0, .gpu_start = 0, .gpu_count = local_gpu_count,
        .system = local_system, .disk = local_disk, .network = local_network,
        .ram_history = local_ram, .swap_history = local_swap,
        .disk_history = local_disk_history, .network_history = local_network_history,
        .vram_history = local_vram, .gpu_history = local_gpu_history,
    };
    for (int i = 0; i < remote_monitor_count && (size_t)tui_view_count < ARRAY_LEN(tui_views); i++) {
        RemoteMonitor *monitor = &remote_monitors[i];
        tui_views[tui_view_count++] = (MonitorView){
            .id = monitor->id, .gpu_start = monitor->gpu_start,
            .gpu_count = monitor->gpu_display_count,
            .system = &monitor->system, .disk = &monitor->disk, .network = &monitor->network,
            .ram_history = &monitor->ram_history, .swap_history = &monitor->swap_history,
            .disk_history = &monitor->disk_history, .network_history = &monitor->network_history,
            .vram_history = &monitor->vram_history, .gpu_history = &monitor->gpu_history,
            .gpu_collapsed = monitor->gpu_collapsed,
            .system_collapsed = monitor->system_collapsed,
            .processes_collapsed = monitor->processes_collapsed,
        };
    }
}

/* ------------------------------------------------------------------------- */
/* Embedded HTTP dashboard                                                   */

static void json_string(FILE *stream, const char *value) {
    fputc('"', stream);
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", stream); break;
            case '\\': fputs("\\\\", stream); break;
            case '\b': fputs("\\b", stream); break;
            case '\f': fputs("\\f", stream); break;
            case '\n': fputs("\\n", stream); break;
            case '\r': fputs("\\r", stream); break;
            case '\t': fputs("\\t", stream); break;
            default:
                if (*p < 0x20) fprintf(stream, "\\u%04x", *p);
                else fputc(*p, stream);
        }
    }
    fputc('"', stream);
}

static void json_number(FILE *stream, double value, bool valid) {
    if (!valid || !isfinite(value)) fputs("null", stream);
    else fprintf(stream, "%.3f", value);
}

static void json_uint(FILE *stream, unsigned long long value, bool valid) {
    if (!valid) fputs("null", stream);
    else fprintf(stream, "%llu", value);
}

static void json_history(FILE *stream, const History *history, double now) {
    const size_t max_buckets = 500;
    typedef struct {
        Sample first, last, minimum, maximum;
        size_t first_order, last_order, minimum_order, maximum_order;
        bool valid;
    } HistoryBucket;
    HistoryBucket buckets[500] = {0};
    size_t in_window = 0;
    for (size_t i = 0; i < history->count; i++) {
        Sample sample = history_at(history, i);
        double age = now - sample.timestamp;
        if (age >= 0.0 && age <= 400.0) in_window++;
    }
    size_t step = in_window > max_buckets ? (in_window + max_buckets - 1) / max_buckets : 1;
    size_t seen = 0;
    for (size_t i = 0; i < history->count; i++) {
        Sample sample = history_at(history, i);
        double age = now - sample.timestamp;
        if (age < 0.0 || age > 400.0) continue;
        size_t bucket_index = seen / step;
        if (bucket_index >= max_buckets) bucket_index = max_buckets - 1;
        HistoryBucket *bucket = &buckets[bucket_index];
        if (!bucket->valid) {
            bucket->first = bucket->last = bucket->minimum = bucket->maximum = sample;
            bucket->first_order = bucket->last_order = bucket->minimum_order = bucket->maximum_order = seen;
            bucket->valid = true;
        } else {
            bucket->last = sample;
            bucket->last_order = seen;
            if (sample.value < bucket->minimum.value) {
                bucket->minimum = sample;
                bucket->minimum_order = seen;
            }
            if (sample.value > bucket->maximum.value) {
                bucket->maximum = sample;
                bucket->maximum_order = seen;
            }
        }
        seen++;
    }

    bool comma = false;
    fputc('[', stream);
    size_t bucket_count = (in_window + step - 1) / step;
    for (size_t i = 0; i < bucket_count; i++) {
        HistoryBucket *bucket = &buckets[i];
        if (!bucket->valid) continue;
        Sample samples[4] = {bucket->first, bucket->minimum, bucket->maximum, bucket->last};
        size_t order[4] = {bucket->first_order, bucket->minimum_order,
                           bucket->maximum_order, bucket->last_order};
        for (size_t a = 0; a < 4; a++) {
            for (size_t b = a + 1; b < 4; b++) {
                if (order[b] < order[a]) {
                    size_t order_tmp = order[a]; order[a] = order[b]; order[b] = order_tmp;
                    Sample sample_tmp = samples[a]; samples[a] = samples[b]; samples[b] = sample_tmp;
                }
            }
        }
        for (size_t j = 0; j < 4; j++) {
            if (j && order[j] == order[j - 1]) continue;
            if (comma) fputc(',', stream);
            fprintf(stream, "{\"age_seconds\":%.3f,\"value\":%.3f}",
                    now - samples[j].timestamp, samples[j].value);
            comma = true;
        }
    }
    fputc(']', stream);
}

static char *web_metrics_json(const GpuInfo *gpus, int gpu_count,
                              const SystemInfo *system, const DiskInfo *disk,
                              const NetworkInfo *network,
                              const History *ram, const History *swap,
                              const History *disk_history, const History *network_history,
                              const History *vram, const History *gpu_history,
                              bool include_monitors, size_t *json_size) {
    char *json = NULL;
    FILE *stream = open_memstream(&json, json_size);
    if (!stream) return NULL;
    double now = monotonic_seconds();
    double average_vram = 0.0, average_gpu = 0.0;
    int vram_valid_count = 0, gpu_valid_count = 0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].memory_valid && gpus[i].memory_total) {
            average_vram += 100.0 * (double)gpus[i].memory_used / (double)gpus[i].memory_total;
            vram_valid_count++;
        }
        if (gpus[i].utilization_valid) {
            average_gpu += gpus[i].utilization;
            gpu_valid_count++;
        }
    }
    if (vram_valid_count) average_vram /= vram_valid_count;
    if (gpu_valid_count) average_gpu /= gpu_valid_count;

    time_t wall_time = time(NULL);
    struct tm local_time;
    char timestamp[64] = "";
    if (localtime_r(&wall_time, &local_time))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);

    fputs("{\"version\":", stream);
    json_string(stream, CTOP_VERSION);
    fputs(",\"timestamp\":", stream);
    json_string(stream, timestamp);
    fprintf(stream, ",\"sample_interval\":%.3f", sample_interval_seconds);
    fprintf(stream, ",\"ram\":{\"used\":%llu,\"total\":%llu,\"percent\":",
            system->ram_used, system->ram_total);
    json_number(stream, system->ram_percent, system->ram_total > 0);
    fprintf(stream, "},\"swap\":{\"used\":%llu,\"total\":%llu,\"percent\":",
            system->swap_used, system->swap_total);
    json_number(stream, system->swap_percent, true);
    fprintf(stream, "},\"disk\":{\"used\":%llu,\"total\":%llu,\"capacity_valid\":%s,\"valid\":%s,\"read_bps\":",
            disk->used_bytes, disk->total_bytes,
            disk->capacity_valid ? "true" : "false", disk->valid ? "true" : "false");
    json_number(stream, disk->read_bps, disk->valid);
    fputs(",\"write_bps\":", stream);
    json_number(stream, disk->write_bps, disk->valid);
    fputs(",\"total_bps\":", stream);
    json_number(stream, disk->total_bps, disk->valid);
    fputs("},\"network\":{\"valid\":", stream);
    fputs(network->valid ? "true" : "false", stream);
    fputs(",\"rx_bps\":", stream);
    json_number(stream, network->rx_bps, network->valid);
    fputs(",\"tx_bps\":", stream);
    json_number(stream, network->tx_bps, network->valid);
    fputs(",\"total_bps\":", stream);
    json_number(stream, network->total_bps, network->valid);
    fputs(",\"utilization_valid\":", stream);
    fputs(network->utilization_valid ? "true" : "false", stream);
    fputs(",\"utilization_percent\":", stream);
    json_number(stream, network->utilization_percent, network->utilization_valid);
    fputs(",\"interfaces\":[", stream);
    for (int i = 0; i < network->interface_count; i++) {
        const NetworkInterfaceInfo *interface = &network->interfaces[i];
        if (i) fputc(',', stream);
        fputs("{\"name\":", stream);
        json_string(stream, interface->name);
        fputs(",\"valid\":", stream);
        fputs(interface->valid ? "true" : "false", stream);
        fputs(",\"rx_bps\":", stream);
        json_number(stream, interface->rx_bps, interface->valid);
        fputs(",\"tx_bps\":", stream);
        json_number(stream, interface->tx_bps, interface->valid);
        fputs(",\"capacity_bps\":", stream);
        json_number(stream, interface->capacity_bps, interface->capacity_valid);
        fputs(",\"utilization_valid\":", stream);
        fputs(interface->utilization_valid ? "true" : "false", stream);
        fputs(",\"utilization_percent\":", stream);
        json_number(stream, interface->utilization_percent, interface->utilization_valid);
        fputc('}', stream);
    }
    fputs("]},\"averages\":{\"vram_percent\":", stream);
    json_number(stream, average_vram, vram_valid_count > 0);
    fputs(",\"gpu_percent\":", stream);
    json_number(stream, average_gpu, gpu_valid_count > 0);
    fputs("},\"gpus\":[", stream);
    bool first_gpu = true;
    for (int i = 0; i < gpu_count; i++) {
        const GpuInfo *gpu = &gpus[i];
        if (!first_gpu) fputc(',', stream);
        first_gpu = false;
        fprintf(stream, "{\"index\":%d,\"name\":", gpu->index);
        json_string(stream, gpu->name);
        fputs(",\"source\":", stream);
        json_string(stream, gpu->data_source);
        fputs(",\"temperature_valid\":", stream);
        fputs(gpu->temperature_valid ? "true" : "false", stream);
        fputs(",\"temperature\":", stream);
        json_number(stream, gpu->temperature, gpu->temperature_valid);
        fputs(",\"power_valid\":", stream);
        fputs(gpu->power_valid ? "true" : "false", stream);
        fputs(",\"power\":", stream);
        json_number(stream, gpu->power, gpu->power_valid);
        fputs(",\"power_limit_valid\":", stream);
        fputs(gpu->power_limit_valid ? "true" : "false", stream);
        fputs(",\"power_limit\":", stream);
        json_number(stream, gpu->power_limit, gpu->power_limit_valid);
        fputs(",\"memory_valid\":", stream);
        fputs(gpu->memory_valid ? "true" : "false", stream);
        fputs(",\"memory_used\":", stream);
        json_uint(stream, gpu->memory_used, gpu->memory_valid);
        fputs(",\"memory_total\":", stream);
        json_uint(stream, gpu->memory_total, gpu->memory_valid);
        fputs(",\"utilization_valid\":", stream);
        fputs(gpu->utilization_valid ? "true" : "false", stream);
        fputs(",\"utilization\":", stream);
        json_number(stream, gpu->utilization, gpu->utilization_valid);
        fputc('}', stream);
    }
    fputs("],\"processes\":[", stream);
    bool first_process = true;
    for (int i = 0; i < gpu_count; i++) {
        const GpuInfo *gpu = &gpus[i];
        for (size_t j = 0; j < gpu->process_count; j++) {
            const ProcessInfo *process = &gpu->processes[j];
            if (!first_process) fputc(',', stream);
            first_process = false;
            fprintf(stream, "{\"gpu\":%d,\"pid\":%ld,\"user\":", gpu->index, (long)process->pid);
            json_string(stream, process->user);
            fputs(",\"command\":", stream);
            json_string(stream, process->command);
            fputs(",\"gpu_memory_valid\":", stream);
            fputs(process->gpu_memory_valid ? "true" : "false", stream);
            fputs(",\"gpu_memory\":", stream);
            json_uint(stream, process->gpu_memory, process->gpu_memory_valid);
            fputs(",\"sm_valid\":", stream);
            fputs(process->sm_valid ? "true" : "false", stream);
            fputs(",\"sm\":", stream);
            json_number(stream, process->sm, process->sm_valid);
            fputs(",\"cpu_valid\":", stream);
            fputs(process->cpu_valid ? "true" : "false", stream);
            fputs(",\"cpu\":", stream);
            json_number(stream, process->cpu, process->cpu_valid);
            fputs(",\"memory_valid\":", stream);
            fputs(process->memory_valid ? "true" : "false", stream);
            fputs(",\"memory\":", stream);
            json_number(stream, process->memory, process->memory_valid);
            char elapsed[32];
            if (process->elapsed_display[0])
                snprintf(elapsed, sizeof(elapsed), "%s", process->elapsed_display);
            else
                format_duration(elapsed, sizeof(elapsed), process->elapsed, process->elapsed_valid);
            fputs(",\"elapsed\":", stream);
            json_string(stream, elapsed);
            fputc('}', stream);
        }
    }
    fputs("],\"history\":{\"ram\":", stream);
    json_history(stream, ram, now);
    fputs(",\"swap\":", stream);
    json_history(stream, swap, now);
    fputs(",\"disk\":", stream);
    json_history(stream, disk_history, now);
    fputs(",\"network\":", stream);
    json_history(stream, network_history, now);
    fputs(",\"vram\":", stream);
    json_history(stream, vram, now);
    fputs(",\"gpu\":", stream);
    json_history(stream, gpu_history, now);
    fputc('}', stream);
    if (include_monitors) {
        fputs(",\"monitors_protected\":false,\"monitors\":[", stream);
        for (int i = 0; i < remote_monitor_count; i++) {
                const RemoteMonitor *monitor = &remote_monitors[i];
                if (i) fputc(',', stream);
                fprintf(stream, "{\"id\":%d,\"prefix\":", monitor->id);
                json_string(stream, monitor->prefix);
                char safe_address[REMOTE_ADDRESS_SIZE];
                remote_monitor_public_address(monitor, safe_address, sizeof(safe_address));
                fputs(",\"address\":", stream);
                json_string(stream, safe_address);
                fputs(",\"host\":", stream);
                json_string(stream, monitor->host);
                fprintf(stream, ",\"port\":%d,\"connected\":%s,\"status\":",
                        monitor->port, monitor->connected ? "true" : "false");
                json_string(stream, monitor->status);
                fputs(",\"metrics\":", stream);
                size_t monitor_json_size = 0;
                char *monitor_json = web_metrics_json(
                    monitor->gpus, monitor->gpu_count, &monitor->system, &monitor->disk,
                    &monitor->network, &monitor->ram_history, &monitor->swap_history,
                    &monitor->disk_history, &monitor->network_history,
                    &monitor->vram_history, &monitor->gpu_history, false,
                    &monitor_json_size);
                if (monitor_json) {
                    fwrite(monitor_json, 1, monitor_json_size, stream);
                    free(monitor_json);
                } else {
                    fputs("null", stream);
                }
            fputc('}', stream);
        }
        fputc(']', stream);
    }
    fputc('}', stream);
    fclose(stream);
    return json;
}

static bool web_send_all(int fd, const void *data, size_t size) {
    const char *cursor = data;
    while (size) {
        ssize_t sent = send(fd, cursor, size, MSG_NOSIGNAL);
        if (sent > 0) {
            cursor += sent;
            size -= (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void web_send_response(int fd, int status, const char *reason,
                              const char *content_type, const void *body, size_t body_size);

/* Optional extra_headers are inserted as complete lines before the blank line
   that ends the response header block. */
static void web_send_response_extra(int fd, int status, const char *reason,
                                    const char *content_type, const void *body, size_t body_size,
                                    const char *extra_headers) {
    char header[1024];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                          "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                          "%s"
                          "Connection: close\r\n\r\n",
                          status, reason, content_type, body_size,
                          extra_headers ? extra_headers : "");
    if (length <= 0 || (size_t)length >= sizeof(header)) return;
    if (!web_send_all(fd, header, (size_t)length)) return;
    if (body_size) web_send_all(fd, body, body_size);
}

/* The access token may be supplied as a bearer header, a `token=` query
   parameter, or the cookie handed out by /login.  Comparison is constant time
   so the endpoint cannot be used to probe the token one byte at a time. */
static bool web_token_equal(const char *candidate) {
    if (!web_token || !candidate) return false;
    size_t expected = strlen(web_token), given = strlen(candidate);
    if (expected != given) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected; i++)
        difference |= (unsigned char)(web_token[i] ^ candidate[i]);
    return difference == 0;
}

static bool web_query_param(const char *query, const char *name, char *out, size_t out_size) {
    if (!query || !out_size) return false;
    size_t name_length = strlen(name);
    const char *at = query;
    while ((at = strstr(at, name)) != NULL) {
        bool boundary = (at == query) || at[-1] == '?' || at[-1] == '&';
        if (boundary && at[name_length] == '=') {
            const char *value = at + name_length + 1;
            size_t length = 0;
            while (value[length] && value[length] != '&' && value[length] != ' ' &&
                   value[length] != '\r' && value[length] != '\n' && length + 1 < out_size) {
                out[length] = value[length];
                length++;
            }
            out[length] = '\0';
            return length > 0;
        }
        at += name_length;
    }
    return false;
}

static bool web_header_value(const char *request, const char *name, char *out, size_t out_size) {
    size_t name_length = strlen(name);
    const char *line = request;
    while (line && *line) {
        const char *next = strchr(line, '\n');
        size_t line_length = next ? (size_t)(next - line) : strlen(line);
        if (line_length > name_length + 1 && strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            const char *value = line + name_length + 1;
            const char *end = line + line_length;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            size_t length = 0;
            while (value < end && *value != '\r' && length + 1 < out_size) {
                out[length++] = *value++;
            }
            out[length] = '\0';
            return length > 0;
        }
        if (!next) break;
        line = next + 1;
    }
    return false;
}

static bool web_cookie_token(const char *request, char *out, size_t out_size) {
    char cookie[1024];
    if (!web_header_value(request, "Cookie", cookie, sizeof(cookie))) return false;
    const char *marker = strstr(cookie, "ctop_token=");
    if (!marker) return false;
    marker += strlen("ctop_token=");
    size_t length = 0;
    while (marker[length] && marker[length] != ';' && marker[length] != ' ' && length + 1 < out_size) {
        out[length] = marker[length];
        length++;
    }
    out[length] = '\0';
    return length > 0;
}

static bool web_bearer_token(const char *request, char *out, size_t out_size) {
    char header[512];
    if (!web_header_value(request, "Authorization", header, sizeof(header))) return false;
    const char *scheme = "Bearer ";
    size_t scheme_length = strlen(scheme);
    if (strncasecmp(header, scheme, scheme_length) != 0) return false;
    size_t length = 0;
    const char *value = header + scheme_length;
    while (*value == ' ') value++;
    while (*value && length + 1 < out_size) out[length++] = *value++;
    out[length] = '\0';
    return length > 0;
}

static bool web_request_authorized(const char *request, const char *query) {
    char candidate[256];
    if (web_bearer_token(request, candidate, sizeof(candidate)) && web_token_equal(candidate)) return true;
    if (web_query_param(query, "token", candidate, sizeof(candidate)) && web_token_equal(candidate)) return true;
    if (web_cookie_token(request, candidate, sizeof(candidate)) && web_token_equal(candidate)) return true;
    return false;
}

static const char web_login_form_head[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ctop - sign in</title>"
    "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/favicon.svg\">"
    "<style>"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;background:#101319;color:#ecf0f6;"
    "font-family:ui-sans-serif,system-ui,sans-serif}"
    ".card{background:#171b23;border:1px solid #2a303c;border-radius:14px;padding:26px 28px;"
    "width:min(360px,calc(100vw - 40px));box-shadow:0 12px 32px #0002}"
    "h1{margin:0 0 6px;font-size:19px}.sub{margin:0 0 18px;color:#8b95a5;font-size:13px}"
    "label{display:block;font:11px ui-monospace,monospace;color:#8b95a5;margin-bottom:6px}"
    "input{width:100%;box-sizing:border-box;padding:10px 12px;border-radius:9px;border:1px solid #2a303c;"
    "background:#101319;color:#ecf0f6;font:14px ui-monospace,monospace}"
    "button{margin-top:14px;width:100%;padding:10px;border:0;border-radius:9px;background:#70e0b1;"
    "color:#0b1913;font-weight:700;cursor:pointer}"
    ".error{margin:12px 0 0;color:#ff747c;font-size:12px}</style></head>"
    "<body><form class=\"card\" method=\"get\" action=\"/login\">"
    "<h1>ctop</h1><p class=\"sub\">Access token required</p>"
    "<label for=\"token\">TOKEN</label>"
    "<input id=\"token\" name=\"token\" type=\"password\" autocomplete=\"current-password\" autofocus>";

static const char web_login_form_tail[] = "<button type=\"submit\">Sign in</button></form></body></html>";

static void web_send_login_page(int fd, bool invalid) {
    char page[2048];
    int length = snprintf(page, sizeof(page), "%s%s%s",
                          web_login_form_head,
                          invalid ? "<p class=\"error\">Invalid token</p>" : "",
                          web_login_form_tail);
    if (length <= 0 || (size_t)length >= sizeof(page)) return;
    web_send_response(fd, invalid ? 401 : 200, invalid ? "Unauthorized" : "OK",
                      "text/html; charset=utf-8", page, (size_t)length);
}

static void web_send_response(int fd, int status, const char *reason,
                              const char *content_type, const void *body, size_t body_size) {
    web_send_response_extra(fd, status, reason, content_type, body, body_size, NULL);
}

static void web_serve_client(int fd, const GpuInfo *gpus, int gpu_count,
                             const SystemInfo *system, const DiskInfo *disk,
                             const NetworkInfo *network, const History *ram,
                             const History *swap, const History *disk_history,
                             const History *network_history, const History *vram,
                             const History *gpu_history, UiState *ui) {
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[4096];
    size_t used = 0;
    while (used + 1 < sizeof(request)) {
        ssize_t received = recv(fd, request + used, sizeof(request) - used - 1, 0);
        if (received > 0) {
            used += (size_t)received;
            request[used] = '\0';
            /* Authorization lives in the header block, so read past the request
               line until the terminating blank line arrives. */
            if (strstr(request, "\r\n\r\n") || strstr(request, "\n\n")) break;
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    request[used] = '\0';
    char method[8], path[1024];
    if (sscanf(request, "%7s %1023s", method, path) != 2) return;
    char *query = strchr(path, '?');
    const char *query_string = query ? query + 1 : NULL;
    if (query) *query = '\0';

    bool metrics_request = strcmp(path, "/api/v1/metrics") == 0 || strcmp(path, "/api/metrics") == 0;
    bool monitor_collection = strcmp(path, "/api/v1/monitors") == 0;
    bool monitor_item = strncmp(path, "/api/v1/monitors/", 17) == 0;
    bool api_request = metrics_request || monitor_collection || monitor_item;
    /* The icon carries no telemetry, so serving it unauthenticated lets the sign-in
       page show it too. */
    bool favicon_request = strcmp(path, "/favicon.svg") == 0 || strcmp(path, "/favicon.ico") == 0;

    if (strcmp(method, "OPTIONS") == 0 && metrics_request) {
        web_send_response_extra(fd, 204, "No Content", "application/json", "", 0,
                                "Access-Control-Allow-Origin: *\r\n"
                                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                                "Access-Control-Allow-Headers: Authorization\r\n"
                                "Access-Control-Max-Age: 600\r\n");
        return;
    }
    bool add_monitor_request = monitor_collection && strcmp(method, "POST") == 0;
    int remove_monitor_id = -1;
    bool remove_monitor_request = false;
    if (monitor_item && strcmp(method, "DELETE") == 0) {
        const char *id_text = path + 17;
        char *end = NULL;
        long parsed_id = strtol(id_text, &end, 10);
        if (*id_text && end && !*end && parsed_id > 0 && parsed_id <= INT_MAX) {
            remove_monitor_id = (int)parsed_id;
            remove_monitor_request = true;
        }
    }
    if (strcmp(method, "GET") != 0 && !add_monitor_request && !remove_monitor_request) {
        static const char body[] = "Method not allowed or route not found\n";
        web_send_response(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                          body, sizeof(body) - 1);
        return;
    }

    if (web_token) {
        if (strcmp(path, "/login") == 0) {
            char candidate[256];
            if (web_query_param(query_string, "token", candidate, sizeof(candidate)) &&
                web_token_equal(candidate)) {
                char extra[384];
                snprintf(extra, sizeof(extra),
                         "Location: /\r\nSet-Cookie: ctop_token=%s; Path=/; HttpOnly; SameSite=Lax;"
                         " Max-Age=604800\r\n", web_token);
                web_send_response_extra(fd, 302, "Found", "text/plain; charset=utf-8", "", 0, extra);
            } else {
                web_send_login_page(fd, query_string != NULL);
            }
            return;
        }
        if (!favicon_request && !web_request_authorized(request, query_string)) {
            if (api_request) {
                static const char body[] = "{\"error\":\"unauthorized\"}\n";
                web_send_response_extra(fd, 401, "Unauthorized", "application/json; charset=utf-8",
                                        body, sizeof(body) - 1,
                                        "WWW-Authenticate: Bearer realm=\"ctop\"\r\n"
                                        "Access-Control-Allow-Origin: *\r\n");
            } else {
                web_send_login_page(fd, false);
            }
            return;
        }
    }

    if (add_monitor_request || remove_monitor_request) {
        if (add_monitor_request) {
            char content_length_text[32];
            if (!web_header_value(request, "Content-Length", content_length_text,
                                  sizeof(content_length_text))) {
                static const char body[] = "Content-Length required\n";
                web_send_response(fd, 411, "Length Required", "text/plain; charset=utf-8",
                                  body, sizeof(body) - 1);
                return;
            }
            char *end = NULL;
            unsigned long parsed_length = strtoul(content_length_text, &end, 10);
            char *header_end = strstr(request, "\r\n\r\n");
            size_t body_offset = header_end ? (size_t)(header_end + 4 - request) : 0;
            if (!end || *end || parsed_length == 0 || parsed_length > 2048 || !body_offset ||
                body_offset + (size_t)parsed_length >= sizeof(request)) {
                static const char body[] = "Invalid request body size\n";
                web_send_response(fd, 413, "Content Too Large", "text/plain; charset=utf-8",
                                  body, sizeof(body) - 1);
                return;
            }
            size_t body_received = used > body_offset ? used - body_offset : 0;
            while (body_received < (size_t)parsed_length) {
                ssize_t received = recv(fd, request + body_offset + body_received,
                                        (size_t)parsed_length - body_received, 0);
                if (received > 0) body_received += (size_t)received;
                else if (received < 0 && errno == EINTR) continue;
                else break;
            }
            if (body_received != (size_t)parsed_length) {
                static const char body[] = "Incomplete request body\n";
                web_send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                  body, sizeof(body) - 1);
                return;
            }
            request[body_offset + body_received] = '\0';
            char address[REMOTE_ADDRESS_SIZE] = "", token[REMOTE_TOKEN_SIZE] = "";
            if (!json_get_string(request + body_offset, "address", address, sizeof(address)) ||
                !address[0]) {
                static const char body[] = "Address is required\n";
                web_send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                  body, sizeof(body) - 1);
                return;
            }
            json_get_string(request + body_offset, "token", token, sizeof(token));
            int old_count = remote_monitor_count;
            char error[160] = "";
            int id = add_remote_monitor(address, token[0] ? token : NULL, error, sizeof(error));
            if (id < 0) {
                char body[256];
                int length = snprintf(body, sizeof(body), "Could not add monitor: %s\n",
                                      error[0] ? error : "invalid configuration");
                if (length < 0 || (size_t)length >= sizeof(body)) return;
                web_send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                  body, (size_t)length);
                return;
            }
            RemoteMonitor *monitor = remote_monitor_by_id(id);
            if (ui && remote_monitor_count != old_count) rebuild_dashboard_sections(ui);
            char *body = NULL;
            size_t body_size = 0;
            FILE *response = open_memstream(&body, &body_size);
            if (!response) return;
            fprintf(response, "{\"id\":%d,\"prefix\":", id);
            json_string(response, monitor ? monitor->prefix : "");
            char safe_address[REMOTE_ADDRESS_SIZE] = "";
            remote_monitor_public_address(monitor, safe_address, sizeof(safe_address));
            fprintf(response, ",\"address\":");
            json_string(response, safe_address);
            fprintf(response, ",\"duplicate\":%s}",
                    remote_monitor_count == old_count ? "true" : "false");
            fclose(response);
            web_send_response(fd, 200, "OK", "application/json; charset=utf-8", body, body_size);
            free(body);
            return;
        }
        if (remove_remote_monitor(remove_monitor_id)) {
            if (ui) rebuild_dashboard_sections(ui);
            static const char body[] = "{\"removed\":true}\n";
            web_send_response(fd, 200, "OK", "application/json; charset=utf-8",
                              body, sizeof(body) - 1);
        } else {
            static const char body[] = "Monitor not found\n";
            web_send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                              body, sizeof(body) - 1);
        }
        return;
    }

    if (metrics_request) {
        size_t json_size = 0;
        char *json = web_metrics_json(gpus, gpu_count, system, disk, network,
                                      ram, swap, disk_history, network_history,
                                      vram, gpu_history, true, &json_size);
        if (json) {
            web_send_response_extra(fd, 200, "OK", "application/json; charset=utf-8",
                                    json, json_size, "Access-Control-Allow-Origin: *\r\n");
            free(json);
        } else {
            static const char body[] = "Unable to serialize metrics\n";
            web_send_response_extra(fd, 500, "Internal Server Error", "text/plain; charset=utf-8",
                                    body, sizeof(body) - 1,
                                    "Access-Control-Allow-Origin: *\r\n");
        }
        return;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        web_send_response(fd, 200, "OK", "text/html; charset=utf-8",
                          CTOP_WEB_INDEX, CTOP_WEB_INDEX_SIZE);
    } else if (strcmp(path, "/style.css") == 0) {
        web_send_response(fd, 200, "OK", "text/css; charset=utf-8",
                          CTOP_WEB_CSS, CTOP_WEB_CSS_SIZE);
    } else if (strcmp(path, "/app.js") == 0) {
        web_send_response(fd, 200, "OK", "text/javascript; charset=utf-8",
                          CTOP_WEB_JS, CTOP_WEB_JS_SIZE);
    } else if (strcmp(path, "/html2canvas.min.js") == 0) {
        web_send_response(fd, 200, "OK", "text/javascript; charset=utf-8",
                          CTOP_WEB_HTML2CANVAS, CTOP_WEB_HTML2CANVAS_SIZE);
    } else if (strcmp(path, "/favicon.svg") == 0 || strcmp(path, "/favicon.ico") == 0) {
        /* Browsers request /favicon.ico unprompted; the same vector icon answers both. */
        web_send_response(fd, 200, "OK", "image/svg+xml",
                          CTOP_WEB_FAVICON, CTOP_WEB_FAVICON_SIZE);
    } else {
        static const char body[] = "Not found\n";
        web_send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                          body, sizeof(body) - 1);
    }
}

static int web_server_open(int port, bool local_only) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(local_only ? INADDR_LOOPBACK : INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 16) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void web_serve_pending(int listener, const GpuInfo *gpus, int gpu_count,
                              const SystemInfo *system, const DiskInfo *disk,
                              const NetworkInfo *network, const History *ram,
                              const History *swap, const History *disk_history,
                              const History *network_history, const History *vram,
                              const History *gpu_history, UiState *ui) {
    for (int served = 0; served < 8; served++) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                debug_log("web accept failed: %s", strerror(errno));
            return;
        }
        web_serve_client(client, gpus, gpu_count, system, disk, network,
                         ram, swap, disk_history, network_history, vram, gpu_history, ui);
        close(client);
    }
}

static int web_only_run(int listener, double interval, GpuInfo *gpus, int *gpu_count,
                        SystemInfo *system, DiskInfo *disk, NetworkInfo *network,
                        unsigned long long *total_ticks, History *ram, History *swap,
                        History *disk_history, History *network_history,
                        History *vram, History *gpu_history, int port,
                        bool local_only) {
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    if (local_only)
        printf("ctop web monitor: http://127.0.0.1:%d/%s (Ctrl-C to stop)\n", port,
               web_token ? " [token protected]" : "");
    else
        printf("ctop web monitor listening on 0.0.0.0:%d%s (Ctrl-C to stop)\n", port,
               web_token ? " [token protected]" : "");
    fflush(stdout);
    double next_sample = 0.0;
    while (!stop_requested) {
        double now = monotonic_seconds();
        if (now >= next_sample) {
            int result = collect_snapshot(gpus, gpu_count, system, disk, network, total_ticks);
            if (result == 0) {
                refresh_remote_monitors(NULL);
                double average_vram = 0.0, average_gpu = 0.0;
                int vram_valid_count = 0, gpu_valid_count = 0;
                for (int i = 0; i < *gpu_count; i++) {
                    if (gpus[i].memory_valid && gpus[i].memory_total) {
                        average_vram += 100.0 * (double)gpus[i].memory_used / (double)gpus[i].memory_total;
                        vram_valid_count++;
                    }
                    if (gpus[i].utilization_valid) {
                        average_gpu += gpus[i].utilization;
                        gpu_valid_count++;
                    }
                }
                if (vram_valid_count) average_vram /= vram_valid_count;
                if (gpu_valid_count) average_gpu /= gpu_valid_count;
                double timestamp = monotonic_seconds();
                if (vram_valid_count) history_add(vram, timestamp, average_vram);
                if (gpu_valid_count) history_add(gpu_history, timestamp, average_gpu);
                history_add(ram, timestamp, system->ram_percent);
                history_add(swap, timestamp, system->swap_percent);
                if (disk->valid) history_add(disk_history, timestamp, disk->total_bps);
                if (network->valid)
                    history_add(network_history, timestamp, network->total_bps);
            } else {
                debug_log("web snapshot failed result=%d", result);
            }
            next_sample = monotonic_seconds() + interval;
        }
        double wait = next_sample - monotonic_seconds();
        if (wait < 0.0) wait = 0.0;
        if (wait > 1.0) wait = 1.0;
        struct pollfd descriptor = {.fd = listener, .events = POLLIN};
        int polled = poll(&descriptor, 1, (int)(wait * 1000.0));
        if (polled > 0 && (descriptor.revents & POLLIN))
            web_serve_pending(listener, gpus, *gpu_count, system, disk, network,
                              ram, swap, disk_history, network_history, vram, gpu_history, NULL);
        else if (polled < 0 && errno != EINTR)
            debug_log("web poll failed: %s", strerror(errno));
    }
    return 0;
}

static bool valid_web_token(const char *token) {
    if (!token) return false;
    size_t length = strlen(token);
    if (length < 8 || length > 128) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)token[i];
        bool allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '~';
        if (!allowed) return false;
    }
    return true;
}

static void usage(const char *program) {
    printf("Usage: %s [-i seconds] [-p|--port PORT] [--no-tui] [--localhost] [-t|--token TOKEN] [--monitor ADDRESS] [--no-color] [--debug-log FILE] [-a|--alternate-screen] [-r|--restore-screen] [-v|--version]\n", program);
    printf("  -i, --interval N       sample interval, default 1 second\n");
    printf("  -p, --port PORT        serve the web monitor alongside the TUI\n");
    printf("  --no-tui               run web monitor only (requires --port)\n");
    printf("  --localhost            restrict web access to 127.0.0.1 (requires --port)\n");
    printf("  -t, --token TOKEN      require an access token for the web monitor and API\n");
    printf("                         (requires --port; CTOP_TOKEN is also accepted)\n");
    printf("  --monitor ADDRESS      add a shared remote HTTP monitor (repeatable)\n");
    printf("  --no-color             disable ANSI colors\n");
    printf("  --debug-log FILE       append snapshot/render diagnostics to FILE\n");
    printf("  -a, --alternate-screen     restore the previous screen on exit\n");
    printf("  -r, --restore-screen       alias for --alternate-screen\n");
    printf("  -v, --version              show version\n");
}

int main(int argc, char **argv) {
    double interval = 1.0;
    int web_port = 0;
    bool no_tui = false;
    bool local_only = false;
    const char *token_argument = NULL;
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
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            char *end = NULL;
            errno = 0;
            long parsed_port = strtol(argv[i], &end, 10);
            if (errno || !end || *end || parsed_port < 1 || parsed_port > 65535) {
                fprintf(stderr, "port must be an integer from 1 to 65535\n");
                return 2;
            }
            web_port = (int)parsed_port;
        } else if (strcmp(argv[i], "--no-tui") == 0 || strcmp(argv[i], "--web-only") == 0) {
            no_tui = true;
        } else if (strcmp(argv[i], "--localhost") == 0 || strcmp(argv[i], "--local-only") == 0) {
            local_only = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--token") == 0) {
            if (++i >= argc || !argv[i][0]) { usage(argv[0]); return 2; }
            token_argument = argv[i];
        } else if (strcmp(argv[i], "--monitor") == 0) {
            if (++i >= argc || !argv[i][0]) { usage(argv[0]); return 2; }
            char monitor_error[128] = "";
            if (add_remote_monitor(argv[i], NULL, monitor_error, sizeof(monitor_error)) < 0) {
                fprintf(stderr, "cannot add monitor: %s\n", monitor_error);
                return 2;
            }
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
    if (no_tui && !web_port) {
        fprintf(stderr, "--no-tui requires -p/--port\n");
        return 2;
    }
    if (local_only && !web_port) {
        fprintf(stderr, "--localhost requires -p/--port\n");
        return 2;
    }
    const char *environment_token = getenv("CTOP_TOKEN");
    web_token = token_argument ? token_argument
            : (environment_token && environment_token[0] ? environment_token : NULL);
    if (web_token) {
        if (!web_port) {
            fprintf(stderr, "--token requires -p/--port\n");
            return 2;
        }
        if (!valid_web_token(web_token)) {
            fprintf(stderr, "access token must be 8-128 characters of A-Z a-z 0-9 . _ - ~\n");
            return 2;
        }
    }
    sample_interval_seconds = interval;
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
    if (!no_tui && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        debug_log("startup failed: stdin/stdout is not a TTY");
        fprintf(stderr, "ctop requires an interactive terminal\n");
        return 2;
    }

    char driver[96] = "N/A";
    char cuda_library[64] = "N/A", cuda_runtime[64] = "N/A";
    int cuda_driver = 0;
#if defined(CTOP_ENABLE_NVML)
    char nvml_error[256] = "";
    if (!initialize_nvml_backend(nvml_error, sizeof(nvml_error))) {
        debug_log("%s", nvml_error);
#if !defined(CTOP_ENABLE_VULKAN)
        fprintf(stderr, "%s\n", nvml_error);
        return 1;
#endif
    } else {
        debug_log("NVML initialized");
        if (nvml_api.system_get_driver_version)
            nvml_api.system_get_driver_version(driver, sizeof(driver));
        if (nvml_api.system_get_cuda_driver_version_v2)
            nvml_api.system_get_cuda_driver_version_v2(&cuda_driver);
        detect_cuda_library(cuda_library, sizeof(cuda_library), cuda_runtime, sizeof(cuda_runtime));
    }
#endif
#if defined(CTOP_ENABLE_VULKAN)
    char vulkan_error[256] = "";
    if (!initialize_vulkan_backend(vulkan_error, sizeof(vulkan_error)))
        debug_log("%s", vulkan_error);
    if (vulkan_gpu_count) {
        char rocm_error[256] = "";
        if (initialize_rocm_backend(rocm_error, sizeof(rocm_error)))
            debug_log("ROCm backend initialized");
        else
            debug_log("%s", rocm_error);
    }
    if (strcmp(driver, "N/A") == 0) snprintf(driver, sizeof(driver), "Vulkan/AMDGPU");
#endif
    debug_log("driver=%s cuda_library=%s cuda_runtime=%s cuda_driver=%d",
              driver, cuda_library, cuda_runtime, cuda_driver);

    History vram = {0}, gpu_util = {0}, ram = {0}, swap = {0};
    History disk_history = {0}, network_history = {0};
    if (!history_init(&vram) || !history_init(&gpu_util) || !history_init(&ram) ||
        !history_init(&swap) || !history_init(&disk_history) ||
        !history_init(&network_history)) {
        debug_log("startup failed: history allocation");
        fprintf(stderr, "history allocation failed\n");
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&disk_history); history_free(&network_history);
        shutdown_gpu_backends();
        return 1;
    }

    int web_listener = -1;
    if (web_port) {
        web_listener = web_server_open(web_port, local_only);
        if (web_listener < 0) {
            fprintf(stderr, "cannot listen on port %d: %s\n", web_port, strerror(errno));
            history_free(&vram); history_free(&gpu_util); history_free(&ram);
            history_free(&swap); history_free(&disk_history); history_free(&network_history);
            shutdown_gpu_backends();
            return 1;
        }
        active_web_port = web_port;
        format_active_web_url(active_web_url, sizeof(active_web_url), web_port, local_only);
        debug_log("web monitor listening on port %d local_only=%d url=%s",
                  web_port, local_only, active_web_url);
    }

    if (no_tui) {
        GpuInfo *web_gpus = calloc(MAX_GPUS, sizeof(*web_gpus));
        if (!web_gpus) {
            fprintf(stderr, "GPU state allocation failed\n");
            close(web_listener);
            history_free(&vram); history_free(&gpu_util); history_free(&ram);
            history_free(&swap); history_free(&disk_history); history_free(&network_history);
            shutdown_gpu_backends();
            return 1;
        }
        int web_gpu_count = 0;
        SystemInfo web_system = {0};
        DiskInfo web_disk = {0};
        NetworkInfo web_network = {0};
        unsigned long long web_total_ticks = 0;
        int web_result = web_only_run(web_listener, interval, web_gpus, &web_gpu_count,
                                      &web_system, &web_disk, &web_network,
                                      &web_total_ticks, &ram, &swap, &disk_history,
                                      &network_history, &vram, &gpu_util, web_port,
                                      local_only);
        free(web_gpus);
        close(web_listener);
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&disk_history); history_free(&network_history);
        shutdown_gpu_backends();
        return web_result;
    }

    if (!enter_terminal()) {
        debug_log("startup failed: could not enter raw terminal mode (%s)", strerror(errno));
        fprintf(stderr, "could not enter raw terminal mode\n");
        if (web_listener >= 0) close(web_listener);
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&disk_history); history_free(&network_history);
        shutdown_gpu_backends();
        return 1;
    }

    int rows = 24, cols = 120;
    terminal_size(&rows, &cols);
    debug_log("terminal size rows=%d cols=%d", rows, cols);
    Screen screen = {0};
    if (!screen_init(&screen, rows, cols)) {
        debug_log("startup failed: screen allocation rows=%d cols=%d", rows, cols);
        restore_terminal();
        if (web_listener >= 0) close(web_listener);
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&disk_history); history_free(&network_history);
        shutdown_gpu_backends();
        return 1;
    }
    GpuInfo *gpus = calloc(MAX_TUI_GPUS, sizeof(*gpus));
    if (!gpus) {
        debug_log("startup failed: GPU state allocation");
        screen_free(&screen);
        restore_terminal();
        if (web_listener >= 0) close(web_listener);
        history_free(&vram); history_free(&gpu_util); history_free(&ram);
        history_free(&swap); history_free(&disk_history); history_free(&network_history);
        shutdown_gpu_backends();
        return 1;
    }
    int local_gpu_count = 0, gpu_count = 0;
    SystemInfo system = {0};
    DiskInfo disk = {0};
    NetworkInfo network = {0};
    unsigned long long total_ticks = 0;
    UiState ui = {0};
    rebuild_dashboard_sections(&ui);
    build_monitor_views(&system, &disk, &network, &ram, &swap, &disk_history,
                        &network_history, &vram, &gpu_util, local_gpu_count);
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
            int result = collect_snapshot(gpus, &local_gpu_count, &system, &disk,
                                          &network, &total_ticks);
            gpu_count = local_gpu_count;
            double sample_duration = monotonic_seconds() - sample_started;
            if (result != 0) {
                debug_log("snapshot failed result=%d duration=%.3fs", result, sample_duration);
                snprintf(error, sizeof(error), "snapshot error (%d)", result);
            } else {
                debug_log("local snapshot done duration=%.3fs gpus=%d ram=%.1f%% swap=%.1f%% net_valid=%d net=%.1fbps net_load=%.1f%%",
                          sample_duration, local_gpu_count, system.ram_percent, system.swap_percent,
                          network.valid, network.total_bps, network.utilization_percent);
                for (int i = 0; i < local_gpu_count; i++)
                    debug_log("gpu[%d] mem_valid=%d mem=%llu/%llu util_valid=%d util=%.1f",
                              gpus[i].index, gpus[i].memory_valid,
                              gpus[i].memory_used, gpus[i].memory_total,
                              gpus[i].utilization_valid, gpus[i].utilization);
                error[0] = '\0';
                ui.network_interface_count = network.interface_count;
                double average_vram = 0.0, average_gpu = 0.0;
                int vram_valid_count = 0, gpu_valid_count = 0;
                for (int i = 0; i < local_gpu_count; i++) {
                    if (gpus[i].memory_valid && gpus[i].memory_total) {
                        average_vram += 100.0 * (double)gpus[i].memory_used / (double)gpus[i].memory_total;
                        vram_valid_count++;
                    }
                    if (gpus[i].utilization_valid) {
                        average_gpu += gpus[i].utilization;
                        gpu_valid_count++;
                    }
                }
                if (vram_valid_count) average_vram /= vram_valid_count;
                if (gpu_valid_count) average_gpu /= gpu_valid_count;
                double timestamp = monotonic_seconds();
                if (vram_valid_count) history_add(&vram, timestamp, average_vram);
                if (gpu_valid_count) history_add(&gpu_util, timestamp, average_gpu);
                history_add(&ram, timestamp, system.ram_percent);
                history_add(&swap, timestamp, system.swap_percent);
                if (disk.valid)
                    history_add(&disk_history, timestamp, disk.total_bps);
                if (network.valid)
                    history_add(&network_history, timestamp, network.total_bps);
                append_remote_snapshots(gpus, local_gpu_count, &gpu_count, &ui);
                build_monitor_views(&system, &disk, &network, &ram, &swap, &disk_history,
                                    &network_history, &vram, &gpu_util, local_gpu_count);
                sample_duration = monotonic_seconds() - sample_started;
                debug_log("combined snapshot done duration=%.3fs gpus=%d monitors=%d",
                          sample_duration, gpu_count, remote_monitor_count + 1);
            }
            next_sample = monotonic_seconds() + interval;
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
                          &disk_history, &network_history, &disk, &network, &ui,
                          driver, cuda_library, cuda_runtime, cuda_driver,
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
        struct pollfd descriptors[2] = {
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = web_listener, .events = POLLIN},
        };
        nfds_t descriptor_count = web_listener >= 0 ? 2 : 1;
        int poll_result = poll(descriptors, descriptor_count, (int)(wait_seconds * 1000.0));
        if (poll_result < 0 && errno != EINTR)
            debug_log("poll failed: %s", strerror(errno));
        if (poll_result > 0 && descriptor_count == 2 &&
            (descriptors[1].revents & POLLIN)) {
            int monitor_count_before = remote_monitor_count;
            web_serve_pending(web_listener, gpus, local_gpu_count, &system, &disk, &network,
                              &ram, &swap, &disk_history, &network_history,
                              &vram, &gpu_util, &ui);
            if (remote_monitor_count != monitor_count_before) {
                compose_tui_snapshots(gpus, local_gpu_count, &gpu_count);
                build_monitor_views(&system, &disk, &network, &ram, &swap, &disk_history,
                                    &network_history, &vram, &gpu_util, local_gpu_count);
            }
            needs_render = true;
        }
        if (poll_result > 0) {
            debug_log("poll event revents=0x%x", descriptors[0].revents);
            if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                debug_log("terminal input closed or invalid");
                quit = true;
            }
        }
        if (poll_result > 0 && (descriptors[0].revents & POLLIN)) {
            ssize_t read_count = read(STDIN_FILENO, input.data + input.length,
                                      sizeof(input.data) - input.length);
            if (read_count > 0) {
                debug_log("terminal input bytes=%zd", read_count);
                input.length += (size_t)read_count;
                parse_input(&input, &ui, &paused, &help_visible, &interval,
                            &vram, &gpu_util, &ram, &swap, &disk_history,
                            &network_history, gpus, cols, gpu_count, &quit,
                            &mouse_button_down);
                compose_tui_snapshots(gpus, local_gpu_count, &gpu_count);
                build_monitor_views(&system, &disk, &network, &ram, &swap, &disk_history,
                                    &network_history, &vram, &gpu_util, local_gpu_count);
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
    while (remote_monitor_count > 0)
        remove_remote_monitor(remote_monitors[remote_monitor_count - 1].id);
    screen_free(&screen);
    if (web_listener >= 0) close(web_listener);
    history_free(&vram);
    history_free(&gpu_util);
    history_free(&ram);
    history_free(&swap);
    history_free(&disk_history);
    history_free(&network_history);
    restore_terminal();
    shutdown_gpu_backends();
    debug_log("shutdown complete");
    return 0;
}
