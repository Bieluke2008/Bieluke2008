#!/usr/bin/env python3
"""Writes control.nacp for the installable NSP build (libnx's NacpStruct).

usage: make-nacp.py <title_id hex> <version> <out.nacp>

Unlike nacptool's homebrew NACP, this one carries the title id where the
system looks for it, above all LocalCommunicationId: the console's local
wireless (LDN) only lets an application use the ids listed there.
"""
import struct
import sys

tid = int(sys.argv[1], 16)
version = sys.argv[2]
out = sys.argv[3]

nacp = bytearray(0x4000)
name = b"OpenFront"
publisher = b"OpenFront port (homebrew)"
for lang in range(16):  # NacpLanguageEntry: name[0x200], author[0x100]
    off = lang * 0x300
    nacp[off : off + len(name)] = name
    nacp[off + 0x200 : off + 0x200 + len(publisher)] = publisher
nacp[0x3025] = 0  # StartupUserAccount: none
struct.pack_into("<I", nacp, 0x302C, 0xFFFF)  # SupportedLanguageFlag: the 16 entries above
nacp[0x3034] = 0  # Screenshot: allowed
nacp[0x3035] = 2  # VideoCapture: enabled
struct.pack_into("<Q", nacp, 0x3038, tid)  # PresenceGroupId
ver = version.encode()[:15]
nacp[0x3060 : 0x3060 + len(ver)] = ver  # DisplayVersion
struct.pack_into("<Q", nacp, 0x3070, tid + 0x1000)  # AddOnContentBaseId
struct.pack_into("<Q", nacp, 0x3078, tid)  # SaveDataOwnerId
for i in range(8):  # LocalCommunicationId
    struct.pack_into("<Q", nacp, 0x30B0 + 8 * i, tid)
nacp[0x30F0] = 2  # LogoType: Nintendo
nacp[0x30F1] = 0  # LogoHandling: auto
struct.pack_into("<Q", nacp, 0x30F8, tid)  # SeedForPseudoDeviceId
with open(out, "wb") as f:
    f.write(nacp)
