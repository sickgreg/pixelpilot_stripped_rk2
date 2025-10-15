// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include "config.h"
#include "logging.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void cli_copy_string(char *dst, size_t dst_sz, const char *src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, dst_sz - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --card PATH                 DRM card path (default: /dev/dri/card0)\n"
            "  --connector NAME            Connector name, e.g. HDMI-A-1 (default: auto)\n"
            "  --plane-id N                Video plane ID (default: 76)\n"
            "  --config PATH               Load configuration from ini file\n"
            "  --udp-port N                UDP listen port (default: 5600)\n"
            "  --vid-pt N                  RTP payload type for video (default: 97)\n"
            "  --appsink-max-buffers N     Max buffers queued on the appsink (default: 4)\n"
            "  --record-video [PATH]       Enable MP4 recording (optional output path)\n"
            "  --record-mode MODE          MP4 recording mode (standard|sequential|fragmented)\n"
            "  --no-record-video           Disable MP4 recording\n"
            "  --gst-log                   Export GST_DEBUG=3 when not already set\n"
            "  --verbose                   Enable verbose logging\n"
            "  --help                      Show this help text\n",
            prog);
}

void cfg_defaults(AppCfg *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->card_path, "/dev/dri/card0");
    cfg->connector_name[0] = '\0';
    cfg->config_path[0] = '\0';
    cfg->plane_id = 76;
    cfg->udp_port = 5600;
    cfg->vid_pt = 97;
    cfg->appsink_max_buffers = 4;
    cfg->gst_log = 0;

    cfg->record.enable = 0;
    strcpy(cfg->record.output_path, "/media");
    cfg->record.mode = RECORD_MODE_SEQUENTIAL;
}

static int parse_int_arg(const char *opt, const char *value, int *out) {
    if (value == NULL || out == NULL) {
        LOGE("Option %s requires an integer argument", opt);
        return -1;
    }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        LOGE("Invalid integer for %s: %s", opt, value);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int maybe_load_config(AppCfg *cfg) {
    if (cfg == NULL || cfg->config_path[0] == '\0') {
        return 0;
    }
    if (cfg_load_file(cfg->config_path, cfg) != 0) {
        LOGE("Failed to load config file: %s", cfg->config_path);
        return -1;
    }
    return 0;
}

static void maybe_enable_gst_log(const AppCfg *cfg) {
    if (cfg == NULL) {
        return;
    }
    if (cfg->gst_log && getenv("GST_DEBUG") == NULL) {
        setenv("GST_DEBUG", "3", 1);
    }
}

int parse_cli(int argc, char **argv, AppCfg *cfg) {
    if (cfg == NULL) {
        return -1;
    }

    cfg_defaults(cfg);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                LOGE("--config requires a path");
                return -1;
            }
            cli_copy_string(cfg->config_path, sizeof(cfg->config_path), argv[++i]);
            if (maybe_load_config(cfg) != 0) {
                return -1;
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--config") == 0) {
            ++i;
            continue;
        } else if (strcmp(arg, "--card") == 0) {
            if (i + 1 >= argc) {
                LOGE("--card requires a path");
                return -1;
            }
            cli_copy_string(cfg->card_path, sizeof(cfg->card_path), argv[++i]);
        } else if (strcmp(arg, "--connector") == 0) {
            if (i + 1 >= argc) {
                LOGE("--connector requires a value");
                return -1;
            }
            cli_copy_string(cfg->connector_name, sizeof(cfg->connector_name), argv[++i]);
        } else if (strcmp(arg, "--plane-id") == 0) {
            if (i + 1 >= argc || parse_int_arg("--plane-id", argv[i + 1], &cfg->plane_id) != 0) {
                return -1;
            }
            ++i;
        } else if (strcmp(arg, "--udp-port") == 0) {
            if (i + 1 >= argc || parse_int_arg("--udp-port", argv[i + 1], &cfg->udp_port) != 0) {
                return -1;
            }
            ++i;
        } else if (strcmp(arg, "--vid-pt") == 0) {
            if (i + 1 >= argc || parse_int_arg("--vid-pt", argv[i + 1], &cfg->vid_pt) != 0) {
                return -1;
            }
            ++i;
        } else if (strcmp(arg, "--appsink-max-buffers") == 0) {
            if (i + 1 >= argc || parse_int_arg("--appsink-max-buffers", argv[i + 1], &cfg->appsink_max_buffers) != 0) {
                return -1;
            }
            ++i;
        } else if (strcmp(arg, "--record-video") == 0) {
            cfg->record.enable = 1;
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) {
                cli_copy_string(cfg->record.output_path, sizeof(cfg->record.output_path), argv[++i]);
            }
        } else if (strcmp(arg, "--record-mode") == 0) {
            if (i + 1 >= argc) {
                LOGE("--record-mode requires a value");
                return -1;
            }
            RecordMode mode = cfg->record.mode;
            if (cfg_parse_record_mode(argv[i + 1], &mode) != 0) {
                LOGE("Unknown record mode: %s", argv[i + 1]);
                return -1;
            }
            cfg->record.mode = mode;
            ++i;
        } else if (strcmp(arg, "--no-record-video") == 0) {
            cfg->record.enable = 0;
        } else if (strcmp(arg, "--gst-log") == 0) {
            cfg->gst_log = 1;
        } else if (strcmp(arg, "--verbose") == 0) {
            log_set_verbose(1);
        } else {
            LOGE("Unknown option: %s", arg);
            usage(argv[0]);
            return -1;
        }
    }

    maybe_enable_gst_log(cfg);
    return 0;
}

typedef struct {
    const char *name;
    RecordMode mode;
} RecordModeAlias;

static const RecordModeAlias kRecordModeAliases[] = {
    {"standard", RECORD_MODE_STANDARD},
    {"default", RECORD_MODE_STANDARD},
    {"sequential", RECORD_MODE_SEQUENTIAL},
    {"append", RECORD_MODE_SEQUENTIAL},
    {"fragmented", RECORD_MODE_FRAGMENTED},
    {"fragment", RECORD_MODE_FRAGMENTED},
};

int cfg_parse_record_mode(const char *value, RecordMode *mode_out) {
    if (value == NULL || mode_out == NULL) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(kRecordModeAliases) / sizeof(kRecordModeAliases[0]); ++i) {
        if (strcasecmp(value, kRecordModeAliases[i].name) == 0) {
            *mode_out = kRecordModeAliases[i].mode;
            return 0;
        }
    }
    return -1;
}

const char *cfg_record_mode_name(RecordMode mode) {
    switch (mode) {
    case RECORD_MODE_STANDARD:
        return "standard";
    case RECORD_MODE_SEQUENTIAL:
        return "sequential";
    case RECORD_MODE_FRAGMENTED:
        return "fragmented";
    default:
        return "unknown";
    }
}
