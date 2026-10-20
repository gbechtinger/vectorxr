"""
Visualizador de motion vectors do VectorXR.MvCapture (DCS World, D3D11).

Fluxo:
  1. Abre o mapeamento "Local\\VectorXR.MvCapture.<pid>" (somente leitura).
  2. Le cada slot com o seqlock (sequence impar = sendo escrito).
  3. Duplica o NT handle da textura compartilhada de dentro do processo do DCS
     (DuplicateHandle) -> o handle so vale no processo que o criou.
  4. Abre com ID3D11Device1::OpenSharedResource1, copia para uma textura staging,
     Map, decodifica e mostra com OpenCV (matiz = direcao, brilho = magnitude).

Requisitos:  pip install numpy opencv-python
Uso:         python mv_viewer.py [pid] [--max 4.0] [--zoom 1.0] [--latest]
Teclas:      d      alterna motion vectors <-> depth
             c      mostra/esconde a saida do DLSS (antes do HUD, MFDs, menus); precisa de
                    VECTORXR_MVCAPTURE_OUTPUT=1 no ambiente do DCS e do dxgi.dll novo
             + / -  muda a magnitude que satura o brilho (modo MV)
             i      inverte o colormap (modo depth)
             l      alterna entre o ring mais recente e o anterior
             q/Esc  sai
Se o DCS estiver rodando como administrador, rode este script como administrador
tambem (OpenProcess com PROCESS_DUP_HANDLE).
"""
import argparse
import ctypes
import struct
import subprocess
import sys
import time
import uuid
from ctypes import wintypes

import cv2
import numpy as np

# ----------------------------------------------------------------- layout --
HEADER_FMT = "<IIIIIiQQ"          # magic, version, size, slot_count, luid_low, luid_high, fence_handle, evaluate_calls
SLOT_FMT = "<QIIQQ3Q3Q12I4fi"     # ver parse_slot()
SLOT_BASE = 40
SLOT_SIZE = 152
BLOCK_SIZE = 648
RING = 3
# Mapeamento da saida do DLSS ("Local\\VectorXR.MvCapture.Output.<pid>"): OutputBlock = magic, version, size,
# slot_count e 4 OutputSlot de 80 bytes (sequence, valid, ring, frame_seq, fence, handle[3], w, h, fmt, array,
# generation, reserved).
OUT_HEADER_FMT = "<IIII"
OUT_SLOT_FMT = "<QIIQQ3Q6I"
OUT_SLOT_BASE = 16
OUT_SLOT_SIZE = 80
OUT_BLOCK_SIZE = 16 + 4 * 80
MAX_SLOTS = (BLOCK_SIZE - SLOT_BASE) // SLOT_SIZE  # 4

# DXGI_FORMAT -> (nome, dtype numpy, canais, divisor para normalizar)
FORMATS = {
    2:  ("R32G32B32A32_FLOAT", np.float32, 4, 1.0),
    9:  ("R16G16B16A16_TYPELESS(float?)", np.float16, 4, 1.0),
    10: ("R16G16B16A16_FLOAT", np.float16, 4, 1.0),
    11: ("R16G16B16A16_UNORM", np.uint16, 4, 65535.0),
    13: ("R16G16B16A16_SNORM", np.int16, 4, 32767.0),
    15: ("R32G32_TYPELESS(float?)", np.float32, 2, 1.0),
    16: ("R32G32_FLOAT", np.float32, 2, 1.0),
    33: ("R16G16_TYPELESS(float?)", np.float16, 2, 1.0),
    34: ("R16G16_FLOAT", np.float16, 2, 1.0),
    35: ("R16G16_UNORM", np.uint16, 2, 65535.0),
    37: ("R16G16_SNORM", np.int16, 2, 32767.0),
    49: ("R8G8_UNORM", np.uint8, 2, 255.0),
    51: ("R8G8_SNORM", np.int8, 2, 127.0),
    # formatos de 1 canal (depth capturado pelo DCS; convertido costuma ser R32_FLOAT)
    39: ("R32_TYPELESS(float?)", np.float32, 1, 1.0),
    41: ("R32_FLOAT", np.float32, 1, 1.0),
    53: ("R16_TYPELESS(unorm?)", np.uint16, 1, 65535.0),
    54: ("R16_FLOAT", np.float16, 1, 1.0),
    56: ("R16_UNORM", np.uint16, 1, 65535.0),
    61: ("R8_UNORM", np.uint8, 1, 255.0),
}

# Formatos da saida do DLSS (cor): DXGI_FORMAT -> (nome, dtype, canais, divisor, ordem, linear)
# ordem "rgb": canais R,G,B,A; "bgr": B,G,R,A. linear: float/HDR, mostrado com gamma 1/2.2 (sRGB aproximado).
OUTPUT_FORMATS = {
    2:  ("R32G32B32A32_FLOAT", np.float32, 4, 1.0, "rgb", True),
    9:  ("R16G16B16A16_TYPELESS(float?)", np.float16, 4, 1.0, "rgb", True),
    10: ("R16G16B16A16_FLOAT", np.float16, 4, 1.0, "rgb", True),
    11: ("R16G16B16A16_UNORM", np.uint16, 4, 65535.0, "rgb", False),
    27: ("R8G8B8A8_TYPELESS", np.uint8, 4, 255.0, "rgb", False),
    28: ("R8G8B8A8_UNORM", np.uint8, 4, 255.0, "rgb", False),
    29: ("R8G8B8A8_UNORM_SRGB", np.uint8, 4, 255.0, "rgb", False),
    87: ("B8G8R8A8_UNORM", np.uint8, 4, 255.0, "bgr", False),
    90: ("B8G8R8A8_TYPELESS", np.uint8, 4, 255.0, "bgr", False),
    91: ("B8G8R8A8_UNORM_SRGB", np.uint8, 4, 255.0, "bgr", False),
}
PACKED_OUTPUT_FORMATS = {24: "R10G10B10A2_UNORM", 26: "R11G11B10_FLOAT"}


def decode_r11g11b10(v):
    """R11G11B10_FLOAT (uint32 por pixel) -> float32 (H, W, 3) em RGB."""
    def unpack(bits, mant_bits):
        e = (bits >> mant_bits).astype(np.int32)
        m = (bits & ((1 << mant_bits) - 1)).astype(np.float32) / float(1 << mant_bits)
        val = np.where(e == 0, m * 2.0 ** -14, (1.0 + m) * np.exp2((e - 15).astype(np.float32)))
        return np.where(e == 31, 65504.0, val).astype(np.float32)
    r = unpack(v & 0x7FF, 6)
    g = unpack((v >> 11) & 0x7FF, 6)
    b = unpack((v >> 22) & 0x3FF, 5)
    return np.dstack([r, g, b])


# ---------------------------------------------------------------- win32 -----
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.DuplicateHandle.restype = wintypes.BOOL
k32.DuplicateHandle.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
                                ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
k32.GetCurrentProcess.restype = wintypes.HANDLE

FILE_MAP_READ = 0x0004
PROCESS_DUP_HANDLE = 0x0040
DUPLICATE_SAME_ACCESS = 0x00000002


def guid(s):
    return (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID(s).bytes_le)


IID_ID3D11Device1 = guid("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686")
IID_ID3D11Texture2D = guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c")
IID_IDXGIFactory4 = guid("1bc6ea02-ef36-464f-bf0c-21ca39e5168a")
IID_IDXGIAdapter = guid("2411e7e1-12ac-4ccf-bd14-9798e8534dc0")


def vcall(obj, idx, argtypes, *args, restype=ctypes.c_long):
    """Chama o metodo `idx` da vtable de uma interface COM (obj = ponteiro como int)."""
    vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    proto = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)
    return proto(vtbl[idx])(obj, *args)


def release(obj):
    if obj:
        vcall(obj, 2, [], restype=ctypes.c_ulong)


class SAMPLE(ctypes.Structure):
    _fields_ = [("Count", ctypes.c_uint), ("Quality", ctypes.c_uint)]


class TEX2D_DESC(ctypes.Structure):
    _fields_ = [("Width", ctypes.c_uint), ("Height", ctypes.c_uint), ("MipLevels", ctypes.c_uint),
                ("ArraySize", ctypes.c_uint), ("Format", ctypes.c_uint), ("SampleDesc", SAMPLE),
                ("Usage", ctypes.c_uint), ("BindFlags", ctypes.c_uint),
                ("CPUAccessFlags", ctypes.c_uint), ("MiscFlags", ctypes.c_uint)]


class MAPPED(ctypes.Structure):
    _fields_ = [("pData", ctypes.c_void_p), ("RowPitch", ctypes.c_uint), ("DepthPitch", ctypes.c_uint)]


# indices de vtable
DEV_CreateTexture2D = 5
DEV_OpenSharedResource1 = 48
DEV_GetImmediateContext = 40
CTX_Map = 14
CTX_Unmap = 15
CTX_CopyResource = 47
TEX_GetDesc = 10
FACT4_EnumAdapterByLuid = 26

D3D11_USAGE_STAGING = 3
D3D11_CPU_ACCESS_READ = 0x20000
D3D11_MAP_READ = 1


def create_device(luid_low, luid_high):
    """Cria um device D3D11 no mesmo adaptador do DCS (via LUID); cai no padrao se falhar."""
    adapter = ctypes.c_void_p(0)
    try:
        factory = ctypes.c_void_p(0)
        dxgi = ctypes.WinDLL("dxgi")
        if dxgi.CreateDXGIFactory1(ctypes.byref(IID_IDXGIFactory4), ctypes.byref(factory)) == 0:
            luid = (luid_low & 0xFFFFFFFF) | ((luid_high & 0xFFFFFFFF) << 32)
            hr = vcall(factory.value, FACT4_EnumAdapterByLuid,
                       [ctypes.c_uint64, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
                       luid, ctypes.byref(IID_IDXGIAdapter), ctypes.byref(adapter))
            release(factory.value)
            if hr != 0:
                adapter = ctypes.c_void_p(0)
    except Exception:
        adapter = ctypes.c_void_p(0)

    d3d11 = ctypes.WinDLL("d3d11")
    d3d11.D3D11CreateDevice.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint,
                                        ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
                                        ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
                                        ctypes.POINTER(ctypes.c_void_p)]
    dev, ctx = ctypes.c_void_p(0), ctypes.c_void_p(0)
    driver = 0 if adapter.value else 1  # UNKNOWN com adaptador explicito, senao HARDWARE
    hr = d3d11.D3D11CreateDevice(adapter, driver, None, 0, None, 0, 7,
                                 ctypes.byref(dev), None, ctypes.byref(ctx))
    release(adapter.value)
    if hr != 0:
        raise RuntimeError(f"D3D11CreateDevice falhou: 0x{hr & 0xFFFFFFFF:08x}")

    dev1 = ctypes.c_void_p(0)
    hr = vcall(dev.value, 0, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
               ctypes.byref(IID_ID3D11Device1), ctypes.byref(dev1))
    if hr != 0:
        raise RuntimeError("ID3D11Device1 indisponivel (precisa Windows 8+)")
    release(dev.value)
    return dev1.value, ctx.value


# --------------------------------------------------------------- captura ----
def parse_slot(raw):
    f = struct.unpack(SLOT_FMT, raw[:struct.calcsize(SLOT_FMT)])
    return {
        "seq": f[0], "valid": f[1], "ring": f[2], "frame_seq": f[3], "fence": f[4],
        "mv_handle": f[5:8], "depth_handle": f[8:11],
        "width": f[11], "height": f[12], "mv_fmt": f[13], "depth_fmt": f[14], "depth_conv": f[15],
        "array": f[16], "render_w": f[17], "render_h": f[18], "mv_bx": f[19], "mv_by": f[20],
        "d_bx": f[21], "d_by": f[22], "jitter": (f[23], f[24]), "mv_scale": (f[25], f[26]), "reset": f[27],
    }


class Capture:
    def __init__(self, pid):
        self.pid = pid
        name = f"Local\\VectorXR.MvCapture.{pid}"
        h = k32.OpenFileMappingW(FILE_MAP_READ, False, name)
        if not h:
            raise RuntimeError(f"nao consegui abrir {name} (erro {ctypes.get_last_error()})")
        addr = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, BLOCK_SIZE)
        if not addr:
            raise RuntimeError("MapViewOfFile falhou")
        self.buf = (ctypes.c_char * BLOCK_SIZE).from_address(addr)
        hdr = struct.unpack_from(HEADER_FMT, self.buf, 0)
        if hdr[2] != BLOCK_SIZE:
            print(f"aviso: size={hdr[2]} (esperado {BLOCK_SIZE}); os offsets podem estar errados")
        self.luid = (hdr[4], hdr[5])

        self.proc = k32.OpenProcess(PROCESS_DUP_HANDLE, False, pid)
        if not self.proc:
            raise RuntimeError(f"OpenProcess({pid}) falhou (erro {ctypes.get_last_error()}); "
                               "tente rodar como administrador")
        self.dev, self.ctx = create_device(*self.luid)
        self.cache = {}  # (slot, ring) -> dict
        self.out_buf = None

    def open_output(self):
        """Abre o mapeamento da saida do DLSS quando ele existir (o DCS precisa de VECTORXR_MVCAPTURE_OUTPUT=1)."""
        if self.out_buf is not None:
            return True
        h = k32.OpenFileMappingW(FILE_MAP_READ, False, f"Local\\VectorXR.MvCapture.Output.{self.pid}")
        if not h:
            return False
        addr = k32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, OUT_BLOCK_SIZE)
        if not addr:
            return False
        buf = (ctypes.c_char * OUT_BLOCK_SIZE).from_address(addr)
        if struct.unpack_from(OUT_HEADER_FMT, buf, 0)[2] != OUT_BLOCK_SIZE:
            print("aviso: o mapeamento da saida tem tamanho diferente do esperado; os offsets podem estar errados")
        self.out_buf = buf
        return True

    def read_output_slot(self, i):
        if not self.open_output():
            return None
        o = OUT_SLOT_BASE + i * OUT_SLOT_SIZE
        for _ in range(200):
            s1 = struct.unpack_from("<Q", self.out_buf, o)[0]
            if s1 & 1:
                continue
            raw = bytes(self.out_buf[o:o + OUT_SLOT_SIZE])
            s2 = struct.unpack_from("<Q", self.out_buf, o)[0]
            if s1 == s2:
                f = struct.unpack(OUT_SLOT_FMT, raw)
                return {"valid": f[1], "ring": f[2], "frame_seq": f[3], "fence": f[4], "output_handle": f[5:8],
                        "width": f[8], "height": f[9], "format": f[10], "array": f[11], "generation": f[12]}
        return None

    def header(self):
        return struct.unpack_from(HEADER_FMT, self.buf, 0)

    def read_slot(self, i):
        o = SLOT_BASE + i * SLOT_SIZE
        for _ in range(200):
            s1 = struct.unpack_from("<Q", self.buf, o)[0]
            if s1 & 1:
                continue
            raw = bytes(self.buf[o:o + SLOT_SIZE])
            s2 = struct.unpack_from("<Q", self.buf, o)[0]
            if s1 == s2:
                return parse_slot(raw)
        return None

    def _free(self, key):
        ent = self.cache.pop(key, None)
        if ent:
            release(ent["tex"])
            release(ent["stg"])

    @staticmethod
    def _sig(info, kind):
        if kind == "output":
            return (kind, info["width"], info["height"], info["format"], info["generation"])
        return (kind, info["width"], info["height"], info["mv_fmt"], info["depth_fmt"], info["depth_conv"])

    def _open(self, key, remote, info, kind):
        local = wintypes.HANDLE()
        if not k32.DuplicateHandle(self.proc, ctypes.c_void_p(remote), k32.GetCurrentProcess(),
                                   ctypes.byref(local), 0, False, DUPLICATE_SAME_ACCESS):
            raise RuntimeError(f"DuplicateHandle falhou (erro {ctypes.get_last_error()})")
        tex = ctypes.c_void_p(0)
        hr = vcall(self.dev, DEV_OpenSharedResource1,
                   [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
                   local, ctypes.byref(IID_ID3D11Texture2D), ctypes.byref(tex))
        k32.CloseHandle(local)
        if hr != 0:
            raise RuntimeError(f"OpenSharedResource1 falhou: 0x{hr & 0xFFFFFFFF:08x}")

        desc = TEX2D_DESC()
        vcall(tex.value, TEX_GetDesc, [ctypes.POINTER(TEX2D_DESC)], ctypes.byref(desc), restype=None)
        sdesc = TEX2D_DESC.from_buffer_copy(desc)
        sdesc.Usage = D3D11_USAGE_STAGING
        sdesc.BindFlags = 0
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ
        sdesc.MiscFlags = 0
        stg = ctypes.c_void_p(0)
        hr = vcall(self.dev, DEV_CreateTexture2D,
                   [ctypes.POINTER(TEX2D_DESC), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
                   ctypes.byref(sdesc), None, ctypes.byref(stg))
        if hr != 0:
            release(tex.value)
            raise RuntimeError(f"CreateTexture2D(staging) falhou: 0x{hr & 0xFFFFFFFF:08x}")
        self.cache[key] = {"remote": remote, "sig": self._sig(info, kind),
                           "tex": tex.value, "stg": stg.value,
                           "w": desc.Width, "h": desc.Height, "fmt": desc.Format}

    def grab(self, slot, info, ring, kind="mv"):
        """kind='mv': float32 (H, W, 2) em pixels (antes do mv_scale).
        kind='depth': float32 (H, W) com os valores crus do buffer de profundidade."""
        remote = info[kind + "_handle"][ring]
        if not remote:
            return None
        key = (slot, ring, kind)
        ent = self.cache.get(key)
        if not ent or ent["remote"] != remote or ent["sig"] != self._sig(info, kind):
            self._free(key)
            self._open(key, remote, info, kind)
            ent = self.cache[key]

        self.last_fmt = ent["fmt"]
        if kind == "output":
            fmt = ("output",)
        else:
            fmt = FORMATS.get(ent["fmt"])
        if not fmt:
            raise RuntimeError(f"formato DXGI {ent['fmt']} nao suportado; adicione em FORMATS")
        if kind != "output":
            _, dtype, ch, div = fmt

        if kind == "output":
            return self._read_output(ent)
        vcall(self.ctx, CTX_CopyResource, [ctypes.c_void_p, ctypes.c_void_p], ent["stg"], ent["tex"], restype=None)
        m = MAPPED()
        hr = vcall(self.ctx, CTX_Map,
                   [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(MAPPED)],
                   ent["stg"], 0, D3D11_MAP_READ, 0, ctypes.byref(m))
        if hr != 0:
            raise RuntimeError(f"Map falhou: 0x{hr & 0xFFFFFFFF:08x}")
        try:
            w, h = ent["w"], ent["h"]
            bpp = np.dtype(dtype).itemsize * ch
            raw = (ctypes.c_ubyte * (m.RowPitch * h)).from_address(m.pData)
            rows = np.frombuffer(raw, dtype=np.uint8).reshape(h, m.RowPitch)[:, :w * bpp]
            arr = np.ascontiguousarray(rows).view(dtype).reshape(h, w, ch)
            out = (arr[:, :, :2] if kind == "mv" else arr[:, :, 0]).astype(np.float32)
        finally:
            vcall(self.ctx, CTX_Unmap, [ctypes.c_void_p, ctypes.c_uint], ent["stg"], 0, restype=None)
        if div != 1.0:
            out /= div
        return out


# ---------------------------------------------------------- visualizacao ----
def _read_output(self, ent):
    """Copia a saida do DLSS para a CPU e devolve uint8 BGR (H, W, 3) pronto para o OpenCV."""
    fmt_id = ent["fmt"]
    packed = fmt_id in PACKED_OUTPUT_FORMATS
    spec = OUTPUT_FORMATS.get(fmt_id)
    if not spec and not packed:
        raise RuntimeError(f"formato DXGI {fmt_id} da saida nao suportado; adicione em OUTPUT_FORMATS")
    vcall(self.ctx, CTX_CopyResource, [ctypes.c_void_p, ctypes.c_void_p], ent["stg"], ent["tex"], restype=None)
    m = MAPPED()
    hr = vcall(self.ctx, CTX_Map,
               [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(MAPPED)],
               ent["stg"], 0, D3D11_MAP_READ, 0, ctypes.byref(m))
    if hr != 0:
        raise RuntimeError(f"Map falhou: 0x{hr & 0xFFFFFFFF:08x}")
    try:
        w, h = ent["w"], ent["h"]
        raw = (ctypes.c_ubyte * (m.RowPitch * h)).from_address(m.pData)
        data = np.frombuffer(raw, dtype=np.uint8).reshape(h, m.RowPitch)
        if packed:
            v = np.ascontiguousarray(data[:, :w * 4]).view(np.uint32).reshape(h, w)
            if fmt_id == 26:
                rgb = decode_r11g11b10(v)
                linear = True
            else:
                rgb = np.dstack([(v & 0x3FF), (v >> 10) & 0x3FF, (v >> 20) & 0x3FF]).astype(np.float32) / 1023.0
                linear = False
        else:
            _, dtype, ch, div, order, linear = spec
            bpp = np.dtype(dtype).itemsize * ch
            arr = np.ascontiguousarray(data[:, :w * bpp]).view(dtype).reshape(h, w, ch)
            rgb = arr[:, :, :3].astype(np.float32) / div
            if order == "bgr":
                rgb = rgb[:, :, ::-1]
    finally:
        vcall(self.ctx, CTX_Unmap, [ctypes.c_void_p, ctypes.c_uint], ent["stg"], 0, restype=None)
    rgb = np.clip(np.nan_to_num(rgb, nan=0.0, posinf=1.0, neginf=0.0), 0.0, 1.0)
    if linear:
        rgb = rgb ** (1.0 / 2.2)
    return np.ascontiguousarray((rgb[:, :, ::-1] * 255.0 + 0.5).astype(np.uint8))


Capture._read_output = _read_output



def mv_to_bgr(mv, scale, max_px):
    dx = np.nan_to_num(mv[..., 0]) * scale[0]
    dy = np.nan_to_num(mv[..., 1]) * scale[1]
    mag = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)  # -pi..pi
    hue = ((ang + np.pi) / (2 * np.pi) * 179).astype(np.uint8)
    val = (np.clip(mag / max_px, 0, 1) * 255).astype(np.uint8)
    hsv = np.dstack([hue, np.full_like(hue, 255), val])
    return cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR), mag


def depth_to_bgr(d, invert):
    """Colormap TURBO com contraste automatico (percentis 1-99), pois o buffer de
    profundidade nao e linear e costuma ficar concentrado perto de 0 ou de 1."""
    d = np.nan_to_num(d, nan=0.0, posinf=0.0, neginf=0.0)
    lo, hi = np.percentile(d[::4, ::4], (1, 99))
    if hi <= lo:
        lo, hi = float(d.min()), float(d.max()) + 1e-9
    n = np.clip((d - lo) / (hi - lo + 1e-12), 0, 1)
    if invert:
        n = 1.0 - n
    return cv2.applyColorMap((n * 255).astype(np.uint8), cv2.COLORMAP_TURBO), d, lo, hi


def find_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DCS.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == "dcs.exe":
            return int(parts[1])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", nargs="?", type=int)
    ap.add_argument("--max", type=float, default=4.0, help="magnitude (px) que satura o brilho")
    ap.add_argument("--zoom", type=float, default=1.0)
    ap.add_argument("--latest", action="store_true",
                    help="le o ring mais recente (sem espera de fence pode mostrar frame incompleto)")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    if not pid:
        sys.exit("DCS.exe nao encontrado; passe o PID como argumento")
    cap = Capture(pid)
    print(f"PID {pid}, LUID {cap.luid[1]:08x}:{cap.luid[0]:08x}")

    max_px = args.max
    use_latest = args.latest
    mode = "mv"       # "mv", "depth" ou "output" (saida do DLSS)
    invert = False    # inverte o colormap do depth
    last = {}   # slot -> (frame_seq, imagem)
    t0, frames = time.time(), 0

    while True:
        hdr = cap.header()
        tiles = []
        for i in range(min(hdr[3], MAX_SLOTS)):
            info = cap.read_slot(i)
            if not info or not info["valid"]:
                continue
            if mode == "output":
                oinfo = cap.read_output_slot(i)
                if not oinfo or not oinfo["valid"]:
                    if i not in last or last[i][2] != ("sem saida", mode):
                        msg = np.zeros((120, 640, 3), np.uint8)
                        for n, t in enumerate([f"slot {i}: sem captura da saida do DLSS",
                                               "o DCS precisa de VECTORXR_MVCAPTURE_OUTPUT=1 e do dxgi.dll novo"]):
                            cv2.putText(msg, t, (8, 40 + 24 * n), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1,
                                        cv2.LINE_AA)
                        last[i] = (-1, msg, ("sem saida", mode))
                    tiles.append(last[i][1])
                    continue
                if i not in last or last[i][0] != oinfo["frame_seq"] or last[i][2] != ("saida", mode):
                    ring = oinfo["ring"] if use_latest else (oinfo["ring"] - 1) % RING
                    try:
                        img = cap.grab(i, oinfo, ring, "output")
                    except Exception as e:
                        print(f"slot {i}: {e}")
                        img = None
                    if img is not None:
                        name = OUTPUT_FORMATS.get(cap.last_fmt, (PACKED_OUTPUT_FORMATS.get(cap.last_fmt, f"fmt {cap.last_fmt}"),))[0]
                        for n, t in enumerate([f"SAIDA DLSS slot {i}  frame {oinfo['frame_seq']}  "
                                               f"{img.shape[1]}x{img.shape[0]}  {name}"]):
                            cv2.putText(img, t, (8, 20 + 18 * n), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1,
                                        cv2.LINE_AA)
                        last[i] = (oinfo["frame_seq"], img, ("saida", mode))
                        frames += 1
                    else:
                        last.pop(i, None)
                if i in last:
                    tiles.append(last[i][1])
                continue
            if i not in last or last[i][0] != info["frame_seq"] or last[i][2] != (max_px, mode, invert):
                ring = info["ring"] if use_latest else (info["ring"] - 1) % RING
                try:
                    mv = cap.grab(i, info, ring, mode)
                except Exception as e:
                    print(f"slot {i}: {e}")
                    mv = None
                if mv is not None:
                    rw, rh = info["render_w"], info["render_h"]
                    name = FORMATS.get(cap.last_fmt, (f"fmt {cap.last_fmt}",))[0]
                    if mode == "mv":
                        bx, by = info["mv_bx"], info["mv_by"]
                        if rw and rh:
                            mv = mv[by:by + rh, bx:bx + rw]
                        img, mag = mv_to_bgr(mv, info["mv_scale"], max_px)
                        lines = [f"MV slot {i}  frame {info['frame_seq']}  {img.shape[1]}x{img.shape[0]}  {name}",
                                 f"mag max {mag.max():.2f}  media {mag.mean():.3f} px",
                                 f"jitter ({info['jitter'][0]:.3f},{info['jitter'][1]:.3f})  reset {info['reset']}"]
                    else:
                        bx, by = info["d_bx"], info["d_by"]
                        if rw and rh:
                            mv = mv[by:by + rh, bx:bx + rw]
                        img, dv, lo, hi = depth_to_bgr(mv, invert)
                        conv = "convertido p/ R32_FLOAT" if info["depth_conv"] else "copia direta"
                        lines = [f"DEPTH slot {i}  frame {info['frame_seq']}  {img.shape[1]}x{img.shape[0]}  {name} ({conv})",
                                 f"min {dv.min():.5f}  max {dv.max():.5f}  media {dv.mean():.5f}",
                                 f"contraste p1-p99: {lo:.5f} .. {hi:.5f}  invert={invert}"]
                    for n, t in enumerate(lines):
                        cv2.putText(img, t, (8, 20 + 18 * n), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1,
                                    cv2.LINE_AA)
                    last[i] = (info["frame_seq"], img, (max_px, mode, invert))
                    frames += 1
                else:
                    last.pop(i, None)  # ex.: sem depth neste slot -> nao mostra imagem velha do outro modo
            if i in last:
                tiles.append(last[i][1])

        if tiles:
            hmax = max(t.shape[0] for t in tiles)
            tiles = [cv2.copyMakeBorder(t, 0, hmax - t.shape[0], 0, 0, cv2.BORDER_CONSTANT) for t in tiles]
            view = np.hstack(tiles)
            if args.zoom != 1.0:
                view = cv2.resize(view, None, fx=args.zoom, fy=args.zoom, interpolation=cv2.INTER_NEAREST)
            cv2.imshow("VectorXR motion vectors", view)

        k = cv2.waitKey(1) & 0xFF
        if k in (27, ord("q")):
            break
        elif k in (ord("+"), ord("=")):
            max_px *= 1.5
        elif k in (ord("-"), ord("_")):
            max_px /= 1.5
        elif k == ord("d"):
            mode = "depth" if mode == "mv" else "mv"
            last.clear()
            print("modo:", mode)
        elif k == ord("c"):
            mode = "mv" if mode == "output" else "output"
            last.clear()
            print("modo:", mode)
        elif k == ord("i"):
            invert = not invert
        elif k == ord("l"):
            use_latest = not use_latest
            print("ring:", "mais recente" if use_latest else "anterior")
        if time.time() - t0 > 2:
            print(f"{frames / (time.time() - t0):.1f} capturas/s  (max_px={max_px:.2f}, evaluate_calls={hdr[7]})")
            t0, frames = time.time(), 0
        time.sleep(0.002)


if __name__ == "__main__":
    main()
