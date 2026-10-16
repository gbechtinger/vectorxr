"""Live view of the DLSS motion vectors the mvcapture DLL copies to the CPU.

Needs, in the game's environment (set before starting DCS):
    VECTORXR_MVCAPTURE_PREVIEW=1

Run (a normal terminal is enough):
    python mvcapture_viewer.py            # finds dcs.exe
    python mvcapture_viewer.py 6004       # or give the process id

Colour = direction, brightness = length. Keys: q quit, + / - change the length
that is shown as full brightness (pixels of the game's render target).
Requires: numpy, opencv-python, psutil (psutil only to find DCS).
"""
import ctypes
import struct
import sys
import time

import cv2
import numpy as np

FILE_MAP_READ = 0x0004
HEADER_BYTES = 256
SLOT_BYTES = 16 * 1024 * 1024
MAGIC = 0x3150564D  # "MVP1"

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
kernel32.OpenFileMappingW.restype = ctypes.c_void_p
kernel32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t]
kernel32.MapViewOfFile.restype = ctypes.c_void_p

# DXGI_FORMAT -> (numpy dtype, channels)
FORMATS = {
    34: (np.float16, 2), 33: (np.float16, 2),    # R16G16_FLOAT / TYPELESS
    10: (np.float16, 4), 9: (np.float16, 4),     # R16G16B16A16
    16: (np.float32, 2), 15: (np.float32, 2),    # R32G32
    2: (np.float32, 4), 1: (np.float32, 4),      # R32G32B32A32
}


def find_pid():
    import psutil
    for p in psutil.process_iter(["name", "pid"]):
        if p.info["name"] and p.info["name"].lower() == "dcs.exe":
            return p.info["pid"]
    return None


def open_preview(pid):
    handle = kernel32.OpenFileMappingW(FILE_MAP_READ, False, f"Local\\VectorXR.MvCapture.Preview.{pid}")
    if not handle:
        return None
    base = kernel32.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)  # 0: the whole mapping
    return base or None


def read_slot(base, index):
    """Returns (frame_seq, width, height, format, bytes) or None."""
    off = 32 + index * 48
    for _ in range(8):
        head = ctypes.string_at(base + off, 32)
        seq, frame_seq, w, h, fmt, bpp = struct.unpack_from("<qQIIII", head)
        if seq & 1 or seq == 0 or not w or not h or w * h * bpp > SLOT_BYTES:
            return None
        data = ctypes.string_at(base + HEADER_BYTES + index * SLOT_BYTES, w * h * bpp)
        seq_after = struct.unpack_from("<q", ctypes.string_at(base + off, 8))[0]
        if seq_after == seq:
            return frame_seq, w, h, fmt, data
    return None


def to_image(w, h, fmt, data, full_scale):
    if fmt not in FORMATS:
        return None, 0.0
    dtype, channels = FORMATS[fmt]
    mv = np.frombuffer(data, dtype=dtype).reshape(h, w, channels)[:, :, :2].astype(np.float32)
    mv = np.nan_to_num(mv, nan=0.0, posinf=0.0, neginf=0.0)
    mag = np.hypot(mv[..., 0], mv[..., 1])
    ang = np.arctan2(mv[..., 1], mv[..., 0])  # -pi..pi
    hsv = np.zeros((h, w, 3), np.uint8)
    hsv[..., 0] = ((ang + np.pi) / (2 * np.pi) * 179).astype(np.uint8)
    hsv[..., 1] = 255
    hsv[..., 2] = np.clip(mag / full_scale * 255, 0, 255).astype(np.uint8)
    return cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR), float(mag.mean())


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else None
    print("Procurando o DCS...")
    while pid is None:
        pid = find_pid()
        if pid is None:
            time.sleep(2)
    print(f"DCS: PID {pid}. Esperando a memoria de preview (DCS com VECTORXR_MVCAPTURE_PREVIEW=1, missao com DLSS rodando)...")
    base = None
    while base is None:
        base = open_preview(pid)
        if base is None:
            time.sleep(1)
    magic = struct.unpack_from("<I", ctypes.string_at(base, 4))[0]
    if magic != MAGIC:
        print(f"Cabecalho inesperado (0x{magic:08x}); a DLL e a mesma versao deste script?")
        return
    print("Conectado. q = sair, + / - = ajustar o brilho.")

    full_scale = 8.0
    last = [0, 0]
    shown = {}
    while True:
        for i in range(2):
            got = read_slot(base, i)
            if got and got[0] != last[i]:
                last[i] = got[0]
                img, mean = to_image(got[1], got[2], got[3], got[4], full_scale)
                if img is not None:
                    scale = 600.0 / img.shape[0]
                    img = cv2.resize(img, (int(img.shape[1] * scale), 600), interpolation=cv2.INTER_NEAREST)
                    cv2.putText(img, f"slot {i}  {got[1]}x{got[2]}  media {mean:.2f}px  escala {full_scale:g}px", (8, 24),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)
                    shown[i] = img
        if shown:
            cv2.imshow("DCS motion vectors (cor = direcao, brilho = tamanho)", np.hstack([shown[k] for k in sorted(shown)]))
        key = cv2.waitKey(30) & 0xFF
        if key == ord("q"):
            break
        if key in (ord("+"), ord("=")):
            full_scale = max(0.25, full_scale / 2)
        if key in (ord("-"), ord("_")):
            full_scale = min(256.0, full_scale * 2)
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
