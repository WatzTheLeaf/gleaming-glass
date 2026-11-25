# Gleaming Glass

Plays a video as your Windows desktop wallpaper, behind the desktop icons.

- One video on every monitor, scaled to fill the screen (cropped, no black bars)
- Loops without end, no audio
- Hardware decoding with Media Foundation, rendering with Direct3D 11
- Pauses when a full-screen app, a game or a presentation is running, or when the session is locked
- Recovers by itself after display changes or an Explorer restart
- Single executable, no installer, no settings file

## Requirements

- Windows 11 24H2 or later
- A video format that Media Foundation can decode (for example MP4 with H.264)

## Usage

```
gleaming-glass.exe <video>
```

Start the tool again with a different video to replace the current one.

To stop it:

```
gleaming-glass.exe --stop
```

## Build

Requires CMake 3.20+ and MinGW-w64 (UCRT).
The output is a static `build/gleaming-glass.exe`.
