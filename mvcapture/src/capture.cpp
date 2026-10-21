#include "capture.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "log.h"
#include "mvcapture/shared.h"

using Microsoft::WRL::ComPtr;

namespace mvcapture {
namespace {

// Reads a depth-stencil texture's depth plane into an R32_FLOAT texture, which
// can be shared (a depth-stencil texture cannot).
constexpr char kDepthCopyShader[] = R"hlsl(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    src.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) {
        return;
    }
    dst[id.xy] = src.Load(int3(id.xy, 0));
}
)hlsl";

DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

struct Slot {
    bool used{false};
    std::uint64_t last_use{0};
    // Identity of the evaluation stream: its position among the DLSS evaluations of one frame (0 = first eye
    // evaluated, 1 = second...). The game recreates its DLSS features (new handles) now and then, so neither the
    // feature handle nor the motion vector texture can identify an eye for long.
    std::uint32_t key_ordinal{0};
    UINT key_base_x{0};
    UINT key_base_y{0};

    ComPtr<ID3D11Texture2D> mv[kRing];
    ComPtr<ID3D11Texture2D> depth[kRing];
    ComPtr<ID3D11UnorderedAccessView> depth_uav[kRing];
    HANDLE mv_handle[kRing]{};
    HANDLE depth_handle[kRing]{};
    D3D11_TEXTURE2D_DESC mv_desc{};
    D3D11_TEXTURE2D_DESC depth_desc{};
    // The last texture descriptions that could not be captured: not retried (or logged) every frame.
    D3D11_TEXTURE2D_DESC mv_failed{};
    D3D11_TEXTURE2D_DESC depth_failed{};
    bool depth_converted{false};

    // Cached view of the game's depth texture for the conversion.
    const void* depth_src_key{nullptr};
    ComPtr<ID3D11ShaderResourceView> depth_src_view;

    // CPU preview (VECTORXR_MVCAPTURE_PREVIEW): two staging copies read in turn.
    ComPtr<ID3D11Texture2D> preview_stage[2];
    bool preview_pending[2]{};
    std::uint32_t preview_next{0};
    std::uint64_t preview_tick{0};
    D3D11_TEXTURE2D_DESC preview_desc{};
    // The DLSS feature last seen on this slot (only compared, never dereferenced), to log recreations.
    const void* feature{nullptr};

    // The DLSS output (VECTORXR_MVCAPTURE_OUTPUT=1): what DLSS wrote, before the game draws anything over it.
    ComPtr<ID3D11Texture2D> out[kRing];
    HANDLE out_handle[kRing]{};
    D3D11_TEXTURE2D_DESC out_desc{};
    D3D11_TEXTURE2D_DESC out_failed{};
    std::uint32_t out_generation{0};
    // The late copy (see OutputSlot::post_handle): the game's output texture of the previous evaluation, kept alive here
    // until this slot's next evaluation begins, and the shared textures it is copied to.
    ComPtr<ID3D11Texture2D> prev_output;
    std::uint32_t prev_ring{0};
    std::uint64_t prev_frame_seq{0};
    ComPtr<ID3D11Texture2D> post[kRing];
    HANDLE post_handle[kRing]{};
    D3D11_TEXTURE2D_DESC post_desc{};
    D3D11_TEXTURE2D_DESC post_failed{};
    std::uint32_t post_generation{0};

    std::uint32_t ring{kRing - 1};
    std::uint64_t frame_seq{0};
    std::uint32_t generation{0}; // bumped whenever the shared motion vector textures are re-created
};

// A slot not evaluated for this many DLSS calls is dead (its stream ended) and is released.
constexpr std::uint64_t kStaleCalls = 240;

struct State {
    std::mutex mutex;
    bool failed{false};
    std::uint64_t calls{0};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Fence> fence;
    HANDLE fence_handle{};
    std::uint64_t fence_value{0};
    std::uint32_t generation_counter{0}; // never reused: a reader may cache by handle value, which Windows reuses
    ComPtr<ID3D11ComputeShader> depth_shader;
    bool depth_shader_failed{false};

    HANDLE mapping{};
    Block* block{};
    bool preview{false};
    bool preview_checked{false};
    HANDLE preview_mapping{};
    std::uint8_t* preview_view{};
    Slot slots[kMaxSlots];

    // The DLSS output mapping (VECTORXR_MVCAPTURE_OUTPUT=1), and the slot whose vectors OnEvaluate just published
    // (OnEvaluateDone copies that evaluation's output), -1 when there is none.
    HANDLE output_mapping{};
    OutputBlock* output_block{};
    int pending_slot{-1};

    // Features evaluated since the start of the current round (one round = one frame: every eye once).
    const void* round_features[kMaxSlots]{};
    std::uint32_t round_count{0};
};

State& S() {
    static State state;
    return state;
}

void CloseSlot(Slot& slot) {
    for (std::uint32_t i = 0; i < kRing; ++i) {
        if (slot.mv_handle[i]) {
            CloseHandle(slot.mv_handle[i]);
        }
        if (slot.depth_handle[i]) {
            CloseHandle(slot.depth_handle[i]);
        }
        if (slot.out_handle[i]) {
            CloseHandle(slot.out_handle[i]);
        }
        if (slot.post_handle[i]) {
            CloseHandle(slot.post_handle[i]);
        }
    }
    slot = Slot{};
}

bool EnsureBlock(State& s) {
    if (s.block) {
        return true;
    }
    wchar_t name[64];
    MappingName(GetCurrentProcessId(), name);
    s.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Block), name);
    if (!s.mapping) {
        Log("CreateFileMapping failed: %lu", GetLastError());
        return false;
    }
    s.block = static_cast<Block*>(MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
    if (!s.block) {
        Log("MapViewOfFile failed: %lu", GetLastError());
        return false;
    }
    std::memset(s.block, 0, sizeof(Block));
    s.block->magic = kMagic;
    s.block->version = kVersion;
    s.block->size = sizeof(Block);
    return true;
}

// VECTORXR_MVCAPTURE_OUTPUT=1: also capture what DLSS writes (see shared.h, OutputBlock). Read once.
bool OutputEnabled() {
    static const bool enabled = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(L"VECTORXR_MVCAPTURE_OUTPUT", value, 8);
        return length == 1 && value[0] == L'1';
    }();
    return enabled;
}

// The output mapping, created on first use.
bool EnsureOutputBlock(State& s) {
    if (s.output_block) {
        return true;
    }
    wchar_t name[64];
    OutputMappingName(GetCurrentProcessId(), name);
    s.output_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(OutputBlock), name);
    if (!s.output_mapping) {
        Log("output: CreateFileMapping failed: %lu", GetLastError());
        return false;
    }
    s.output_block = static_cast<OutputBlock*>(MapViewOfFile(s.output_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OutputBlock)));
    if (!s.output_block) {
        Log("output: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(s.output_mapping);
        s.output_mapping = nullptr;
        return false;
    }
    std::memset(s.output_block, 0, sizeof(OutputBlock));
    s.output_block->magic = kOutputMagic;
    s.output_block->version = kOutputVersion;
    s.output_block->size = sizeof(OutputBlock);
    Log("output: the DLSS output is copied for readers (VECTORXR_MVCAPTURE_OUTPUT=1)");
    return true;
}

// Readers see valid == 0 for output slot `index` (its stream ended or the device changed).
void UnpublishOutput(State& s, std::uint32_t index) {
    if (!s.output_block) {
        return;
    }
    OutputSlot& info = s.output_block->slots[index];
    InterlockedIncrement64(&info.sequence); // odd: being written
    MemoryBarrier();
    info.valid = 0;
    for (std::uint32_t i = 0; i < kRing; ++i) {
        info.handle[i] = 0;
        info.post_handle[i] = 0;
    }
    info.post_valid = 0;
    MemoryBarrier();
    InterlockedIncrement64(&info.sequence); // even: published
}

// (Re)starts capture on `device`: the fence, the adapter and empty slots.
bool InitDevice(State& s, ID3D11Device* device) {
    for (Slot& slot : s.slots) {
        CloseSlot(slot);
    }
    if (s.fence_handle) {
        CloseHandle(s.fence_handle);
        s.fence_handle = nullptr;
    }
    s.fence.Reset();
    s.depth_shader.Reset();
    s.depth_shader_failed = false;
    s.fence_value = 0;
    s.device = device;
    if (!EnsureBlock(s)) {
        return false;
    }
    s.block->slot_count = 0;
    s.block->fence_handle = 0;
    s.round_count = 0;
    s.pending_slot = -1;
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
        UnpublishOutput(s, i);
    }
    for (SlotInfo& info : s.block->slots) {
        info.valid = 0;
        for (std::uint32_t i = 0; i < kRing; ++i) {
            info.mv_handle[i] = 0;
            info.depth_handle[i] = 0;
        }
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        s.block->adapter_luid_low = adapter_desc.AdapterLuid.LowPart;
        s.block->adapter_luid_high = adapter_desc.AdapterLuid.HighPart;
    }

    ComPtr<ID3D11Device5> device5;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
        SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fence))) &&
        SUCCEEDED(s.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &s.fence_handle))) {
        s.block->fence_handle = reinterpret_cast<std::uint64_t>(s.fence_handle);
    } else {
        s.fence.Reset();
        Log("no shared fence (needs Windows 10 1703+ and a driver with D3D11 fences): readers get no GPU ordering");
    }
    Log("capture started on a new device (adapter LUID %08lx:%08lx)", static_cast<unsigned long>(s.block->adapter_luid_high),
        static_cast<unsigned long>(s.block->adapter_luid_low));
    return true;
}

bool CompileDepthShader(State& s) {
    if (s.depth_shader) {
        return true;
    }
    if (s.depth_shader_failed) {
        return false;
    }
    s.depth_shader_failed = true;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                       ID3DBlob**, ID3DBlob**);
    const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        Log("d3dcompiler_47.dll is not available: depth-stencil depth cannot be converted");
        return false;
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(kDepthCopyShader, sizeof(kDepthCopyShader) - 1, "mvcapture_depth", nullptr, nullptr, "main",
                               "cs_5_0", 0, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("depth shader did not compile: 0x%08lx %s", static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(s.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s.depth_shader))) {
        Log("CreateComputeShader failed");
        return false;
    }
    s.depth_shader_failed = false;
    return true;
}

bool SameDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.ArraySize == b.ArraySize &&
           a.MipLevels == b.MipLevels;
}

// Makes the ring of shared textures for `desc` (the game's texture's own
// description, so CopyResource applies), or for a converted depth.
bool MakeShared(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& wanted, UINT bind, ComPtr<ID3D11Texture2D>* textures,
                HANDLE* handles) {
    D3D11_TEXTURE2D_DESC desc = wanted;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (std::uint32_t i = 0; i < kRing; ++i) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &textures[i]))) {
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        if (FAILED(textures[i].As(&resource)) ||
            FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                                &handles[i]))) {
            return false;
        }
    }
    return true;
}

template <class T>
bool Read(const NVSDK_NGX_Parameter* parameters, const char* name, T* out) {
    return parameters->Get(name, out) == NVSDK_NGX_Result_Success;
}

struct Inputs {
    ID3D11Resource* mv{};
    ID3D11Resource* depth{};
    float jitter_x{0.0f};
    float jitter_y{0.0f};
    float mv_scale_x{1.0f};
    float mv_scale_y{1.0f};
    int reset{0};
    unsigned int render_w{0};
    unsigned int render_h{0};
    unsigned int mv_base_x{0};
    unsigned int mv_base_y{0};
    unsigned int depth_base_x{0};
    unsigned int depth_base_y{0};
};

Inputs ReadInputs(const NVSDK_NGX_Parameter* p) {
    Inputs in;
    Read(p, NVSDK_NGX_Parameter_MotionVectors, &in.mv);
    Read(p, NVSDK_NGX_Parameter_Depth, &in.depth);
    Read(p, NVSDK_NGX_Parameter_Jitter_Offset_X, &in.jitter_x);
    Read(p, NVSDK_NGX_Parameter_Jitter_Offset_Y, &in.jitter_y);
    Read(p, NVSDK_NGX_Parameter_MV_Scale_X, &in.mv_scale_x);
    Read(p, NVSDK_NGX_Parameter_MV_Scale_Y, &in.mv_scale_y);
    Read(p, NVSDK_NGX_Parameter_Reset, &in.reset);
    Read(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &in.render_w);
    Read(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &in.render_h);
    Read(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &in.mv_base_x);
    Read(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &in.mv_base_y);
    Read(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &in.depth_base_x);
    Read(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &in.depth_base_y);
    return in;
}

// Unpublishes slot `index` and frees its textures: readers see valid == 0.
void ReleaseSlot(State& s, std::uint32_t index) {
    SlotInfo& info = s.block->slots[index];
    InterlockedIncrement64(&info.sequence); // odd: being written
    MemoryBarrier();
    info.valid = 0;
    for (std::uint32_t i = 0; i < kRing; ++i) {
        info.mv_handle[i] = 0;
        info.depth_handle[i] = 0;
    }
    MemoryBarrier();
    InterlockedIncrement64(&info.sequence); // even: published
    UnpublishOutput(s, index);
    CloseSlot(s.slots[index]);
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
        if (s.slots[i].used) {
            count = i + 1;
        }
    }
    s.block->slot_count = count;
    Log("slot %u released (its stream was not evaluated for %llu calls)", index,
        static_cast<unsigned long long>(kStaleCalls));
}

Slot* FindSlot(State& s, std::uint32_t ordinal, const void* feature, UINT base_x, UINT base_y, std::uint32_t& index) {
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
        Slot& slot = s.slots[i];
        if (slot.used && slot.key_ordinal == ordinal && slot.key_base_x == base_x && slot.key_base_y == base_y) {
            index = i;
            return &slot;
        }
    }
    Slot* oldest = nullptr;
    std::uint32_t oldest_index = 0;
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
        Slot& slot = s.slots[i];
        if (!slot.used) {
            index = i;
            slot.used = true;
            slot.key_ordinal = ordinal;
            slot.key_base_x = base_x;
            slot.key_base_y = base_y;
            if (s.block->slot_count < i + 1) {
                s.block->slot_count = i + 1;
            }
            Log("new evaluation stream -> slot %u (evaluation %u of the frame, DLSS feature %p, subrect base %u,%u)", i,
                ordinal, feature, base_x, base_y);
            return &slot;
        }
        if (!oldest || slot.last_use < oldest->last_use) {
            oldest = &slot;
            oldest_index = i;
        }
    }
    // All slots are taken by other streams: reuse the one unused for longest.
    Log("slot %u reused for evaluation %u of the frame (DLSS feature %p)", oldest_index, ordinal, feature);
    UnpublishOutput(s, oldest_index);
    CloseSlot(*oldest);
    oldest->used = true;
    oldest->key_ordinal = ordinal;
    oldest->key_base_x = base_x;
    oldest->key_base_y = base_y;
    index = oldest_index;
    return oldest;
}

// Converts or copies the game's depth into slot.depth[ring]. False when the
// depth cannot be captured (logged once per stream).
bool CaptureDepth(State& s, ID3D11DeviceContext* context, Slot& slot, std::uint32_t ring, ID3D11Resource* depth) {
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(depth->QueryInterface(IID_PPV_ARGS(&source)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1 || SameDesc(desc, slot.depth_failed)) {
        return false;
    }
    const bool convert = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;

    if (!SameDesc(desc, slot.depth_desc) || slot.depth_converted != convert || !slot.depth[0]) {
        for (std::uint32_t i = 0; i < kRing; ++i) {
            if (slot.depth_handle[i]) {
                CloseHandle(slot.depth_handle[i]);
                slot.depth_handle[i] = nullptr;
            }
            slot.depth[i].Reset();
            slot.depth_uav[i].Reset();
        }
        bool made = false;
        if (convert) {
            if (desc.ArraySize > 1 || DepthSrvFormat(desc.Format) == DXGI_FORMAT_UNKNOWN ||
                !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !CompileDepthShader(s)) {
                Log("depth format %u (array %u, bind 0x%x) cannot be converted; depth is not captured", desc.Format,
                    desc.ArraySize, desc.BindFlags);
                slot.depth_failed = desc;
                return false;
            }
            D3D11_TEXTURE2D_DESC out = desc;
            out.Format = DXGI_FORMAT_R32_FLOAT;
            out.MipLevels = 1;
            out.ArraySize = 1;
            made = MakeShared(s.device.Get(), out, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, slot.depth,
                              slot.depth_handle);
            for (std::uint32_t i = 0; made && i < kRing; ++i) {
                made = SUCCEEDED(s.device->CreateUnorderedAccessView(slot.depth[i].Get(), nullptr, &slot.depth_uav[i]));
            }
            slot.depth_src_key = nullptr;
            slot.depth_src_view.Reset();
        } else {
            made = MakeShared(s.device.Get(), desc, D3D11_BIND_SHADER_RESOURCE, slot.depth, slot.depth_handle);
        }
        if (!made) {
            Log("could not create shared depth textures (format %u %ux%u)", desc.Format, desc.Width, desc.Height);
            for (std::uint32_t i = 0; i < kRing; ++i) {
                if (slot.depth_handle[i]) {
                    CloseHandle(slot.depth_handle[i]);
                    slot.depth_handle[i] = nullptr;
                }
                slot.depth[i].Reset();
                slot.depth_uav[i].Reset();
            }
            slot.depth_failed = desc;
            return false;
        }
        slot.depth_desc = desc;
        slot.depth_converted = convert;
        Log("shared depth: %ux%u format %u %s", desc.Width, desc.Height, desc.Format,
            convert ? "(converted from a depth-stencil texture to R32_FLOAT, raw buffer values)" : "(copied as written)");
    }

    if (!convert) {
        context->CopyResource(slot.depth[ring].Get(), source.Get());
        return true;
    }

    if (slot.depth_src_key != source.Get() || !slot.depth_src_view) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DepthSrvFormat(desc.Format);
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        slot.depth_src_view.Reset();
        if (FAILED(s.device->CreateShaderResourceView(source.Get(), &view, &slot.depth_src_view))) {
            Log("could not read the game's depth texture (format %u)", desc.Format);
            slot.depth_failed = desc;
            return false;
        }
        slot.depth_src_key = source.Get();
    }

    // Leave the game's compute stage as it was.
    ComPtr<ID3D11ComputeShader> old_shader;
    ComPtr<ID3D11ShaderResourceView> old_srv;
    ComPtr<ID3D11UnorderedAccessView> old_uav;
    context->CSGetShader(&old_shader, nullptr, nullptr);
    context->CSGetShaderResources(0, 1, &old_srv);
    context->CSGetUnorderedAccessViews(0, 1, &old_uav);

    ID3D11ShaderResourceView* srv = slot.depth_src_view.Get();
    ID3D11UnorderedAccessView* uav = slot.depth_uav[ring].Get();
    const UINT keep = 0xffffffffu;
    context->CSSetShader(s.depth_shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);

    ID3D11ShaderResourceView* old_srv_raw = old_srv.Get();
    ID3D11UnorderedAccessView* old_uav_raw = old_uav.Get();
    context->CSSetShader(old_shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &old_srv_raw);
    context->CSSetUnorderedAccessViews(0, 1, &old_uav_raw, &keep);
    return true;
}

bool CaptureMotion(State& s, ID3D11DeviceContext* context, Slot& slot, std::uint32_t ring, ID3D11Resource* mv) {
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(mv->QueryInterface(IID_PPV_ARGS(&source)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1 || SameDesc(desc, slot.mv_failed)) {
        return false;
    }
    if (!SameDesc(desc, slot.mv_desc) || !slot.mv[0]) {
        slot.generation = ++s.generation_counter;
        for (std::uint32_t i = 0; i < kRing; ++i) {
            if (slot.mv_handle[i]) {
                CloseHandle(slot.mv_handle[i]);
                slot.mv_handle[i] = nullptr;
            }
            slot.mv[i].Reset();
        }
        if (!MakeShared(s.device.Get(), desc, D3D11_BIND_SHADER_RESOURCE, slot.mv, slot.mv_handle)) {
            Log("could not create shared motion vector textures (format %u %ux%u)", desc.Format, desc.Width, desc.Height);
            for (std::uint32_t i = 0; i < kRing; ++i) {
                if (slot.mv_handle[i]) {
                    CloseHandle(slot.mv_handle[i]);
                    slot.mv_handle[i] = nullptr;
                }
                slot.mv[i].Reset();
            }
            slot.mv_failed = desc;
            return false;
        }
        slot.mv_desc = desc;
        Log("shared motion vectors: %ux%u format %u array %u", desc.Width, desc.Height, desc.Format, desc.ArraySize);
    }
    context->CopyResource(slot.mv[ring].Get(), source.Get());
    return true;
}

// Copies `source` into textures[ring], (re)creating the shared ring when its description changed. False when it cannot
// be captured (logged once per description).
bool CopyShared(State& s, ID3D11DeviceContext* context, ComPtr<ID3D11Texture2D>* textures, HANDLE* handles,
                D3D11_TEXTURE2D_DESC& current, D3D11_TEXTURE2D_DESC& failed, std::uint32_t& generation,
                std::uint32_t ring, ID3D11Resource* resource, const char* what) {
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&source)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1 || SameDesc(desc, failed)) {
        return false;
    }
    if (!SameDesc(desc, current) || !textures[0]) {
        generation = ++s.generation_counter; // new handles: readers must not reuse cached ones
        for (std::uint32_t i = 0; i < kRing; ++i) {
            if (handles[i]) {
                CloseHandle(handles[i]);
                handles[i] = nullptr;
            }
            textures[i].Reset();
        }
        if (!MakeShared(s.device.Get(), desc, D3D11_BIND_SHADER_RESOURCE, textures, handles)) {
            Log("%s: could not create shared textures (format %u %ux%u)", what, desc.Format, desc.Width, desc.Height);
            for (std::uint32_t i = 0; i < kRing; ++i) {
                if (handles[i]) {
                    CloseHandle(handles[i]);
                    handles[i] = nullptr;
                }
                textures[i].Reset();
            }
            failed = desc;
            return false;
        }
        current = desc;
        Log("%s: shared %ux%u format %u array %u", what, desc.Width, desc.Height, desc.Format, desc.ArraySize);
    }
    context->CopyResource(textures[ring].Get(), source.Get());
    return true;
}

// Copies what DLSS wrote into slot.out[ring].
bool CaptureOutput(State& s, ID3D11DeviceContext* context, Slot& slot, std::uint32_t ring, ID3D11Resource* output) {
    return CopyShared(s, context, slot.out, slot.out_handle, slot.out_desc, slot.out_failed, slot.out_generation, ring,
                      output, "output");
}

std::uint32_t BytesPerPixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return 16;
    default:
        return 0;
    }
}

// The preview mapping, created on first use when VECTORXR_MVCAPTURE_PREVIEW=1.
bool EnsurePreview(State& s) {
    if (s.preview_view) {
        return true;
    }
    if (s.preview_checked) {
        return false;
    }
    s.preview_checked = true;
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(L"VECTORXR_MVCAPTURE_PREVIEW", value, 8);
    if (!(length == 1 && value[0] == L'1')) {
        return false;
    }
    const std::uint64_t total = std::uint64_t(kPreviewHeaderBytes) + std::uint64_t(kPreviewSlots) * kPreviewSlotBytes;
    wchar_t name[64];
    PreviewMappingName(GetCurrentProcessId(), name);
    s.preview_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
                                           static_cast<DWORD>(total & 0xffffffffu), name);
    if (!s.preview_mapping) {
        Log("preview: CreateFileMapping failed: %lu", GetLastError());
        return false;
    }
    s.preview_view = static_cast<std::uint8_t*>(MapViewOfFile(s.preview_mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(total)));
    if (!s.preview_view) {
        Log("preview: MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(s.preview_mapping);
        s.preview_mapping = nullptr;
        return false;
    }
    auto* header = reinterpret_cast<PreviewHeader*>(s.preview_view);
    std::memset(header, 0, sizeof(PreviewHeader));
    header->magic = kPreviewMagic;
    header->version = 1;
    header->header_bytes = kPreviewHeaderBytes;
    header->slot_bytes = kPreviewSlotBytes;
    header->slot_count = kPreviewSlots;
    s.preview = true;
    Log("preview: motion vectors are copied to the CPU (every 8th evaluation) for debugging tools");
    return true;
}

// Copies the newest vectors to a staging texture, and publishes the copy made
// the time before (it is done by now, so reading it does not stall the game).
void PreviewMotion(State& s, ID3D11DeviceContext* context, Slot& slot, std::uint32_t index, std::uint32_t ring) {
    if (index >= kPreviewSlots || !EnsurePreview(s) || ++slot.preview_tick % 8 != 0) {
        return;
    }
    const D3D11_TEXTURE2D_DESC& desc = slot.mv_desc;
    const std::uint32_t bpp = BytesPerPixel(desc.Format);
    if (!bpp || desc.ArraySize != 1 || std::uint64_t(desc.Width) * desc.Height * bpp > kPreviewSlotBytes) {
        return;
    }
    if (!slot.preview_stage[0] || !SameDesc(desc, slot.preview_desc)) {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        D3D11_TEXTURE2D_DESC stage = desc;
        stage.Usage = D3D11_USAGE_STAGING;
        stage.BindFlags = 0;
        stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stage.MiscFlags = 0;
        for (int i = 0; i < 2; ++i) {
            slot.preview_stage[i].Reset();
            slot.preview_pending[i] = false;
            if (!device || FAILED(device->CreateTexture2D(&stage, nullptr, &slot.preview_stage[i]))) {
                Log("preview: could not create a staging texture (format %u %ux%u)", desc.Format, desc.Width, desc.Height);
                slot.preview_stage[0].Reset();
                slot.preview_stage[1].Reset();
                return;
            }
        }
        slot.preview_desc = desc;
        slot.preview_next = 0;
    }
    // The copy made last time, if the GPU has finished it.
    const std::uint32_t read = slot.preview_next ^ 1u;
    if (slot.preview_pending[read]) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(slot.preview_stage[read].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            auto* header = reinterpret_cast<PreviewHeader*>(s.preview_view);
            PreviewSlot& out = header->slots[index];
            std::uint8_t* dst = s.preview_view + kPreviewHeaderBytes + std::size_t(index) * kPreviewSlotBytes;
            InterlockedIncrement64(&out.sequence); // odd: being written
            MemoryBarrier();
            const std::size_t row = std::size_t(desc.Width) * bpp;
            for (UINT y = 0; y < desc.Height; ++y) {
                std::memcpy(dst + y * row, static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, row);
            }
            out.frame_seq = slot.frame_seq;
            out.width = desc.Width;
            out.height = desc.Height;
            out.format = desc.Format;
            out.bytes_per_pixel = bpp;
            MemoryBarrier();
            InterlockedIncrement64(&out.sequence); // even: published
            context->Unmap(slot.preview_stage[read].Get(), 0);
            slot.preview_pending[read] = false;
        }
    }
    context->CopyResource(slot.preview_stage[slot.preview_next].Get(), slot.mv[ring].Get());
    slot.preview_pending[slot.preview_next] = true;
    slot.preview_next ^= 1u;
}

} // namespace

bool Enabled() {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(L"VECTORXR_MVCAPTURE", value, 8);
    return !(length > 0 && length < 8 && value[0] == L'0');
}

// The texture DLSS wrote for this slot's previous evaluation, copied again now, before this evaluation overwrites it:
// it holds whatever the game drew over it for that frame (if it draws on that texture), and published in the output
// mapping next to the copy made when the evaluation returned.
void PublishLateOutput(State& s, ID3D11DeviceContext* context, Slot& slot, std::uint32_t index) {
    ID3D11Texture2D* previous = slot.prev_output.Get();
    const std::uint32_t ring = slot.prev_ring;
    if (!EnsureOutputBlock(s) ||
        !CopyShared(s, context, slot.post, slot.post_handle, slot.post_desc, slot.post_failed, slot.post_generation, ring,
                    previous, "output (late)")) {
        slot.prev_output.Reset();
        return;
    }
    std::uint64_t fence_value = 0;
    if (s.fence) {
        ComPtr<ID3D11DeviceContext4> context4;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            fence_value = ++s.fence_value;
            context4->Signal(s.fence.Get(), fence_value);
        }
    }
    OutputSlot& info = s.output_block->slots[index];
    InterlockedIncrement64(&info.sequence); // odd: being written
    MemoryBarrier();
    for (std::uint32_t i = 0; i < kRing; ++i) {
        info.post_handle[i] = reinterpret_cast<std::uint64_t>(slot.post_handle[i]);
    }
    info.post_frame_seq = slot.prev_frame_seq;
    info.post_fence_value = fence_value;
    info.post_ring = ring;
    info.post_generation = slot.post_generation;
    info.post_valid = 1;
    MemoryBarrier();
    InterlockedIncrement64(&info.sequence); // even: published
    slot.prev_output.Reset();
}

void OnEvaluate(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* parameters) {
    if (!context || !parameters) {
        return;
    }
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed) {
        return;
    }
    ++s.calls;
    s.pending_slot = -1;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        if (s.calls == 1) {
            Log("DLSS was evaluated on a deferred context; not captured");
        }
        return;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) {
        return;
    }
    if (device.Get() != s.device.Get() || !s.block) {
        if (!InitDevice(s, device.Get())) {
            s.failed = true;
            return;
        }
    }
    ++s.block->evaluate_calls;

    const Inputs in = ReadInputs(parameters);
    const bool verbose = s.calls <= 12 || s.calls % 3000 == 0;
    if (verbose) {
        Log("evaluate #%llu: feature=%p mv=%p depth=%p jitter=(%.3f,%.3f) mvScale=(%.3f,%.3f) reset=%d render=%ux%u "
            "mvBase=%u,%u depthBase=%u,%u",
            static_cast<unsigned long long>(s.calls), static_cast<const void*>(feature), static_cast<void*>(in.mv),
            static_cast<void*>(in.depth), in.jitter_x,
            in.jitter_y, in.mv_scale_x, in.mv_scale_y, in.reset, in.render_w, in.render_h, in.mv_base_x, in.mv_base_y,
            in.depth_base_x, in.depth_base_y);
    }
    if (!in.mv) {
        return;
    }

    // Release streams that ended (the game recreated its DLSS features and the old ones are gone).
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
        if (s.slots[i].used && s.calls - s.slots[i].last_use > kStaleCalls) {
            ReleaseSlot(s, i);
        }
    }

    // Which evaluation of the frame is this? A feature seen twice starts a new round, so the ordinal is 0 for the
    // first eye, 1 for the second, whatever feature handles and textures the game currently uses.
    bool repeated = s.round_count >= kMaxSlots;
    for (std::uint32_t i = 0; !repeated && i < s.round_count; ++i) {
        repeated = s.round_features[i] == static_cast<const void*>(feature);
    }
    if (repeated) {
        s.round_count = 0;
    }
    const std::uint32_t ordinal = s.round_count;
    s.round_features[s.round_count++] = feature;

    std::uint32_t index = 0;
    Slot* slot = FindSlot(s, ordinal, feature, in.mv_base_x, in.mv_base_y, index);
    if (slot->feature != static_cast<const void*>(feature)) {
        if (slot->feature) {
            Log("slot %u: DLSS feature changed %p -> %p (the game recreated it)", index, slot->feature,
                static_cast<const void*>(feature));
        }
        slot->feature = feature;
    }
    slot->last_use = s.calls;
    const std::uint32_t ring = (slot->ring + 1) % kRing;
    if (slot->prev_output) {
        PublishLateOutput(s, context, *slot, index);
    }

    const bool have_mv = CaptureMotion(s, context, *slot, ring, in.mv);
    const bool have_depth = in.depth && CaptureDepth(s, context, *slot, ring, in.depth);
    if (!have_mv) {
        return;
    }

    PreviewMotion(s, context, *slot, index, ring);

    std::uint64_t fence_value = 0;
    if (s.fence) {
        ComPtr<ID3D11DeviceContext4> context4;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            fence_value = ++s.fence_value;
            context4->Signal(s.fence.Get(), fence_value);
        }
    }

    SlotInfo& info = s.block->slots[index];
    InterlockedIncrement64(&info.sequence); // odd: being written
    MemoryBarrier();
    ++slot->frame_seq;
    slot->ring = ring;
    info.valid = 1;
    info.ring_index = ring;
    info.frame_seq = slot->frame_seq;
    info.fence_value = fence_value;
    for (std::uint32_t i = 0; i < kRing; ++i) {
        info.mv_handle[i] = reinterpret_cast<std::uint64_t>(slot->mv_handle[i]);
        info.depth_handle[i] = have_depth ? reinterpret_cast<std::uint64_t>(slot->depth_handle[i]) : 0;
    }
    info.width = slot->mv_desc.Width;
    info.height = slot->mv_desc.Height;
    info.mv_format = slot->mv_desc.Format;
    info.depth_format = have_depth ? (slot->depth_converted ? DXGI_FORMAT_R32_FLOAT : slot->depth_desc.Format) : 0;
    info.depth_converted = have_depth && slot->depth_converted ? 1 : 0;
    info.array_size = slot->mv_desc.ArraySize;
    info.render_width = in.render_w;
    info.render_height = in.render_h;
    info.mv_base_x = in.mv_base_x;
    info.mv_base_y = in.mv_base_y;
    info.depth_base_x = in.depth_base_x;
    info.depth_base_y = in.depth_base_y;
    info.jitter_x = in.jitter_x;
    info.jitter_y = in.jitter_y;
    info.mv_scale_x = in.mv_scale_x;
    info.mv_scale_y = in.mv_scale_y;
    info.reset = in.reset;
    info.generation = slot->generation;
    MemoryBarrier();
    InterlockedIncrement64(&info.sequence); // even: published
    s.pending_slot = static_cast<int>(index);
}

// Called after the real evaluation: the DLSS output is final now, before the game draws over it.
void OnEvaluateDone(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* parameters) {
    if (!context || !parameters || !OutputEnabled()) {
        return;
    }
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed || !s.block || s.pending_slot < 0) {
        return;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(s.pending_slot);
    s.pending_slot = -1;
    Slot& slot = s.slots[index];
    if (!slot.used || slot.feature != static_cast<const void*>(feature) || !EnsureOutputBlock(s)) {
        return;
    }
    ID3D11Resource* output = nullptr;
    if (!Read(parameters, NVSDK_NGX_Parameter_Output, &output) || !output) {
        return;
    }
    const std::uint32_t ring = slot.ring;
    if (!CaptureOutput(s, context, slot, ring, output)) {
        return;
    }
    // Kept until this slot's next evaluation begins, to be copied again then (PublishLateOutput).
    slot.prev_output.Reset();
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&slot.prev_output)))) {
        slot.prev_ring = ring;
        slot.prev_frame_seq = slot.frame_seq;
    }

    std::uint64_t fence_value = 0;
    if (s.fence) {
        ComPtr<ID3D11DeviceContext4> context4;
        if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            fence_value = ++s.fence_value;
            context4->Signal(s.fence.Get(), fence_value);
        }
    }

    OutputSlot& info = s.output_block->slots[index];
    InterlockedIncrement64(&info.sequence); // odd: being written
    MemoryBarrier();
    info.valid = 1;
    info.ring_index = ring;
    info.frame_seq = slot.frame_seq;
    info.fence_value = fence_value;
    for (std::uint32_t i = 0; i < kRing; ++i) {
        info.handle[i] = reinterpret_cast<std::uint64_t>(slot.out_handle[i]);
    }
    info.width = slot.out_desc.Width;
    info.height = slot.out_desc.Height;
    info.format = slot.out_desc.Format;
    info.array_size = slot.out_desc.ArraySize;
    info.generation = slot.out_generation;
    if (s.output_block->slot_count < index + 1) {
        s.output_block->slot_count = index + 1;
    }
    MemoryBarrier();
    InterlockedIncrement64(&info.sequence); // even: published
}

} // namespace mvcapture
