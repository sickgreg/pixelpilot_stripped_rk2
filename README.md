# pixelpilot_stripped_rk

pixelpilot_stripped_rk keeps the essentials required to display an RTP/H.265 stream on a Rockchip DRM plane:

- DRM/KMS initialisation and plane management.
- A custom UDP receiver feeding RTP packets into a small GStreamer pipeline.
- A dedicated appsink that hands Annex-B access units to the Rockchip MPP video decoder.
- Optional MP4 recording via the bundled `minimp4` writer.

All other subsystems (OSD, SSE streaming, splash player, udev hotplug management, etc.) have been removed.

## Command-line reference

```
--card PATH                 DRM card path (default: /dev/dri/card0)
--connector NAME            Connector name (e.g. HDMI-A-1); auto-picks the first connected head when omitted
--plane-id N                Video plane ID (default: 76)
--config PATH               Load settings from an INI file
--udp-port N                UDP listen port (default: 5600)
--vid-pt N                  RTP payload type for the video stream (default: 97)
--appsink-max-buffers N     Queue depth before the appsink drops old buffers (default: 4)
--record-video [PATH]       Enable MP4 recording; optional output path or directory (defaults to /media)
--record-mode MODE          MP4 writer mode: standard | sequential | fragmented
--no-record-video           Disable MP4 recording
--gst-log                   Export GST_DEBUG=3 when the environment variable is unset
--verbose                   Enable verbose logging
--help                      Show the usage summary
```

### Recording

`--record-video` enables the minimp4 writer. Passing a directory records into a timestamped filename; supplying a concrete file
path appends the timestamp to the basename. The writer mode can be tuned with `--record-mode`:

- `standard` — seekable MP4 (default).
- `sequential` — append-only output that avoids seeks.
- `fragmented` — fragmented MP4 suitable for live delivery.

Use `--no-record-video` to disable recording even when the INI file requests it.

## INI configuration

Settings can be stored in an INI file and loaded with `--config`. CLI options always win when both sources define the same key.
The parser understands two sections:

```
[video]
card_path = /dev/dri/card0
connector = HDMI-A-1
plane_id = 76
udp_port = 5600
vid_pt = 97
appsink_max_buffers = 4
gst_log = false

[record]
enable = false
output_path = /media
mode = sequential
```

The repository ships a commented template at `config/sample.ini`.

## Build

```
make
```

The build expects libdrm, GStreamer (core + app library), GLib, pthreads, and Rockchip MPP to be available. Use `ENABLE_NEON=0`
when targeting CPUs without NEON support.

## Runtime overview

1. `atomic_modeset_maxhz` selects the highest refresh mode for the requested connector and commits the target plane.
2. The UDP helper listens for RTP/H.265 packets on the configured port and payload type, pushing them into an `appsrc`.
3. A small GStreamer pipeline (`appsrc → queue → rtph265depay → h265parse → appsink`) forwards access units to the appsink.
4. The appsink thread feeds the Rockchip MPP decoder and, when enabled, the minimp4 writer.

Press `Ctrl+C` to shut the process down cleanly; send `SIGHUP` if you need to restart the video pipeline without exiting.

### Runtime signals

- `SIGUSR1` — enable MP4 recording (if already active, the request is ignored).
- `SIGUSR2` — disable MP4 recording.
- `SIGINT` — graceful shutdown (default behaviour for `Ctrl+C`).
- `SIGHUP` — restart the playback pipeline without exiting.
- `SIGTERM` — graceful shutdown of the process.
