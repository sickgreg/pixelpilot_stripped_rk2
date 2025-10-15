// SPDX-License-Identifier: MIT

#include "config.h"
#include "logging.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_INI_LINE 512

static void trim_whitespace(char **begin, char **end) {
    while (*begin <= *end && isspace((unsigned char)**begin)) {
        (*begin)++;
    }
    while (*end >= *begin && isspace((unsigned char)**end)) {
        **end = '\0';
        (*end)--;
    }
}

static int parse_int(const char *key, const char *value, int *out) {
    if (value == NULL || out == NULL) {
        return -1;
    }
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        LOGW("config: invalid integer for %s: %s", key, value);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int parse_bool(const char *key, const char *value, int *out) {
    if (value == NULL || out == NULL) {
        return -1;
    }
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0) {
        *out = 0;
        return 0;
    }
    LOGW("config: invalid boolean for %s: %s", key, value);
    return -1;
}

static void copy_string(char *dst, size_t dst_sz, const char *value) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (value == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(value, dst_sz - 1);
    memcpy(dst, value, len);
    dst[len] = '\0';
}

static int handle_global_key(const char *key, const char *value, AppCfg *cfg) {
    if (strcasecmp(key, "card_path") == 0) {
        copy_string(cfg->card_path, sizeof(cfg->card_path), value);
        return 0;
    }
    if (strcasecmp(key, "connector") == 0 || strcasecmp(key, "connector_name") == 0) {
        copy_string(cfg->connector_name, sizeof(cfg->connector_name), value);
        return 0;
    }
    if (strcasecmp(key, "plane_id") == 0) {
        return parse_int("plane_id", value, &cfg->plane_id);
    }
    if (strcasecmp(key, "udp_port") == 0) {
        return parse_int("udp_port", value, &cfg->udp_port);
    }
    if (strcasecmp(key, "vid_pt") == 0 || strcasecmp(key, "video_payload_type") == 0) {
        return parse_int("vid_pt", value, &cfg->vid_pt);
    }
    if (strcasecmp(key, "appsink_max_buffers") == 0) {
        return parse_int("appsink_max_buffers", value, &cfg->appsink_max_buffers);
    }
    if (strcasecmp(key, "gst_log") == 0) {
        return parse_bool("gst_log", value, &cfg->gst_log);
    }
    if (strncasecmp(key, "record.", 7) == 0) {
        const char *sub = key + 7;
        if (strcasecmp(sub, "enable") == 0) {
            return parse_bool("record.enable", value, &cfg->record.enable);
        }
        if (strcasecmp(sub, "output_path") == 0 || strcasecmp(sub, "path") == 0) {
            copy_string(cfg->record.output_path, sizeof(cfg->record.output_path), value);
            return 0;
        }
        if (strcasecmp(sub, "mode") == 0) {
            RecordMode mode = cfg->record.mode;
            if (cfg_parse_record_mode(value, &mode) == 0) {
                cfg->record.mode = mode;
                return 0;
            }
            LOGW("config: invalid record.mode value: %s", value);
            return -1;
        }
    }
    return -1;
}

static int handle_section_key(const char *section, const char *key, const char *value, AppCfg *cfg) {
    if (section == NULL || cfg == NULL) {
        return -1;
    }
    if (strcasecmp(section, "video") == 0) {
        return handle_global_key(key, value, cfg);
    }
    if (strcasecmp(section, "record") == 0) {
        if (strcasecmp(key, "enable") == 0) {
            return parse_bool("record.enable", value, &cfg->record.enable);
        }
        if (strcasecmp(key, "output_path") == 0 || strcasecmp(key, "path") == 0) {
            copy_string(cfg->record.output_path, sizeof(cfg->record.output_path), value);
            return 0;
        }
        if (strcasecmp(key, "mode") == 0) {
            RecordMode mode = cfg->record.mode;
            if (cfg_parse_record_mode(value, &mode) == 0) {
                cfg->record.mode = mode;
                return 0;
            }
            LOGW("config: invalid record.mode value: %s", value);
            return -1;
        }
        return -1;
    }
    return handle_global_key(key, value, cfg);
}

int cfg_load_file(const char *path, AppCfg *cfg) {
    if (path == NULL || cfg == NULL) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        LOGE("config: failed to open %s: %s", path, strerror(errno));
        return -1;
    }

    char section[32] = {0};
    char line[MAX_INI_LINE];
    int line_no = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        ++line_no;
        char *begin = line;
        while (isspace((unsigned char)*begin)) {
            begin++;
        }
        if (*begin == '\0' || *begin == '#' || *begin == ';') {
            continue;
        }

        char *comment = strpbrk(begin, "#;");
        if (comment != NULL) {
            *comment = '\0';
        }

        char *end = begin + strlen(begin);
        if (end != begin) {
            --end;
            trim_whitespace(&begin, &end);
        }

        if (*begin == '\0') {
            continue;
        }

        if (*begin == '[') {
            char *close = strchr(begin, ']');
            if (close == NULL) {
                LOGW("config: line %d: missing ']'", line_no);
                continue;
            }
            *close = '\0';
            const char *name = begin + 1;
            copy_string(section, sizeof(section), name);
            continue;
        }

        char *equals = strchr(begin, '=');
        if (equals == NULL) {
            LOGW("config: line %d: missing '='", line_no);
            continue;
        }

        *equals = '\0';
        char *key = begin;
        char *value = equals + 1;
        char *key_end = key + strlen(key);
        if (key_end != key) {
            --key_end;
            trim_whitespace(&key, &key_end);
        }
        char *value_end = value + strlen(value);
        if (value_end != value) {
            --value_end;
            trim_whitespace(&value, &value_end);
        }

        if (*key == '\0') {
            continue;
        }

        if (section[0] != '\0') {
            handle_section_key(section, key, value, cfg);
        } else {
            handle_global_key(key, value, cfg);
        }
    }

    fclose(fp);
    return 0;
}
