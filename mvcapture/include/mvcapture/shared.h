#pragma once

// Layout of the block the capture DLL (dxgi.dll proxy, see mvcapture/README.md)
// publishes inside the game process, and a small reader for the OpenXR layer.
//
// Both live in the same process, so the NT handle values stored here are valid
// as they are: the layer opens the textures with ID3D12Device::OpenSharedHandle
// and the fence with ID3D12Device::OpenSharedHandle (a shared ID3D11Fence).
//
// The block is a named file mapping, "Local\VectorXR.MvCapture.<pid>". Each
// slot is guarded by a seqlock: `sequence` is odd while the writer is
// updating it; a reader copies the slot and accepts the copy only when the
// sequence was even and unchanged.

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mvcapture {

inline constexpr std::uint32_t kMagic = 0x3143564du; // "MVC1"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxSlots = 4; // distinct DLSS evaluations per frame (one per eye, with spares)
inline constexpr std::uint32_t kRing = 3;     // texture sets per slot, so a reader never meets the writer

// One DLSS evaluation stream, usually one eye. Slots are numbered in the
// order their inputs were first seen, so with two eyes rendered in the same
// order every frame, slot 0 is the first eye evaluated.
struct SlotInfo {
    volatile LONG64 sequence;  // seqlock, odd = being written
    std::uint32_t valid;       // 1 once the slot has been published
    std::uint32_t ring_index;  // textures of the newest capture: mv_handle[ring_index], depth_handle[ring_index]
    std::uint64_t frame_seq;   // counts DLSS evaluations of this slot
    std::uint64_t fence_value; // the shared fence reaches this when the copies for `ring_index` are done
    std::uint64_t mv_handle[kRing];    // NT handles of shared textures (0 when not captured)
    std::uint64_t depth_handle[kRing]; // the game's depth as written, or R32_FLOAT when depth_converted (0 when not captured)
    std::uint32_t width;       // texture size, texels
    std::uint32_t height;
    std::uint32_t mv_format;           // DXGI_FORMAT of the shared motion vector texture
    std::uint32_t depth_format;        // DXGI_FORMAT of the shared depth texture
    std::uint32_t depth_converted;     // 1: the game's depth-stencil was converted to R32_FLOAT (raw depth buffer values)
    std::uint32_t array_size;          // slices in the shared textures
    std::uint32_t render_width;        // DLSS render subrect, texels (0 when the game did not set it)
    std::uint32_t render_height;
    std::uint32_t mv_base_x;           // where the render subrect starts in the motion vector texture
    std::uint32_t mv_base_y;
    std::uint32_t depth_base_x;
    std::uint32_t depth_base_y;
    float jitter_x;                    // DLSS "Jitter.Offset.X/Y", pixels
    float jitter_y;
    float mv_scale_x;                  // DLSS "MV.Scale.X/Y": multiply a vector by this to get render-resolution pixels
    float mv_scale_y;
    std::int32_t reset;                // DLSS "Reset": history was discarded this frame
    std::uint32_t generation;          // changes whenever the shared motion vector textures are re-created
};

struct Block {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;        // sizeof(Block)
    std::uint32_t slot_count;  // slots 0..slot_count-1 have been seen
    std::uint64_t fence_handle; // NT handle of a shared ID3D11Fence, signalled after each capture (0 when unavailable)
    std::uint32_t adapter_luid_low;
    std::int32_t adapter_luid_high;
    std::uint64_t evaluate_calls; // all DLSS evaluations seen, captured or not
    SlotInfo slots[kMaxSlots];
};

inline void MappingName(DWORD pid, wchar_t (&name)[64]) {
    wsprintfW(name, L"Local\\VectorXR.MvCapture.%lu", pid);
}

// Optional CPU preview of the motion vectors, for debugging tools (not for the
// OpenXR layer, which reads the shared textures on the GPU). Enabled with
// VECTORXR_MVCAPTURE_PREVIEW=1 in the game's environment. A second named
// mapping, "Local\\VectorXR.MvCapture.Preview.<pid>", holds a header and, per
// slot, a tightly packed copy of the newest motion vector texture (every few
// evaluations, a couple of frames late) at kPreviewHeaderBytes + slot * slot_bytes.
inline constexpr std::uint32_t kPreviewMagic = 0x3150564du; // "MVP1"
inline constexpr std::uint32_t kPreviewSlots = 2;
inline constexpr std::uint32_t kPreviewHeaderBytes = 256;
inline constexpr std::uint32_t kPreviewSlotBytes = 16u * 1024u * 1024u;

struct PreviewSlot {
    volatile LONG64 sequence;  // seqlock, odd = being written
    std::uint64_t frame_seq;   // SlotInfo::frame_seq of the evaluation the copy comes from (approximate)
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;      // DXGI_FORMAT
    std::uint32_t bytes_per_pixel;
    std::uint32_t reserved[4];
};

struct PreviewHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_bytes;
    std::uint32_t slot_bytes;
    std::uint32_t slot_count;
    std::uint32_t reserved[3];
    PreviewSlot slots[kPreviewSlots];
};
static_assert(sizeof(PreviewHeader) <= kPreviewHeaderBytes, "preview header must fit its reserved space");

inline void PreviewMappingName(DWORD pid, wchar_t (&name)[64]) {
    wsprintfW(name, L"Local\\VectorXR.MvCapture.Preview.%lu", pid);
}

// Reader side (the OpenXR layer). Header-only so the layer needs no new target.
class Reader {
  public:
    ~Reader() { Close(); }

    // False when the capture DLL is not loaded in this process.
    bool Open() {
        if (block_) {
            return true;
        }
        wchar_t name[64];
        MappingName(GetCurrentProcessId(), name);
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!mapping_) {
            return false;
        }
        block_ = static_cast<const Block*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(Block)));
        if (!block_ || block_->magic != kMagic || block_->version != kVersion || block_->size != sizeof(Block)) {
            Close();
            return false;
        }
        return true;
    }

    void Close() {
        if (block_) {
            UnmapViewOfFile(block_);
            block_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    const Block* Get() const { return block_; }

    // Copies slot `index` consistently; false when it is not published yet
    // or the writer kept changing it.
    bool ReadSlot(std::uint32_t index, SlotInfo& out) const {
        if (!block_ || index >= kMaxSlots) {
            return false;
        }
        const SlotInfo& slot = block_->slots[index];
        for (int attempt = 0; attempt < 8; ++attempt) {
            const LONG64 before = slot.sequence;
            MemoryBarrier();
            if (before & 1) {
                continue;
            }
            out = slot;
            MemoryBarrier();
            if (slot.sequence == before && out.valid) {
                out.sequence = before;
                return true;
            }
        }
        return false;
    }

  private:
    HANDLE mapping_{nullptr};
    const Block* block_{nullptr};
};

} // namespace mvcapture
