# VectorXR motion vector capture: a dxgi.dll proxy for the game's folder that
# hooks DLSS (NVSDK_NGX_D3D11_EvaluateFeature) and shares the motion vectors and
# depth DLSS reads. See README.md.

if(NOT WIN32)
    message(FATAL_ERROR "VectorXR motion vector capture only builds for Windows")
endif()

# NVIDIA's NGX headers are under the NVIDIA RTX SDKs licence, which does not
# allow redistributing them from this repository: point this at a local clone
# of https://github.com/NVIDIA/DLSS (its include/ folder is used).
set(VECTORXR_NGX_SDK_DIR "" CACHE PATH "Local clone of https://github.com/NVIDIA/DLSS (for include/nvsdk_ngx.h)")
if(NOT EXISTS "${VECTORXR_NGX_SDK_DIR}/include/nvsdk_ngx.h")
    message(FATAL_ERROR
        "DEPTHXR_BUILD_MVCAPTURE needs VECTORXR_NGX_SDK_DIR set to a clone of https://github.com/NVIDIA/DLSS "
        "(include/nvsdk_ngx.h was not found under '${VECTORXR_NGX_SDK_DIR}')")
endif()

include(FetchContent)
# MinHook v1.3.4 (BSD-2-Clause), pinned by commit.
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)

add_library(vectorxr_mvcapture SHARED
    src/dllmain.cpp
    src/ngx_hook.cpp
    src/capture.cpp
    src/log.cpp)
target_compile_features(vectorxr_mvcapture PRIVATE cxx_std_20)
target_include_directories(vectorxr_mvcapture PRIVATE
    src
    include
    "${VECTORXR_NGX_SDK_DIR}/include")
target_link_libraries(vectorxr_mvcapture PRIVATE minhook shell32 user32)

# The game loads this file as its own dxgi.dll. Its import library must not
# take the name of the system's dxgi.lib.
set_target_properties(vectorxr_mvcapture PROPERTIES
    PREFIX ""
    OUTPUT_NAME dxgi
    ARCHIVE_OUTPUT_NAME vectorxr_mvcapture_implib)

if(MSVC)
    # No Visual C++ redistributable needed inside someone else's process.
    set_property(TARGET vectorxr_mvcapture minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(vectorxr_mvcapture PRIVATE /W4 /permissive-)
endif()
