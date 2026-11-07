# SPDX-License-Identifier: Apache-2.0

import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile


STOCK_SHA256 = '205b9ea8d3a12293e360c544af8f8e64afd07e488a515c0533c36238d7ebf19e'
SITES = {0x12e94: bytes.fromhex('402740b9'), 0x12f34: bytes.fromhex('432740b9')}
CAVE = 0x23c40
LIMIT = 0x24000


def patch_gpu(data):
    asm = Path(__file__).with_name('gpu_nv12_layout.S')
    with tempfile.TemporaryDirectory() as tmp:
        obj, raw = Path(tmp) / 'layout.o', Path(tmp) / 'layout.bin'
        subprocess.run(['llvm-mc', '-triple=aarch64', '-filetype=obj',
                        str(asm), '-o', str(obj)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text',
                        str(obj), str(raw)], check=True)
        payload = raw.read_bytes()

    assert CAVE < len(payload) <= LIMIT
    original = bytearray(data)
    phoff = struct.unpack_from('<Q', original, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', original, 54)
    rx = None
    for index in range(phcount):
        offset = phoff + index * phsize
        kind, flags = struct.unpack_from('<II', original, offset)
        if kind == 1 and flags == 5:
            assert rx is None
            rx = offset
    assert rx is not None
    assert struct.unpack_from('<QQ', original, rx + 8) == (0xf000, 0xf000)
    old_size = CAVE - 0xf000
    new_size = len(payload) - 0xf000
    sizes = struct.unpack_from('<QQ', original, rx + 32)
    if sizes == (new_size, new_size):
        assert original[CAVE:len(payload)] == payload[CAVE:]
        for site in SITES:
            assert original[site:site + 4] == payload[site:site + 4]
        original[CAVE:len(payload)] = bytes(len(payload) - CAVE)
        struct.pack_into('<QQ', original, rx + 32, old_size, old_size)
        for site, instruction in SITES.items():
            original[site:site + 4] = instruction
    assert hashlib.sha256(original).hexdigest() == STOCK_SHA256
    assert not any(original[CAVE:LIMIT])

    for site, instruction in SITES.items():
        assert original[site:site + 4] == instruction
        original[site:site + 4] = payload[site:site + 4]
    original[CAVE:len(payload)] = payload[CAVE:]
    struct.pack_into('<QQ', original, rx + 32, new_size, new_size)
    return bytes(original)


def fix_gpu_layout(_ctx, _file, file_path, *args, **kwargs):
    path = Path(file_path)
    path.write_bytes(patch_gpu(path.read_bytes()))
