# displayPost

Windows x64 low-latency framebuffer streamer for the X1830 project.

## Usage

```text
displayPost.exe <url> [frames=0] [monitor=1]
```

- `url`: X1830 HTTP base URL, for example `http://192.168.1.154`
- `frames`: number of frames to send; `0` means continuous
- `monitor`: 1-based Windows display number; default is `1`
- capture rate: **30 FPS**

Example:

```text
displayPost.exe http://192.168.1.154 300 1
```

The client first requests `/fbinfo`, then enumerates the real Windows monitors. It captures the selected monitor directly with GDI, scales it proportionally to the `/fbinfo` resolution with black letterboxing/pillarboxing, converts to the framebuffer's declared 16/32-bit bitfield format, and sends fixed-size raw frames continuously to `/fb`.

For the supplied X1830 framebuffer:

```text
480x320
xrgb8888
stride=1920
frame_size=614400
height_virtual=640
memory_size=1228800
```

only the visible 480x320 buffer is sent: `1920 * 320 = 614400` bytes per frame. The virtual/back buffer is not transmitted.

## Low-latency design

There is no WebSocket, multipart JPEG, ffmpeg process, JPEG encoding, or per-frame HTTP request. One HTTP/1.1 POST is opened and raw fixed-size frames are written into that stream. `Expect:` is disabled and WinHTTP is configured to bypass the system proxy.

The first implementation uses a reusable 32-bit DIB and `StretchBlt`; when the target is XRGB8888 with a matching stride, the final copy is a direct row copy. RGB565 and other common 16/32-bit bitfields are converted directly into the output buffer.

## Build

GitHub Actions builds `displayPost.exe` with Visual Studio/MSBuild and uploads the executable as the `displayPost-x64` artifact.

This direct-capture version intentionally does not install or require an IDD driver. The IDD work can be added later when the virtual-monitor source path is needed.
