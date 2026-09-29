# displayPost

Windows x64 proof of concept for the X1830 framebuffer display project.

## What it does

1. Prompts for the X1830 HTTP base URL.
2. GETs `/fbinfo`.
3. Installs/loads the bundled Microsoft IddSample-based virtual display driver.
4. Creates the software device with `SwDeviceCreate`.
5. Waits for the virtual monitor to appear at the framebuffer resolution.
6. Captures that monitor with GDI, converts 32-bit desktop pixels to RGB565, and continuously POSTs raw fixed-size frames to `/fb`.

The application deliberately has no WebSocket/framing protocol. `/fb` is a continuous byte stream; each frame is exactly `frame_size` bytes reported by `/fbinfo`.

## Current PoC limits

- Windows client is x64.
- X1830 framebuffer must report RGB565.
- The bundled sample driver advertises 800x480 as its preferred mode (plus a few fallback modes). The first target is the common 800x480 X1830 panel.
- GDI capture + CPU RGB565 conversion is intentionally simple; it is not the final high-performance path.
- The Microsoft sample driver is built by GitHub Actions. Driver signing/install policy still applies on the target Windows machine. An unsigned/test-signed driver may require Windows test-signing mode for development.

## Build

Push to `main` or run the `Build displayPost` workflow manually. The workflow uses the current Microsoft Windows-driver-samples IddSampleDriver and the `windows-2025-vs2026` GitHub runner, then uploads `displayPost-x64` containing `displayPost.exe` and the driver package.

## Run

Place the artifact as:

```text
displayPost.exe
driver/
  IddSampleDriver.inf
  IddSampleDriver.dll
  IddSampleDriver.cat
```

Run `displayPost.exe`, enter for example:

```text
http://192.168.1.100:8080
```

The program requests elevation because adding a driver package requires administrator privileges. `pnputil` is used to add/install the INF, and the software device is then created by the application.

## Architecture

```text
Windows DWM
    |
    v
IddSampleDriver -> virtual monitor
    ^
    | SwDeviceCreate
    |
displayPost.exe
    |
    +-- GET /fbinfo
    +-- GDI capture virtual monitor
    +-- BGRA/XRGB -> RGB565
    +-- POST /fb (continuous fixed-size frames)
    |
    v
X1830 /dev/fb0
```

The driver portion is based on Microsoft's Indirect Display Driver sample. See the Microsoft Windows Driver Samples repository for the upstream sample and licensing information.
