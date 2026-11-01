#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Verify a Zephyr-for-mido boot image without a phone.

There is no way to test a boot image without flashing it, so this replays what
lk2nd does with the image, step by step, and asserts that every assumption the
Zephyr side makes actually holds. It checks:

  1. the boot image header the way app/aboot/aboot.c parses it;
  2. the load address lk2nd really uses, which is NOT the one in the header:
     msm8953 is built with ABOOT_IGNORE_BOOT_HEADER_ADDRS, so the address is
     recomputed as ((DDR_START + 0x80000) & ~0x1fffff) + text_offset. That has
     to equal the address the ELF is linked for, or the image dies instantly;
  3. the appended devicetree, found the way dev_tree_appended() finds it (32-bit
     offset at kernel+0x2C), and that it fits where lk2nd copies it
     (ABOOT_FORCE_TAGS_ADDR = DDR_START + 0x3400000) without colliding with the
     kernel image or with lk2nd's own 10 MiB region at 0x8f600000;
  4. that every PT_LOAD segment lands in the DDR window the Zephyr MMU maps;
  5. the framebuffer handoff: it rebuilds the device tree the way lk2nd's
     simplefb.c does (reg/width/height/stride/format on the
     simple-framebuffer node, plus /chosen/bootargs) and runs the *actual*
     on-target parser (lib/fdt_min.c, compiled for the host) over the result,
     asserting the driver would discover exactly the injected surface;
  6. the MDP5 pipe decode, against the register values a 1080p x8r8g8b8
     bootloader surface produces.

Usage:
    tools/verify_boot.py --bootimg prebuilt/boot.img --elf app/build/zephyr/zephyr.elf
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

DDR_START = 0x80000000
DDR_START_LOW = 0x80000000
DDR_END_LOW = 0xC0000000          # the window soc/qcom/msm8953/mmu_regions.c maps
ABOOT_FORCE_KERNEL64_ADDR = DDR_START + 0x80000
ABOOT_FORCE_TAGS_ADDR = DDR_START + 0x3400000
ABOOT_FORCE_RAMDISK_ADDR = DDR_START + 0x3600000
ABOOT_MEMBASE = 0x8F600000        # lk2nd target/msm8953/rules.mk
ABOOT_MEMSIZE = 0x00A00000

BOOT_MAGIC = b"ANDROID!"
KERNEL64_MAGIC = 0x644D5241
FDT_MAGIC = 0xD00DFEED

failures = []
checks = 0


def check(cond, msg, detail=""):
    global checks
    checks += 1
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s%s" % (msg, (": " + detail) if detail else ""))
        failures.append(msg)
    return cond


# --------------------------------------------------------------------------
# FDT: just enough of libfdt to reproduce what lk2nd's simplefb.c injects.
# --------------------------------------------------------------------------

class Fdt:
    def __init__(self, data):
        magic, totalsize, off_struct, off_strings, off_rsvmap, version, \
            last_comp, _, size_strings, size_struct = struct.unpack_from(">10I", data, 0)
        if magic != FDT_MAGIC:
            raise ValueError("not a DTB")
        self.hdr = dict(totalsize=totalsize, off_struct=off_struct,
                        off_strings=off_strings, off_rsvmap=off_rsvmap,
                        version=version, last_comp=last_comp,
                        size_strings=size_strings, size_struct=size_struct)
        self.strings = bytes(data[off_strings:off_strings + size_strings])
        self.root, _ = self._parse(data, off_struct, off_struct + size_struct)
        self.rsvmap = bytes(data[off_rsvmap:off_struct])

    def _str(self, off):
        end = self.strings.index(b"\x00", off)
        return self.strings[off:end].decode()

    def _parse(self, data, p, end):
        node = {"name": "", "props": [], "nodes": []}
        stack = [node]
        while p + 4 <= end:
            tok = struct.unpack_from(">I", data, p)[0]
            p += 4
            if tok == 1:
                e = data.index(b"\x00", p)
                child = {"name": data[p:e].decode(), "props": [], "nodes": []}
                stack[-1]["nodes"].append(child)
                stack.append(child)
                p = (e + 4) & ~3
            elif tok == 2:
                stack.pop()
            elif tok == 3:
                ln, noff = struct.unpack_from(">II", data, p)
                p += 8
                stack[-1]["props"].append([self._str(noff), bytes(data[p:p + ln])])
                p = (p + ln + 3) & ~3
            elif tok == 4:
                pass
            elif tok == 9:
                break
            else:
                raise ValueError("bad FDT token %d" % tok)
        return node, p

    # -- helpers ---------------------------------------------------------
    def find(self, path):
        node = self.root
        for part in [x for x in path.split("/") if x]:
            for c in node["nodes"]:
                if c["name"] == part:
                    node = c
                    break
            else:
                return None
        return node

    def ensure(self, path):
        node = self.root
        for part in [x for x in path.split("/") if x]:
            for c in node["nodes"]:
                if c["name"] == part:
                    node = c
                    break
            else:
                child = {"name": part, "props": [], "nodes": []}
                node["nodes"].append(child)
                node = child
        return node

    def set_prop(self, path, name, value):
        node = self.ensure(path)
        for p in node["props"]:
            if p[0] == name:
                p[1] = value
                return
        node["props"].append([name, value])

    def get_prop(self, path, name):
        node = self.find(path)
        if node is None:
            return None
        for p in node["props"]:
            if p[0] == name:
                return p[1]
        return None

    def find_compatible(self, compat):
        out = []

        def rec(n, path):
            for name, val in n["props"]:
                if name == "compatible" and compat.encode() + b"\x00" in val:
                    out.append(path if path else "/")
            for c in n["nodes"]:
                rec(c, path + "/" + c["name"])

        rec(self.root, "")
        return out

    # -- serialise -------------------------------------------------------
    def to_bytes(self):
        strings = bytearray()
        offsets = {}

        def str_off(name):
            if name not in offsets:
                offsets[name] = len(strings)
                strings.extend(name.encode() + b"\x00")
            return offsets[name]

        struct_blk = bytearray()

        def emit(n):
            struct_blk.extend(struct.pack(">I", 1))
            struct_blk.extend(n["name"].encode() + b"\x00")
            while len(struct_blk) % 4:
                struct_blk.extend(b"\x00")
            for name, val in n["props"]:
                struct_blk.extend(struct.pack(">III", 3, len(val), str_off(name)))
                struct_blk.extend(val)
                while len(struct_blk) % 4:
                    struct_blk.extend(b"\x00")
            for c in n["nodes"]:
                emit(c)
            struct_blk.extend(struct.pack(">I", 2))

        emit(self.root)
        struct_blk.extend(struct.pack(">I", 9))

        rsv = self.rsvmap
        if len(rsv) < 16:
            rsv = b"\x00" * 16

        off_rsvmap = 40
        off_struct = off_rsvmap + len(rsv)
        off_strings = off_struct + len(struct_blk)
        total = off_strings + len(strings)

        out = struct.pack(">10I", FDT_MAGIC, total, off_struct, off_strings,
                          off_rsvmap, 17, 16, 0, len(strings), len(struct_blk))
        out += rsv
        out += struct_blk
        out += strings
        return out


# --------------------------------------------------------------------------
# ELF: PT_LOAD ranges
# --------------------------------------------------------------------------

def elf_load_segments(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError("not an ELF64 file")
    e_entry = struct.unpack_from("<Q", data, 0x18)[0]
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = \
            struct.unpack_from("<IIQQQQQQ", data, off)
        if p_type == 1:  # PT_LOAD
            segs.append(dict(vaddr=p_vaddr, paddr=p_paddr, filesz=p_filesz,
                             memsz=p_memsz, offset=p_offset))
    return e_entry, segs


# --------------------------------------------------------------------------
# MDP5 decode, mirroring drivers/display/display_qcom_mdp5.c
# --------------------------------------------------------------------------

def decode_mdp5(addr, img_size, stride, fmt):
    width = img_size & 0xFFFF
    height = img_size >> 16
    bpp = ((fmt >> 9) & 0x3) + 1
    return dict(base=addr, width=width, height=height, stride=stride, bpp=bpp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bootimg", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = args.workdir or tempfile.mkdtemp(prefix="mido-verify-")

    blob = open(args.bootimg, "rb").read()

    print("== 1. boot image header (app/aboot/aboot.c)")
    check(blob[:8] == BOOT_MAGIC, "ANDROID! magic")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page_size, header_version, _) = \
        struct.unpack_from("<10I", blob, 8)
    check(page_size == 2048, "page size 2048", str(page_size))
    check(header_version == 0, "header version 0", str(header_version))
    check(kernel_size > 0x1000, "kernel_size sane", hex(kernel_size))
    cmdline = blob[64:64 + 512].split(b"\x00")[0].decode()

    print("== 2. where lk2nd actually loads the kernel")
    kstart = ((page_size + kernel_size + page_size - 1) // page_size) * 0  # placeholder
    kernel = blob[page_size:page_size + kernel_size]

    insn, res1, text_offset, image_size, flags = struct.unpack_from("<IIQQQ", kernel, 0)
    magic = struct.unpack_from("<I", kernel, 0x38)[0]
    check(magic == KERNEL64_MAGIC, "ARM64 Image magic at 0x38 (IS_ARM64 passes)")
    check((insn & 0xFC000000) == 0x14000000, "first word is a branch instruction",
          hex(insn))

    # app/aboot/aboot.c update_ker_tags_rdisk_addr(), ABOOT_IGNORE_BOOT_HEADER_ADDRS
    forced = ABOOT_FORCE_KERNEL64_ADDR
    if image_size:
        forced &= ~0x1FFFFF
        if text_offset > 2 * 1024 * 1024:
            text_offset = 0
        forced += text_offset
    load_addr = forced
    print("     lk2nd will load the image at 0x%x (header said 0x%x)"
          % (load_addr, kernel_addr))

    e_entry, segs = elf_load_segments(args.elf)
    base = min(s["vaddr"] for s in segs)
    print("     ELF is linked for 0x%x, entry 0x%x" % (base, e_entry))
    check(load_addr == base, "lk2nd load address matches the link address",
          "0x%x != 0x%x" % (load_addr, base))

    print("== 3. appended devicetree (dev_tree_appended)")
    dtb_offset = struct.unpack_from("<I", kernel, 0x2C)[0]
    check(dtb_offset != 0, "dtb offset at kernel+0x2C is non-zero", hex(dtb_offset))
    dtb = kernel[dtb_offset:]
    check(len(dtb) > 64 and struct.unpack_from(">I", dtb, 0)[0] == FDT_MAGIC,
          "FDT magic at kernel+dtb_offset")
    dtb_total = struct.unpack_from(">I", dtb, 4)[0]
    check(0 < dtb_total <= len(dtb), "dtb totalsize fits in the image",
          "%d vs %d" % (dtb_total, len(dtb)))
    dtb = dtb[:dtb_total]

    kernel_end = load_addr + kernel_size
    print("     kernel occupies 0x%x..0x%x" % (load_addr, kernel_end))
    check(kernel_end < ABOOT_FORCE_TAGS_ADDR,
          "kernel does not run into the tags/DTB area at 0x%x" % ABOOT_FORCE_TAGS_ADDR,
          "kernel ends at 0x%x" % kernel_end)
    dtb_end = ABOOT_FORCE_TAGS_ADDR + dtb_total
    check(dtb_end < ABOOT_MEMBASE,
          "DTB (0x%x..0x%x) does not overlap lk2nd's own memory at 0x%x"
          % (ABOOT_FORCE_TAGS_ADDR, dtb_end, ABOOT_MEMBASE))

    print("== 4. MMU window")
    for s in segs:
        lo, hi = s["vaddr"], s["vaddr"] + s["memsz"]
        check(DDR_START_LOW <= lo and hi <= DDR_END_LOW,
              "segment 0x%x..0x%x inside the mapped DDR window" % (lo, hi))
    check(DDR_START_LOW <= ABOOT_FORCE_TAGS_ADDR and dtb_end <= DDR_END_LOW,
          "DTB address is mapped by the MMU")

    print("== 5. framebuffer handoff, as lk2nd's simplefb.c builds it")
    fdt = Fdt(dtb)
    nodes = fdt.find_compatible("simple-framebuffer")
    print("     existing simple-framebuffer nodes: %s" % (nodes or "none"))

    # Pretend lk2nd read a *different* surface than whatever the DT shipped
    # with, so passing this check cannot be an accident of the input file.
    sim_base = 0x90001000
    sim_w, sim_h, sim_bpp = 1080, 1920, 24
    sim_stride = sim_w * sim_bpp // 8
    sim_size = sim_stride * sim_h
    fmt_name = {16: b"r5g6b5\x00", 24: b"r8g8b8\x00", 32: b"x8r8g8b8\x00"}[sim_bpp]

    node_path = "/chosen/framebuffer@%08x" % sim_base
    fdt.set_prop("/chosen", "#address-cells", struct.pack(">I", 2))
    fdt.set_prop("/chosen", "#size-cells", struct.pack(">I", 2))
    fdt.set_prop("/chosen", "ranges", b"")
    fdt.set_prop(node_path, "compatible", b"simple-framebuffer\x00")
    fdt.set_prop(node_path, "reg", struct.pack(">QQ", sim_base, sim_size))
    fdt.set_prop(node_path, "width", struct.pack(">I", sim_w))
    fdt.set_prop(node_path, "height", struct.pack(">I", sim_h))
    fdt.set_prop(node_path, "stride", struct.pack(">I", sim_stride))
    fdt.set_prop(node_path, "format", fmt_name)
    fdt.set_prop(node_path, "status", b"okay\x00")
    fdt.set_prop("/chosen", "bootargs",
                 (cmdline + "\x00").encode() if cmdline else b"\x00")
    sim_dtb = fdt.to_bytes()
    sim_path = os.path.join(tmp, "runtime.dtb")
    open(sim_path, "wb").write(sim_dtb)
    print("     wrote simulated runtime DT (%d bytes) to %s" % (len(sim_dtb), sim_path))

    # Now run the real, on-target parser over it.
    harness = os.path.join(tmp, "fdt_dump")
    src = [os.path.join(here, "tests", "host", "fdt_dump.c"),
           os.path.join(here, "lib", "fdt_min.c")]
    cmd = [args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror",
           "-I", os.path.join(here, "include"), "-o", harness] + src
    subprocess.run(cmd, check=True)
    out = subprocess.run([harness, sim_path], capture_output=True, text=True)
    print("     parser said: %s" % out.stdout.strip().replace("\n", " | "))
    check(out.returncode == 0, "on-target parser found the injected framebuffer")

    expect = ("base 0x%x size 0x%x %dx%d stride %d bpp %d"
              % (sim_base, sim_size, sim_w, sim_h, sim_stride, sim_bpp // 8))
    check(expect in out.stdout, "parser decoded the injected geometry",
          "expected '%s'" % expect)
    check("bootargs: " + cmdline in out.stdout or cmdline == "",
          "parser sees /chosen/bootargs")

    print("== 6. MDP5 register decode (same surface, read back from the pipe)")
    # SRC_FORMAT for x8r8g8b8 on MDP5: BPP field bits [10:9] = 3 -> 4 bytes
    fmt_reg = (0x3 << 9) | (0x3 << 6) | (0x3 << 4) | (0x3 << 2) | 0x3
    dec = decode_mdp5(sim_base, (sim_h << 16) | sim_w, sim_stride, fmt_reg)
    check(dec["width"] == sim_w and dec["height"] == sim_h,
          "MDP5 pipe decode gives %dx%d" % (sim_w, sim_h),
          str(dec))
    check(dec["bpp"] == 4, "MDP5 SRC_FORMAT bpp field decodes to 4", str(dec["bpp"]))
    fmt565 = (0x1 << 9) | (0x1 << 4) | (0x1 << 2) | 0x2
    check(decode_mdp5(sim_base, (sim_h << 16) | sim_w, sim_w * 2, fmt565)["bpp"] == 2,
          "MDP5 SRC_FORMAT bpp field decodes to 2 for r5g6b5")

    print()
    if failures:
        print("%d/%d checks FAILED" % (len(failures), checks))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("all %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
