// SPDX-License-Identifier: MIT

#define _GNU_SOURCE

#include "udp_receiver.h"
#include "logging.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#include <pthread.h>
#include <sched.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#define UDP_MAX_PACKET    (4 * 1024)
#define UDP_RCVBUF_BYTES  (8 * 1024 * 1024)
#define APPSRC_LEVEL_MAX  (8 * 1024 * 1024)   /* drop incoming if appsrc level above this */

struct UdpReceiver {
    int udp_port;
    int vid_pt;
    GstAppSrc *video_appsrc;

    int sockfd;
    GThread *thread;
    GMutex lock;
    gboolean running;
    gboolean stop_requested;
    GstBufferPool *pool;
    gboolean pool_active;
};

static void boost_thread_priority_rx(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 12; /* slightly higher than appsink */
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) != 0) {
        /* Fallback best-effort */
        nice(-12);
    }
}

static gboolean ensure_buffer_pool(UdpReceiver *ur) {
    if (ur == NULL) return FALSE;

    if (ur->pool != NULL) {
        if (!ur->pool_active) {
            if (!gst_buffer_pool_set_active(ur->pool, TRUE)) {
                LOGW("UDP receiver: failed to activate buffer pool");
                return FALSE;
            }
            ur->pool_active = TRUE;
        }
        return TRUE;
    }

    GstBufferPool *pool = gst_buffer_pool_new();
    if (pool == NULL) {
        LOGW("UDP receiver: failed to create buffer pool");
        return FALSE;
    }

    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, NULL, UDP_MAX_PACKET, 8, 32);
    if (!gst_buffer_pool_set_config(pool, config)) {
        LOGW("UDP receiver: failed to configure buffer pool");
        gst_object_unref(pool);
        return FALSE;
    }
    if (!gst_buffer_pool_set_active(pool, TRUE)) {
        LOGW("UDP receiver: failed to activate buffer pool");
        gst_object_unref(pool);
        return FALSE;
    }

    ur->pool = pool;
    ur->pool_active = TRUE;
    return TRUE;
}

static gboolean payload_type_matches(const guint8 *data, gssize len, int expected_pt) {
    if (expected_pt < 0) return TRUE;
    if (len < 2)       return FALSE;
    guint8 payload_type = data[1] & 0x7Fu;
    return payload_type == (guint8)expected_pt;
}

static gpointer receiver_thread(gpointer data) {
    UdpReceiver *ur = (UdpReceiver *)data;

    /* Boost priority for packet ingest */
    boost_thread_priority_rx();

    guint8 *buffer = g_malloc(UDP_MAX_PACKET);
    if (buffer == NULL) {
        LOGE("UDP receiver: failed to allocate packet buffer");
        return NULL;
    }

    while (TRUE) {
        g_mutex_lock(&ur->lock);
        gboolean stop = ur->stop_requested;
        g_mutex_unlock(&ur->lock);
        if (stop) break;

        ssize_t n = recv(ur->sockfd, buffer, UDP_MAX_PACKET, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                g_usleep(1000); /* 1 ms to avoid tight spin */
                continue;
            }
            LOGW("UDP receiver: recv failed: %s", g_strerror(errno));
            continue;
        }
        if (n == 0) continue;
        if (!payload_type_matches(buffer, n, ur->vid_pt)) continue;

        /* Manual upstream leak: if appsrc is backed up, drop this packet */
        guint64 level = gst_app_src_get_current_level_bytes(ur->video_appsrc);
        if (level > APPSRC_LEVEL_MAX) {
            /* Optional: LOGV to avoid spam */
            // LOGV("UDP receiver: dropping packet (appsrc level=%" G_GUINT64_FORMAT ")", level);
            continue;
        }

        GstBuffer *gst_buf = NULL;
        if (ensure_buffer_pool(ur)) {
            GstFlowReturn acquire_ret = gst_buffer_pool_acquire_buffer(ur->pool, &gst_buf, NULL);
            if (acquire_ret != GST_FLOW_OK) {
                LOGW("UDP receiver: buffer pool acquisition failed: %s", gst_flow_get_name(acquire_ret));
                gst_buf = NULL;
            }
        }
        if (gst_buf == NULL) {
            gst_buf = gst_buffer_new_allocate(NULL, (gsize)n, NULL);
            if (gst_buf == NULL) {
                LOGW("UDP receiver: dropping packet (allocation failed)");
                continue;
            }
        }

        GstMapInfo map;
        if (gst_buffer_map(gst_buf, &map, GST_MAP_WRITE)) {
            gsize copy_size = (gsize)n;
            if (map.size < copy_size) {
                LOGW("UDP receiver: dropping packet (buffer too small: %" G_GSIZE_FORMAT " < %" G_GSIZE_FORMAT ")",
                     map.size, copy_size);
                gst_buffer_unmap(gst_buf, &map);
                gst_buffer_unref(gst_buf);
                continue;
            }
            memcpy(map.data, buffer, (size_t)copy_size);
            gst_buffer_unmap(gst_buf, &map);
            gst_buffer_set_size(gst_buf, copy_size);
        } else {
            LOGW("UDP receiver: failed to map GstBuffer");
            gst_buffer_unref(gst_buf);
            continue;
        }

        GstFlowReturn flow = gst_app_src_push_buffer(ur->video_appsrc, gst_buf);
        if (flow != GST_FLOW_OK) {
            LOGV("UDP receiver: appsrc push returned %s", gst_flow_get_name(flow));
        }
    }

    g_free(buffer);
    return NULL;
}

UdpReceiver *udp_receiver_create(int udp_port, int vid_pt, GstAppSrc *video_appsrc) {
    if (video_appsrc == NULL) return NULL;

    UdpReceiver *ur = g_new0(UdpReceiver, 1);
    if (ur == NULL) return NULL;

    ur->udp_port = udp_port;
    ur->vid_pt = vid_pt;
    ur->video_appsrc = GST_APP_SRC(gst_object_ref(video_appsrc));
    ur->sockfd = -1;
    g_mutex_init(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    ur->thread = NULL;
    ur->pool = NULL;
    ur->pool_active = FALSE;

    return ur;
}

int udp_receiver_start(UdpReceiver *ur) {
    if (ur == NULL) return -1;

    g_mutex_lock(&ur->lock);
    if (ur->running) {
        g_mutex_unlock(&ur->lock);
        return 0;
    }
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE("UDP receiver: socket failed: %s", g_strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_REUSEADDR) failed: %s", g_strerror(errno));
    }

    int rcvbuf = UDP_RCVBUF_BYTES;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        LOGW("UDP receiver: setsockopt(SO_RCVBUF) failed: %s", g_strerror(errno));
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            LOGW("UDP receiver: fcntl(O_NONBLOCK) failed: %s", g_strerror(errno));
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ur->udp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("UDP receiver: bind(%d) failed: %s", ur->udp_port, g_strerror(errno));
        close(fd);
        return -1;
    }

    g_mutex_lock(&ur->lock);
    ur->sockfd = fd;
    ur->running = TRUE;
    g_mutex_unlock(&ur->lock);

    ur->thread = g_thread_new("udp-receiver", receiver_thread, ur);
    if (ur->thread == NULL) {
        LOGE("UDP receiver: failed to create thread");
        g_mutex_lock(&ur->lock);
        ur->running = FALSE;
        g_mutex_unlock(&ur->lock);
        close(fd);
        ur->sockfd = -1;
        return -1;
    }
    return 0;
}

void udp_receiver_stop(UdpReceiver *ur) {
    if (ur == NULL) return;

    g_mutex_lock(&ur->lock);
    if (!ur->running) {
        g_mutex_unlock(&ur->lock);
        return;
    }
    ur->stop_requested = TRUE;
    g_mutex_unlock(&ur->lock);

    if (ur->sockfd >= 0) {
        shutdown(ur->sockfd, SHUT_RDWR);
    }

    if (ur->thread != NULL) {
        g_thread_join(ur->thread);
        ur->thread = NULL;
    }

    if (ur->sockfd >= 0) {
        close(ur->sockfd);
        ur->sockfd = -1;
    }

    g_mutex_lock(&ur->lock);
    ur->running = FALSE;
    ur->stop_requested = FALSE;
    g_mutex_unlock(&ur->lock);
}

void udp_receiver_destroy(UdpReceiver *ur) {
    if (ur == NULL) return;
    udp_receiver_stop(ur);
    if (ur->video_appsrc != NULL) {
        gst_object_unref(ur->video_appsrc);
        ur->video_appsrc = NULL;
    }
    if (ur->pool != NULL) {
        if (ur->pool_active) {
            gst_buffer_pool_set_active(ur->pool, FALSE);
            ur->pool_active = FALSE;
        }
        gst_object_unref(ur->pool);
        ur->pool = NULL;
    }
    g_mutex_clear(&ur->lock);
    g_free(ur);
}
