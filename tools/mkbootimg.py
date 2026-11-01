#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Build an Android boot image that lk2nd accepts as if it were a Linux kernel.

Two things make this img not-quite-a-Linux-image:

  * text_offset. lk2nd on msm8953 is built with ABOOT_IGNORE_BOOT_HEADER_ADDRS,
    so the load address in the boot header is thrown away and recomputed as
    "(DDR_START + 0x80000) & ~0x1fffff) + text_offset" - that is, the 2 MiB
    aligned base of RAM plus text_offset from the ARM64 image header. A Zephyr
    image carries text_offset = 0, which would land it at 0x80000000 while it is
    linked for 0x80080000. So we patch text_offset in the image header to
    0x80000. text_offset is informational for Zephyr, it does not use it.

  * the appended devicetree. lk2nd's dev_tree_appended() reads a 32-bit offset
    from the kernel at offset 0x2C, then expects a flattened device tree at
    kernel + that offset. Zephyr's image header has 0 there. So we append the
    DTB, 8 byte aligned, and store the offset at 0x2C.

Everything else is a plain Android boot image v0, page size 2048, no ramdisk.
"""

import argparse
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
BOOT_NAME_SIZE = 16
BOOT_ARGS_SIZE = 512
BOOT_EXTRA_ARGS_SIZE = 1024

TEXT_OFFSET = 0x80000        # ARM64 Image header field at 0x08
TEXT_OFFSET_FIELD = 0x08
DTB_OFFSET_FIELD = 0x2C      # lk2nd platform/msm_shared/include/dev_tree.h
KERNEL64_MAGIC_FIELD = 0x38
KERNEL64_MAGIC = 0x644D5241  # "ARM\x64"

FDT_MAGIC = 0xD00DFEED


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def check_kernel_header(image):
    if len(image) < 0x40:
        sys.exit("error: kernel image is smaller than the ARM64 header")
    magic = struct.unpack_from("<I", image, KERNEL64_MAGIC_FIELD)[0]
    if magic != KERNEL64_MAGIC:
        sys.exit("error: kernel does not carry the ARM64 Image magic at 0x38 "
                 "(0x%08x) - is this a raw Zephyr arm64 build?" % magic)


def check_dtb(dtb):
    magic = struct.unpack_from(">I", dtb, 0)[0]
    if magic != FDT_MAGIC:
        sys.exit("error: dtb does not start with the FDT magic")
    totalsize = struct.unpack_from(">I", dtb, 4)[0]
    if totalsize > len(dtb):
        sys.exit("error: dtb is truncated (header says %d bytes)" % totalsize)


def build(kernel, dtb, cmdline, page_size, kernel_addr, tags_addr, ramdisk_addr,
          second_addr, name):
    image = bytearray(kernel)
    check_kernel_header(image)
    check_dtb(dtb)

    struct.pack_into("<Q", image, TEXT_OFFSET_FIELD, TEXT_OFFSET)

    dtb_offset = align(len(image), 8)
    image.extend(b"\x00" * (dtb_offset - len(image)))
    struct.pack_into("<I", image, DTB_OFFSET_FIELD, dtb_offset)
    image.extend(dtb)

    kernel_size = len(image)

    header = bytearray()
    header += BOOT_MAGIC
    header += struct.pack("<I", kernel_size)
    header += struct.pack("<I", kernel_addr)
    header += struct.pack("<I", 0)                 # ramdisk_size
    header += struct.pack("<I", ramdisk_addr)
    header += struct.pack("<I", 0)                 # second_size
    header += struct.pack("<I", second_addr)
    header += struct.pack("<I", tags_addr)
    header += struct.pack("<I", page_size)
    header += struct.pack("<I", 0)                 # header_version
    header += struct.pack("<I", 0)                 # os_version
    header += name.encode("ascii")[:BOOT_NAME_SIZE].ljust(BOOT_NAME_SIZE, b"\x00")
    header += cmdline.encode("ascii")[:BOOT_ARGS_SIZE - 1].ljust(BOOT_ARGS_SIZE, b"\x00")
    header += b"\x00" * 32                         # id
    header += b"\x00" * BOOT_EXTRA_ARGS_SIZE

    out = header
    out += b"\x00" * (align(len(out), page_size) - len(out))
    out += image
    out += b"\x00" * (align(len(out), page_size) - len(out))

    return bytes(out), kernel_size


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", required=True, help="zephyr.bin")
    ap.add_argument("--dtb", required=True, help="device tree to append (e.g. msm8953-xiaomi-mido.dtb)")
    ap.add_argument("--cmdline", default="lk2nd.pass-simplefb=autorefresh")
    ap.add_argument("--output", "-o", default="boot.img")
    ap.add_argument("--page-size", type=lambda s: int(s, 0), default=2048)
    # lk2nd ignores these on msm8953, but keep them meaningful anyway.
    ap.add_argument("--kernel-addr", type=lambda s: int(s, 0), default=0x80080000)
    ap.add_argument("--tags-addr", type=lambda s: int(s, 0), default=0x83400000)
    ap.add_argument("--ramdisk-addr", type=lambda s: int(s, 0), default=0x83600000)
    ap.add_argument("--second-addr", type=lambda s: int(s, 0), default=0x80F00000)
    ap.add_argument("--name", default="zephyr-mido")
    args = ap.parse_args()

    kernel = open(args.kernel, "rb").read()
    dtb = open(args.dtb, "rb").read()

    blob, kernel_size = build(kernel, dtb, args.cmdline, args.page_size,
                              args.kernel_addr, args.tags_addr, args.ramdisk_addr,
                              args.second_addr, args.name)

    with open(args.output, "wb") as f:
        f.write(blob)

    print("wrote %s: %d bytes (kernel+dtb %d, dtb offset 0x%x, page %d)"
          % (args.output, len(blob), kernel_size,
             struct.unpack_from("<I", blob, args.page_size + DTB_OFFSET_FIELD)[0],
             args.page_size))
    print("cmdline: %s" % args.cmdline)


if __name__ == "__main__":
    main()
