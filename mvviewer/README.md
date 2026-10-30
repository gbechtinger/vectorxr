#

A small Python script that shows the **motion vectors and depth buffer** that DCS World
hands to DLSS, live, while the game is running. It is a debugging aid for the
[`mvcapture`](../mvcapture) DLL and the game-motion-vector frame generation path.

## How it works

1. Opens the shared memory block `Local\VectorXR.MvCapture.<pid>` published by the
   `mvcapture` `dxgi.dll` (read-only).
2. Reads each capture slot with the sequence lock, so it never shows a half-written frame.
3. Duplicates the shared texture handle from inside the DCS process and opens the texture
   with `ID3D11Device1::OpenSharedResource1`.
4. Copies it to a staging texture, decodes it and shows it with OpenCV.
   - Motion vectors: hue = direction, brightness = magnitude.
   - Depth: shown as a grayscale/colormap image.

## Requirements

- Windows, with DCS World running and the `mvcapture` `dxgi.dll` installed in DCS `bin`.
- A mission loaded (the capture block only exists after the first DLSS evaluation).
- Python 3 with:

```
pip install numpy opencv-python
```

If DCS runs as administrator, run this script as administrator too.

## Usage

```
python view.py
```

| Option     | Meaning                                                        |
|------------|----------------------------------------------------------------|
| `pid`      | DCS process id (optional if it can be detected)                |
| `--max`    | Motion magnitude (pixels) that saturates the brightness        |
| `--zoom`   | Display scale factor                                           |
| `--latest` | Start on the most recent ring entry                            |

## Keys

| Key       | Action                                              |
|-----------|-----------------------------------------------------|
| `d`       | Toggle motion vectors / depth                       |
| `+` / `-` | Change the magnitude that saturates brightness (MV) |
| `i`       | Invert the colormap (depth)                         |
| `l`       | Switch between the latest and previous ring entry   |
| `q`/`Esc` | Quit                                                |