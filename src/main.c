// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include "config.h"
#include "drm_modeset.h"
#include "logging.h"
#include "pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit_flag = 0;
static volatile sig_atomic_t g_restart_flag = 0;
static volatile sig_atomic_t g_start_record_flag = 0;
static volatile sig_atomic_t g_stop_record_flag = 0;

static pthread_t g_signal_thread;
static sigset_t g_signal_mask;

typedef struct {
    PipelineState *ps;
    int wait_ms;
} PipelineStopContext;

static void *pipeline_stop_worker(void *arg) {
    PipelineStopContext *ctx = (PipelineStopContext *)arg;
    if (ctx != NULL && ctx->ps != NULL) {
        pipeline_stop(ctx->ps, ctx->wait_ms);
    }
    return NULL;
}

static const char *g_instance_pid_path = "/tmp/pixelpilot_mini_rk.pid";

static void remove_instance_pid(void) {
    if (unlink(g_instance_pid_path) != 0 && errno != ENOENT) {
        LOGW("Failed to remove %s: %s", g_instance_pid_path, strerror(errno));
    }
}

static pid_t read_existing_pid(void) {
    int fd = open(g_instance_pid_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    char buf[64];
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0) {
        return -1;
    }

    buf[len] = '\0';
    char *end = NULL;
    errno = 0;
    long parsed = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || parsed <= 0 || parsed > INT_MAX) {
        return -1;
    }
    return (pid_t)parsed;
}

static int write_pid_file(void) {
    int fd = open(g_instance_pid_path, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return 1;
        }
        LOGE("Failed to create %s: %s", g_instance_pid_path, strerror(errno));
        return -1;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        LOGE("PID buffer overflow while writing %s", g_instance_pid_path);
        close(fd);
        unlink(g_instance_pid_path);
        return -1;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    if (written != (ssize_t)len) {
        LOGE("Failed to write PID file %s: %s", g_instance_pid_path, strerror(errno));
        close(fd);
        unlink(g_instance_pid_path);
        return -1;
    }

    close(fd);
    atexit(remove_instance_pid);
    return 0;
}

static int ensure_single_instance(void) {
    for (;;) {
        int rc = write_pid_file();
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }

        pid_t existing_pid = read_existing_pid();
        if (existing_pid > 0) {
            errno = 0;
            if (kill(existing_pid, 0) == 0 || errno == EPERM) {
                LOGE("An existing instance of pixelpilot_mini_rk is already running ... exiting ...");
                return -1;
            }
        }

        if (unlink(g_instance_pid_path) != 0 && errno != ENOENT) {
            LOGE("Failed to clear stale pid file %s: %s", g_instance_pid_path, strerror(errno));
            return -1;
        }
    }
}

static void *signal_thread_func(void *arg) {
    (void)arg;
    for (;;) {
        siginfo_t info;
        int sig = sigwaitinfo(&g_signal_mask, &info);
        if (sig < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGW("Signal watcher: sigwaitinfo failed: %s", strerror(errno));
            continue;
        }

        switch (sig) {
        case SIGINT:
            LOGI("SIGINT received; shutting down");
            g_exit_flag = 1;
            break;
        case SIGTERM:
            LOGI("SIGTERM received; shutting down");
            g_exit_flag = 1;
            break;
        case SIGHUP:
            LOGI("SIGHUP received; scheduling pipeline restart");
            g_restart_flag = 1;
            break;
        case SIGUSR1:
            LOGI("SIGUSR1 received; enabling recording");
            g_start_record_flag++;
            break;
        case SIGUSR2:
            LOGI("SIGUSR2 received; disabling recording");
            g_stop_record_flag++;
            break;
        default:
            LOGW("Signal watcher: unhandled signal %d", sig);
            break;
        }

        if (sig == SIGTERM && g_exit_flag) {
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    AppCfg cfg;
    int parse_rc = parse_cli(argc, argv, &cfg);
    if (parse_rc != 0) {
        return parse_rc > 0 ? 0 : 2;
    }

    if (ensure_single_instance() < 0) {
        return 1;
    }

    sigemptyset(&g_signal_mask);
    sigaddset(&g_signal_mask, SIGINT);
    sigaddset(&g_signal_mask, SIGTERM);
    sigaddset(&g_signal_mask, SIGUSR1);
    sigaddset(&g_signal_mask, SIGUSR2);
    sigaddset(&g_signal_mask, SIGHUP);

    if (pthread_sigmask(SIG_BLOCK, &g_signal_mask, NULL) != 0) {
        LOGE("pthread_sigmask failed: %s", strerror(errno));
        return 1;
    }

    if (pthread_create(&g_signal_thread, NULL, signal_thread_func, NULL) != 0) {
        LOGE("Failed to create signal watcher thread: %s", strerror(errno));
        return 1;
    }

    int fd = open(cfg.card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open %s: %s", cfg.card_path, strerror(errno));
        g_exit_flag = 1;
        pthread_kill(g_signal_thread, SIGTERM);
        pthread_join(g_signal_thread, NULL);
        return 1;
    }

    ModesetResult ms = {0};
    if (atomic_modeset_maxhz(fd, &cfg, &ms) != 0) {
        LOGE("Failed to configure display output");
        g_exit_flag = 1;
        pthread_kill(g_signal_thread, SIGTERM);
        pthread_join(g_signal_thread, NULL);
        close(fd);
        return 1;
    }

    PipelineState ps = {0};
    ps.state = PIPELINE_STOPPED;

    if (pipeline_start(&cfg, &ms, fd, &ps) != 0) {
        LOGE("Pipeline start failed");
        g_exit_flag = 1;
        pthread_kill(g_signal_thread, SIGTERM);
        pthread_join(g_signal_thread, NULL);
        close(fd);
        return 1;
    }

    if (cfg.record.enable) {
        if (pipeline_enable_recording(&ps, &cfg.record) != 0) {
            LOGW("Failed to start MP4 recorder; continuing without recording");
        }
    }

    for (;;) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = 0};
        poll(&pfd, 1, 200);

        if (g_exit_flag) {
            LOGI("Exit requested; preparing to stop pipeline");
            break;
        }

        if (g_start_record_flag > 0) {
            g_start_record_flag = 0;
            if (!cfg.record.enable) {
                cfg.record.enable = 1;
                LOGI("SIGUSR1: enabling MP4 recording");
            } else {
                LOGI("SIGUSR1: recording already enabled");
            }
            if (ps.state == PIPELINE_RUNNING) {
                if (pipeline_enable_recording(&ps, &cfg.record) != 0) {
                    LOGW("Failed to enable recording on running pipeline");
                }
            }
        }

        if (g_stop_record_flag > 0) {
            g_stop_record_flag = 0;
            if (cfg.record.enable) {
                LOGI("SIGUSR2: disabling MP4 recording");
                cfg.record.enable = 0;
            } else {
                LOGI("SIGUSR2: recording already disabled");
            }
            if (ps.state == PIPELINE_RUNNING) {
                pipeline_disable_recording(&ps);
            }
        }

        if (g_restart_flag) {
            g_restart_flag = 0;
            LOGI("Restarting pipeline");
            pipeline_stop(&ps, 700);
            if (pipeline_start(&cfg, &ms, fd, &ps) != 0) {
                LOGE("Pipeline restart failed");
                g_exit_flag = 1;
            } else if (cfg.record.enable) {
                if (pipeline_enable_recording(&ps, &cfg.record) != 0) {
                    LOGW("Failed to re-enable recording after restart");
                }
            }
        }

        pipeline_poll_child(&ps);
        if (ps.state == PIPELINE_STOPPED) {
            LOGI("Pipeline stopped; exiting main loop");
            g_exit_flag = 1;
            break;
        }
    }

    LOGI("Stopping pipeline");
    PipelineStopContext stop_ctx = {.ps = &ps, .wait_ms = 700};
    pthread_t stop_thread;
    if (pthread_create(&stop_thread, NULL, pipeline_stop_worker, &stop_ctx) != 0) {
        LOGE("Failed to spawn pipeline stop worker: %s", strerror(errno));
        pipeline_stop(&ps, 700);
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;
        int join_rc = pthread_timedjoin_np(stop_thread, NULL, &deadline);
        if (join_rc != 0) {
            LOGE("Pipeline stop timed out (%s); forcing process exit", strerror(join_rc));
            _exit(128);
        }
    }
    LOGI("Pipeline stopped");

    g_exit_flag = 1;
    pthread_kill(g_signal_thread, SIGTERM);
    pthread_join(g_signal_thread, NULL);

    close(fd);
    LOGI("Bye.");
    return 0;
}
