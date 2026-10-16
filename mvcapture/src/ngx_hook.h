#pragma once

#include <windows.h>

namespace mvcapture {

// Thread entry: waits for the NVIDIA NGX core library (_nvngx.dll or
// nvngx.dll) to be loaded by the game, then hooks its
// NVSDK_NGX_D3D11_EvaluateFeature. Everything it finds is logged (see log.h),
// including the library's matching exports, so a build that cannot hook says
// why.
DWORD WINAPI WatchForNgx(LPVOID);

} // namespace mvcapture
