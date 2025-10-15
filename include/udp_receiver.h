#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include <glib.h>
#include <gst/app/gstappsrc.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UdpReceiver UdpReceiver;

UdpReceiver *udp_receiver_create(int udp_port, int vid_pt, GstAppSrc *video_appsrc);
int udp_receiver_start(UdpReceiver *ur);
void udp_receiver_stop(UdpReceiver *ur);
void udp_receiver_destroy(UdpReceiver *ur);

#ifdef __cplusplus
}
#endif

#endif // UDP_RECEIVER_H
