"""Lector de ROMs de Nintendo DS.

Saca de la ROM lo que necesita el traductor:
  - la cabecera (código del juego, dónde está cada binario)
  - el ARM9 (el procesador principal, donde va el juego), ya descomprimido
  - el ARM7 (el segundo procesador: sonido, táctil, wifi)
  - la tabla de overlays del ARM9 (trozos de código que se cargan y descargan)

Uso:
    python tools/nds_rom.py rom/mkds.nds            -> resumen
    python tools/nds_rom.py rom/mkds.nds extraido   -> además lo extrae a la carpeta
"""

import json
import os
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def blz_decompress(data):
    """Descompresión "backwards LZ" del SDK de Nintendo (MI_UncompressBackward).

    Los últimos 8 bytes dicen cuánto crece y qué parte está comprimida; se lee
    desde el final hacia el principio.
    """
    buf = bytes(data)
    inc_len = u32(buf, len(buf) - 4)
    if inc_len == 0:
        return bytearray(buf)
    hdr = u32(buf, len(buf) - 8)
    hdr_len = hdr >> 24
    enc_len = hdr & 0xFFFFFF
    raw_end = len(buf) - enc_len  # lo que hay antes está sin comprimir
    out = bytearray(len(buf) + inc_len)
    out[:raw_end] = buf[:raw_end]

    src = len(buf) - hdr_len
    dst = len(out)
    while dst > raw_end and src > raw_end:
        src -= 1
        flags = buf[src]
        for _ in range(8):
            if dst <= raw_end or src <= raw_end:
                break
            if flags & 0x80:
                b1 = buf[src - 1]
                b2 = buf[src - 2]
                src -= 2
                length = (b1 >> 4) + 3
                disp = (((b1 & 0xF) << 8) | b2) + 3
                for _ in range(length):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = buf[src]
            flags = (flags << 1) & 0xFF
    return out


MODULE_MAGIC = struct.pack("<II", 0xDEC00621, 0x2106C0DE)


class NdsRom:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        self.title = d[0:12].rstrip(b"\0").decode("ascii", "replace")
        self.gamecode = d[12:16].decode("ascii", "replace")
        (self.arm9_off, self.arm9_entry, self.arm9_ram, self.arm9_size,
         self.arm7_off, self.arm7_entry, self.arm7_ram, self.arm7_size,
         self.fnt_off, self.fnt_size, self.fat_off, self.fat_size,
         self.ovt9_off, self.ovt9_size, self.ovt7_off, self.ovt7_size) = struct.unpack_from("<16I", d, 0x20)

        self.arm9_raw = d[self.arm9_off:self.arm9_off + self.arm9_size]
        self.arm7 = d[self.arm7_off:self.arm7_off + self.arm7_size]
        self.fat = [struct.unpack_from("<II", d, self.fat_off + i * 8)
                    for i in range(self.fat_size // 8)]
        self._read_arm9()
        self.overlays9 = self._read_overlays(self.ovt9_off, self.ovt9_size)

    # -- ARM9 ---------------------------------------------------------------
    def _read_arm9(self):
        raw = self.arm9_raw
        pos = raw.find(MODULE_MAGIC)
        self.module_params = None
        self.arm9 = bytearray(raw)
        self.arm9_compressed = False
        if pos < 0:
            return
        mp_off = pos - 0x1C  # 6 campos + versión del SDK, luego los dos números mágicos
        names = ["autoload_list_start", "autoload_list_end", "autoload_start",
                 "static_bss_start", "static_bss_end", "compressed_static_end"]
        mp = dict(zip(names, struct.unpack_from("<6I", raw, mp_off)))
        mp["offset"] = mp_off
        self.module_params = mp
        end = mp["compressed_static_end"]
        if end:
            cut = end - self.arm9_ram
            dec = blz_decompress(raw[:cut])
            self.arm9 = dec + bytearray(raw[cut:])
            self.arm9_compressed = True
            # como hacen los emuladores y ndstool: marcarlo como ya descomprimido
            # para que el arranque del juego no lo intente otra vez
            struct.pack_into("<I", self.arm9, mp_off + 0x14, 0)

    def autoloads(self):
        """Bloques que el arranque copia a ITCM/DTCM (memorias rápidas)."""
        mp = self.module_params
        if not mp:
            return []
        out = []
        src = mp["autoload_start"] - self.arm9_ram
        lst = mp["autoload_list_start"] - self.arm9_ram
        lst_end = mp["autoload_list_end"] - self.arm9_ram
        while lst < lst_end:
            dest, size, bss = struct.unpack_from("<III", self.arm9, lst)
            out.append({"ram": dest, "size": size, "bss": bss, "offset": src})
            src += size
            lst += 12
        return out

    # -- overlays -----------------------------------------------------------
    def _read_overlays(self, off, size):
        ovs = []
        for i in range(size // 32):
            (ov_id, ram, ram_size, bss, sinit_s, sinit_e, file_id,
             flags) = struct.unpack_from("<8I", self.data, off + i * 32)
            start, end = self.fat[file_id]
            ovs.append({
                "id": ov_id, "ram": ram, "ram_size": ram_size, "bss": bss,
                "sinit_start": sinit_s, "sinit_end": sinit_e, "file": file_id,
                "compressed": bool((flags >> 24) & 1),
                "rom_start": start, "rom_end": end,
            })
        return ovs

    def overlay_data(self, ov):
        raw = self.data[ov["rom_start"]:ov["rom_end"]]
        return blz_decompress(raw) if ov["compressed"] else bytearray(raw)

    # -- resumen ------------------------------------------------------------
    def summary(self):
        lines = [
            f"Juego: {self.title}  ({self.gamecode})",
            f"ARM9: {len(self.arm9_raw)} bytes en ROM -> {len(self.arm9)} bytes"
            f"{' (estaba comprimido)' if self.arm9_compressed else ''}, "
            f"se carga en {self.arm9_ram:08X}, empieza en {self.arm9_entry:08X}",
            f"ARM7: {self.arm7_size} bytes en {self.arm7_ram:08X}, empieza en {self.arm7_entry:08X}",
        ]
        for a in self.autoloads():
            lines.append(f"  autoload -> {a['ram']:08X}  {a['size']} bytes (+{a['bss']} bss)")
        lines.append(f"Overlays ARM9: {len(self.overlays9)}")
        for ov in self.overlays9:
            lines.append(f"  #{ov['id']:3d} en {ov['ram']:08X}  {ov['ram_size']:7d} bytes"
                         f"{'  comprimido' if ov['compressed'] else ''}")
        return "\n".join(lines)

    def extract(self, folder):
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "arm9.bin"), "wb") as f:
            f.write(self.arm9)
        with open(os.path.join(folder, "arm7.bin"), "wb") as f:
            f.write(self.arm7)
        for ov in self.overlays9:
            with open(os.path.join(folder, f"overlay9_{ov['id']}.bin"), "wb") as f:
                f.write(self.overlay_data(ov))
        info = {
            "title": self.title, "gamecode": self.gamecode,
            "arm9_ram": self.arm9_ram, "arm9_entry": self.arm9_entry,
            "arm7_ram": self.arm7_ram, "arm7_entry": self.arm7_entry,
            "module_params": self.module_params, "autoloads": self.autoloads(),
            "overlays9": self.overlays9,
        }
        with open(os.path.join(folder, "info.json"), "w") as f:
            json.dump(info, f, indent=2)


if __name__ == "__main__":
    rom = NdsRom(sys.argv[1])
    print(rom.summary())
    if len(sys.argv) > 2:
        rom.extract(sys.argv[2])
        print(f"Extraído en {sys.argv[2]}")
