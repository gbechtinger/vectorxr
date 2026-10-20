# VectorXR motion vector capture (experimental)

A `dxgi.dll` for a game's folder that captures the motion vectors and depth
the game hands to DLSS (Direct3D 11) and publishes them, inside the game
process, for the VectorXR layer's frame generation.

It forwards every DXGI export to the system's `dxgi.dll`. From a thread of its
own it waits for the NGX core library (`_nvngx.dll` or `nvngx.dll`, which the
game loads when it starts DLSS), hooks `NVSDK_NGX_D3D11_EvaluateFeature` with
MinHook, and on each evaluation copies the `MotionVectors` and `Depth`
textures into shared textures (three per stream, so a reader never meets the
writer) together with the jitter, the motion vector scale and the render
subrect. Depth-stencil depth is converted to `R32_FLOAT` with a small compute
pass, because a depth-stencil texture cannot be shared.

## Build

Windows, Visual Studio, CMake 3.28+. NVIDIA's NGX headers are under the
NVIDIA RTX SDKs licence and are not part of this repository: clone
<https://github.com/NVIDIA/DLSS> somewhere and point CMake at it.

```
cmake -S . -B build-mvcapture -DDEPTHXR_BUILD_LAYER=OFF -DDEPTHXR_BUILD_TESTS=OFF ^
      -DDEPTHXR_BUILD_MVCAPTURE=ON -DVECTORXR_NGX_SDK_DIR=C:\src\DLSS
cmake --build build-mvcapture --config Release --target vectorxr_mvcapture
```

The output is `dxgi.dll`. The forwarders point at
`C:\Windows\System32\dxgi.dll`.

## Use

Copy `dxgi.dll` next to the game's executable (for DCS, the `bin` folder it
runs from) and start the game with DLSS on. There can be only one `dxgi.dll`
per folder, so remove any other proxy first (or rename this one to another
system DLL name the game loads and add its forwarders to `src/dllmain.cpp`).

* Log: `%LOCALAPPDATA%\VectorXR\mvcapture.log`. It lists the NGX library's
  exports that mention Evaluate or D3D11, whether the hook was installed, and
  the first evaluations' parameters (resource sizes and formats, subrects,
  jitter, scale).
* `VECTORXR_MVCAPTURE=0` turns capture off; the DLL then only forwards DXGI.

## Reading the data

`include/mvcapture/shared.h` describes the named file mapping
`Local\VectorXR.MvCapture.<pid>` and has a small `Reader`. Slots are numbered
in the order the streams (usually eyes) were first seen. Texture and fence
handles are NT handles valid in the same process: open them with
`ID3D12Device::OpenSharedHandle`, and wait for `fence_value` on the shared
fence before reading `ring_index`'s textures.

## Limits

* Direct3D 11 DLSS only, and only when the game evaluates DLSS through the
  NGX core library's exported function. If the log says the function is not
  exported, nothing is hooked.
* Converted depth is the raw depth buffer (it may be reversed-Z and
  non-linear). DLSS is not given the projection's near and far planes, so the
  reader needs them from elsewhere.
* Whether a game or server accepts a replaced `dxgi.dll` is up to the game.

## Using the vectors in VectorXR frame generation (stage 1)

With `dxgi.dll` from this project next to the game's executable, set
`VECTORXR_FRAMEGEN_GAME_MOTION=1` before starting the game. Frame generation
then builds its flow field from the game's DLSS motion vectors (one slot per
eye, in the order the game's DLSS evaluations first run) instead of the
optical flow hardware/shader search; each cell is still verified against the
luma pair and falls back to zero motion where the vector does not match
(canopy glass, particles, sky). Only the Direct3D 12 backend uses them; others
measure the motion as before. If the eyes look swapped, add
`VECTORXR_FRAMEGEN_GAME_MOTION_SWAP=1`. Depth is not used yet.

## Seeing the vectors (debug preview)

The shared block only holds metadata and GPU handles, so an external program
cannot read the pixels from it. For debugging, set
`VECTORXR_MVCAPTURE_PREVIEW=1` in the game's environment: the DLL then copies
every 8th evaluation's vectors (two eyes) to a second mapping,
`Local\VectorXR.MvCapture.Preview.<pid>` (layout in `shared.h`, `PreviewHeader`),
without waiting for the GPU. `tools/mvcapture_viewer.py` shows them (colour =
direction, brightness = length):

```
pip install numpy opencv-python psutil
python mvviewer/view.py
```

Leave the variable unset for normal play: the layer does not use the preview.

## Generating frames only while DLSS runs (optional, off by default)

`VECTORXR_FRAMEGEN_GAME_MOTION_ONLY=1` (in the environment of the VR app and the game) makes Frame Generation generate
frames only while the game's DLSS evaluations arrive. After 1 s without one (menus, the map, loading screens, anything
without a DLSS session) the game's frames are shown as they are, with no interpolation and no optical flow, at full
quality. Frame Generation stays engaged meanwhile (so the game is still paced at half the refresh rate and nothing
switches on or off), and generation resumes once about twenty new evaluations have come in. The log says when it
switches. Without the capture DLL nothing is generated with this option on.

## Capturing DLSS's output (optional, off by default)

`VECTORXR_MVCAPTURE_OUTPUT=1` in the game's environment makes the DLL also copy the image DLSS writes (its `Output`
parameter) right after each evaluation returns, before the game draws anything over it (HUD, displays, menus, a
helmet-mounted display). Compared with the image the game finally submits, it shows where those things are.

It lives in its own mapping, `Local\VectorXR.MvCapture.Output.<pid>` (`OutputBlock` in `mvcapture/shared.h`, read with
`OutputReader`), so the main block and everything that reads it stay as they are. Slots are the main block's slots, and
the textures are ordered by the same shared fence. It costs a copy of the full output image per evaluation and video
memory for three of them per eye, which is why it is opt-in. Nothing in the layer reads it yet.
