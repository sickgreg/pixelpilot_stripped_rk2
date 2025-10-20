#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>

typedef enum {
    RECORD_MODE_STANDARD = 0,
    RECORD_MODE_SEQUENTIAL,
    RECORD_MODE_FRAGMENTED,
} RecordMode;

typedef struct {
    int enable;
    char output_path[PATH_MAX];
    RecordMode mode;
} RecordCfg;

typedef struct {
    char card_path[64];
    char connector_name[32];
    char config_path[PATH_MAX];
    int plane_id;

    int udp_port;
    int vid_pt;
    int  jitter_buffer_ms;
    int appsink_max_buffers;
    int gst_log;

    RecordCfg record;
} AppCfg;

int parse_cli(int argc, char **argv, AppCfg *cfg);
void cfg_defaults(AppCfg *cfg);
int cfg_load_file(const char *path, AppCfg *cfg);
int cfg_parse_record_mode(const char *value, RecordMode *mode_out);
const char *cfg_record_mode_name(RecordMode mode);

#endif // CONFIG_H
