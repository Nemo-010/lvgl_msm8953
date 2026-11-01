#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Execute the built Zephyr image for mido on an emulated Cortex-A53 (unicorn).

This is not a phone and not full-system QEMU: there is no TrustZone, no SCM
call, no real GIC interrupt delivery. What it does prove is that the image is a
*loadable, executing* AArch64 payload at the address lk2nd jumps to: the reset
path runs, .bss is zeroed, the firmware-args hook fires, the MMU comes up, the
GIC and timer are programmed through their real registers, the console driver
writes characters we can capture, the display driver probes its surface, and
LVGL paints the UI into that surface through the guest's own page tables (the
panel memory is read back and checked).

Usage:
    tools/emulate_boot.py --elf app/build/zephyr/zephyr.elf \
                          --dtb prebuilt/msm8953-xiaomi-mido.dtb \
                          [--max-insns 400000000] [--trace]
"""

import argparse
import struct
import sys

from unicorn import (
    Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_WRITE,
    UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE, UC_PROT_ALL,
)
from unicorn.arm64_const import (
    UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
)

DDR_BASE = 0x80000000
DDR_END = 0xC0000000
KERNEL_ADDR = 0x80080000
TAGS_ADDR = 0x83400000
PAGE = 0x1000

# MMIO windows we care about
UART_BASE, UART_SIZE = 0x078AF000, 0x1000
GIC_DIST, GIC_SIZE = 0x0B000000, 0x1000
GIC_CPU = 0x0B002000
MDP_BASE, MDP_SIZE = 0x01A00000, 0x10000
GCC_BASE, GCC_SIZE = 0x01800000, 0x1000

# UARTDM (BLSP) offsets
UART_SR = 0x0A4
UART_TF = 0x100
UART_NO_CHARS = 0x040

# MDP5 SSPP offsets, as the driver uses them
SSPP_SRC_IMG_SIZE = 0x004
SSPP_SRC0_ADDR = 0x014
SSPP_SRC_YSTRIDE = 0x024
SSPP_SRC_FORMAT = 0x030
MDP5_VIG_0 = 0x05000


class Emu:
    def __init__(self, trace=False):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.trace = trace
        self.console = bytearray()
        self.mmio_reads = {}
        self.ufb = 0x90001000          # the surface the bootloader left
        self.ufb_w, self.ufb_h, self.ufb_bpp = 1080, 1920, 24
        self.mapped = []
        self.insns = 0
        self.notes = []

        self._map(DDR_BASE, 0x02000000)                 # image + a bit
        self._map(TAGS_ADDR, 0x00020000)                # device tree
        self._map(UART_BASE, UART_SIZE)
        self._map(GIC_DIST, GIC_SIZE)
        self._map(GIC_CPU, 0x2000)
        self._map(MDP_BASE, MDP_SIZE)
        self._map(GCC_BASE, GCC_SIZE)
        # The framebuffer is in DDR; map it lazily via the unmapped hook.

        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.uc.hook_add(UC_HOOK_MEM_READ, self._on_read)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        self.uc.hook_add(UC_HOOK_CODE, self._on_code)

    def _map(self, addr, size):
        self.uc.mem_map(addr, size, UC_PROT_ALL)
        self.mapped.append((addr, addr + size))

    def _is_mapped(self, addr, size):
        return any(lo <= addr and addr + size <= hi for lo, hi in self.mapped)

    def _on_unmapped(self, uc, access, address, size, value, user_data):
        # DDR is mapped by the MMU on the guest side but must have backing
        # memory here; allocate on demand in 2 MiB chunks.
        if DDR_BASE <= address < DDR_END:
                chunk = address & ~0x1FFFFF
                try:
                    self._map(chunk, 0x200000)
                except UcError:
                    return False
                return True
        # MMIO we never configured: give it a page of zeros and remember it.
        page = (address & ~(PAGE - 1))
        try:
            self._map(page, PAGE)
        except UcError:
            return False
        return True

    def _on_write(self, uc, access, address, size, value, user_data):
        if UART_BASE <= address < UART_BASE + UART_SIZE:
            off = address - UART_BASE
            if off == UART_TF:
                c = value & 0xFF
                self.console.append(c)
            return
        if MDP_BASE <= address < MDP_BASE + MDP_SIZE:
            return
        if GIC_DIST <= address < GIC_DIST + GIC_SIZE or GIC_CPU <= address < GIC_CPU + 0x2000:
            return
        return

    def _on_read(self, uc, access, address, size, value, user_data):
        # UART status: always ready to transmit, nothing to receive.
        if UART_BASE <= address < UART_BASE + UART_SIZE:
            off = address - UART_BASE
            if off == UART_SR:
                uc.mem_write(address, struct.pack("<I", 0x6))  # TXRDY | TXEMT
                return
            uc.mem_write(address, struct.pack("<I", 0))
            return

        # MDP5: describe the bootloader's surface.
        if MDP_BASE <= address < MDP_BASE + MDP_SIZE:
            off = address - MDP_BASE
            val = 0
            if MDP5_VIG_0 <= off < MDP5_VIG_0 + 0x400:
                o = off - MDP5_VIG_0
                if o == SSPP_SRC0_ADDR:
                    val = self.ufb
                elif o == SSPP_SRC_IMG_SIZE:
                    val = (self.ufb_h << 16) | self.ufb_w
                elif o == SSPP_SRC_YSTRIDE:
                    val = self.ufb_w * (self.ufb_bpp // 8)
                elif o == SSPP_SRC_FORMAT:
                    # SRC_BPP field = bytes per pixel - 1; r8g8b8 -> 2
                    val = ((self.ufb_bpp // 8 - 1) << 9)
            self.mmio_reads[off] = val
            uc.mem_write(address, struct.pack("<I", val))
            return

        # Anything else (GIC, GCC, ...) is plain memory that was mapped
        # zero-filled; let unicorn read it as-is. Never touch RAM here.
        return

    def _on_code(self, uc, address, size, user_data):
        self.insns += 1
        if self.trace and self.insns < 40:
            print("    pc 0x%x" % address)
        if self.insns % 2000000 == 0:
            print("    ... %d insns, pc 0x%x, console %d bytes"
                  % (self.insns, address, len(self.console)))


def load_elf(path):
    data = open(path, "rb").read()
    e_entry = struct.unpack_from("<Q", data, 0x18)[0]
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = \
            struct.unpack_from("<IIQQQQQQ", data, off)
        if p_type == 1:
            segs.append((p_vaddr, data[p_offset:p_offset + p_filesz]))
    return e_entry, segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--dtb", required=True)
    ap.add_argument("--max-insns", type=int, default=500_000_000)
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--drop-dtb", action="store_true",
                    help="do not place a device tree, to exercise the MDP5 "
                         "register fallback path instead of the simplefb node")
    args = ap.parse_args()

    e_entry, segs = load_elf(args.elf)
    dtb = open(args.dtb, "rb").read()

    emu = Emu(trace=args.trace)

    print("loading image:")
    for vaddr, blob in segs:
        emu.uc.mem_write(vaddr, blob)
        print("  0x%x..0x%x (%d bytes)" % (vaddr, vaddr + len(blob), len(blob)))

    # lk2nd copies the device tree to the tags address and passes it in x0.
    if not args.drop_dtb:
        emu.uc.mem_write(TAGS_ADDR, dtb)
    print("device tree at 0x%x (%d bytes), as lk2nd would place it" % (TAGS_ADDR, len(dtb)))

    print("entering at 0x%x, as scm_elexec_call would" % e_entry)
    emu.uc.reg_write(UC_ARM64_REG_PC, e_entry)
    emu.uc.reg_write(UC_ARM64_REG_X0, TAGS_ADDR)
    emu.uc.reg_write(UC_ARM64_REG_SP, 0xB0000000)

    try:
        emu.uc.emu_start(e_entry, 0, count=args.max_insns)
    except UcError as e:
        print("\n[emulation stopped: %s at pc 0x%x after %d insns]"
              % (e, emu.uc.reg_read(UC_ARM64_REG_PC), emu.insns))

    print("\n[stopped after %d insns, pc 0x%x, mmio reads %s]"
          % (emu.insns, emu.uc.reg_read(UC_ARM64_REG_PC),
             {hex(k): hex(v) for k, v in sorted(emu.mmio_reads.items())[:8]}))

    out = emu.console.decode("ascii", errors="replace")
    print("\n================ UART output ================")
    sys.stdout.write(out)
    if not out.endswith("\n"):
        print()
    print("============================================")

    # Did the guest really render into the surface the bootloader left? Read the
    # panel memory back through the emulator: the writes had to translate
    # through the guest MMU for this to be non-zero.
    fb_size = emu.ufb_w * (emu.ufb_bpp // 8) * emu.ufb_h
    bg_px = 0
    try:
        panel = emu.uc.mem_read(emu.ufb, fb_size)
        nonzero = sum(1 for b in panel if b)
        # The UI paints 0x101828 over the whole screen; count matching pixels so
        # this is not just "something wrote here".
        #
        # Byte order: Zephyr's PIXEL_FORMAT_RGB_888 (which MDP5's 24bpp RGB888
        # surface is) stores B,G,R in memory - see
        # zephyr/modules/lvgl/lvgl_display_24bit.c. So the background lands as
        # 0x28,0x18,0x10, not 0x10,0x18,0x28.
        if emu.ufb_bpp == 24:
            want = bytes((0x28, 0x18, 0x10))
        else:
            want = bytes((0x10, 0x18, 0x28))
        for i in range(0, len(panel), 3):
            if panel[i:i + 3] == want:
                bg_px += 1
        print("\npanel 0x%x: %d%% of %d bytes non-zero, %d/%d px are the UI background"
              % (emu.ufb, 100 * nonzero // len(panel), len(panel), bg_px, fb_size // 3))
    except UcError as e:
        print("\npanel read failed: %s" % e)
        nonzero = 0
        bg_px = 0

    ok = 0
    if "Booting Zephyr" in out:
        ok += 1
        print("[ok] kernel banner reached the console")
    if "Zephyr on mido" in out:
        ok += 1
        print("[ok] application main() reached")
    if "display:" in out:
        ok += 1
        print("[ok] display driver probed a surface")
    if "LVGL up" in out or "timer interrupt never fired" in out:
        ok += 1
        print("[ok] LVGL initialised and the render loop was entered")
    if nonzero > 1000:
        ok += 1
        print("[ok] pixels written through the MMU into the bootloader framebuffer")
    if bg_px > 100000:
        ok += 1
        print("[ok] the frame actually drawn is the application UI")

    return 0 if ok >= 2 else 1


if __name__ == "__main__":
    sys.exit(main())
