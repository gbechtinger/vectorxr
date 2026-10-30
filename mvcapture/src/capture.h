#pragma once

struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace mvcapture {

// Called from inside the hooked NVSDK_NGX_D3D11_EvaluateFeature, on the
// game's thread and before the real call, with the evaluation's parameters.
// Copies the motion vectors and depth DLSS is about to read into shared
// textures and publishes them (shared.h). Never throws and never changes
// what the game or DLSS see, apart from the commands it adds to `context`
// (copies, and one compute dispatch when the depth must be converted); the
// context's compute stage bindings are restored.
//
// `feature` is the DLSS feature handle of the evaluation (the InFeatureHandle
// the game passed to NVSDK_NGX_D3D11_EvaluateFeature). It stays the same for
// the lifetime of a DLSS instance, so it identifies the evaluation stream even
// when the game alternates its motion vector textures from frame to frame.
// It is only compared, never dereferenced.
void OnEvaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* parameters);

// VECTORXR_MVCAPTURE=0 turns capture off (the DLL still forwards DXGI).
bool Enabled();

} // namespace mvcapture
