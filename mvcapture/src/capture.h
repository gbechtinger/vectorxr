#pragma once

struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;

namespace mvcapture {

// Called from inside the hooked NVSDK_NGX_D3D11_EvaluateFeature, on the
// game's thread and before the real call, with the evaluation's parameters.
// Copies the motion vectors and depth DLSS is about to read into shared
// textures and publishes them (shared.h). Never throws and never changes
// what the game or DLSS see, apart from the commands it adds to `context`
// (copies, and one compute dispatch when the depth must be converted); the
// context's compute stage bindings are restored.
void OnEvaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Parameter* parameters);

// VECTORXR_MVCAPTURE=0 turns capture off (the DLL still forwards DXGI).
bool Enabled();

} // namespace mvcapture
