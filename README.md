# RecPilot

RecPilot is a native OBS Studio plugin for camera-recording workflows. It watches a camera overlay inside the video image, controls OBS recording, names clips from OCR, generates ZoeLog-compatible CSV metadata, and helps capture clapperboard snapshots.

## Current Features

- REC trigger from a selected tally/detection center in the video image.
- One-click selection for the detection center, clip name box, and metadata OCR boxes.
- Auto-detection for the detection center, testing red, green, then the selected custom color.
- Clip name OCR with optional cleanup rules.
- Automatic recording folders by date, camera, and card.
- ZoeLog CSV per card for Silverstack/Pomfort import.
- Metadata OCR fields with visible green dashed detection boxes in the Metadata page.
- Presets for detection center, clip name, and metadata fields.
- Clapperboard snapshots, preview crop, loupe, pan, and 90 degree rotation.
- Multilingual dock UI, including the Minion easter egg.
- Ko-fi support dialog and reminder logic.

## Metadata Strategy

The production metadata path is ZoeLog CSV per card.

Direct metadata embedding into MOV/MP4 files was intentionally removed. Generic FFmpeg/QuickTime tags and macOS extended attributes were not reliable enough for Resolve or Silverstack workflows. Future in-file metadata support should be treated as a separate R&D track around real QuickTime/camera metadata atoms.

## OBS Setup

1. Add the camera capture source in OBS.
2. Add the `RecPilot` filter to that source.
3. Open the RecPilot dock.
4. Configure Detection Center, Clip Name, automatic folders, and Metadata.
5. Arm RecPilot or enable arm-on-launch.

## Local Build

Use the macOS preset from the plugin directory:

```bash
cmake --preset macos
cmake --build --preset macos
```

The project is based on the OBS plugin template, but this repository is now the RecPilot product codebase. The original template remote is kept as `upstream`; the RecPilot GitHub repository is `origin`.

## Known Limits

- The plugin changes OBS recording settings for folder, codec, filename format, and output resolution. Those changes are now blocked while OBS is actively recording, streaming, or replay-buffering.
- ZoeLog import quality depends on Silverstack matching options and on clip naming consistency.
- Clapperboard detection is heuristic and should remain manually adjustable through crop pan, zoom/full view, and rotation.
- The source is still concentrated in two large files; the next refactor should split recording control, ZoeLog export, OCR, clapperboard detection, and dock pages into smaller modules.
