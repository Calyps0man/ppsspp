#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project. GPL-2.0-or-later.
"""Generate synthetic encrypted PRXs with Python cryptography, independently of libkirk.

No game content. The committed fixtures can be tested without Python dependencies.
"""
from pathlib import Path
import hashlib
import re
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.cmac import CMAC

repo = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent / 'prx-fixtures'
out.mkdir(exist_ok=True)
prx_source = (repo / 'Core/ELF/PrxDecrypter.cpp').read_text()
kirk_source = (repo / 'ext/libkirk/kirk_engine.c').read_text()

def array(source, name):
    text = re.search(r'\b' + name + r'\[.*?\]\s*=\s*\{(.*?)\};', source, re.S)[1]
    return [int(x, 16) for x in re.findall(r'0x([\da-fA-F]+)', text)]

vault = bytes(array(kirk_source, 'keyvault'))
kirk1 = bytes(array(kirk_source, 'kirk1_key'))

def aes(key, data, decrypt=False):
    cipher = Cipher(algorithms.AES(key), modes.CBC(bytes(16)))
    op = cipher.decryptor() if decrypt else cipher.encryptor()
    return op.update(data) + op.finalize()

def xor(a, b):
    assert len(a) == len(b)
    return bytes(x ^ y for x, y in zip(a, b))

def cmac(key, data):
    c = CMAC(algorithms.AES(key))
    c.update(data)
    return c.finalize()

payload = b'\x7fELF' + bytes(range(4, 64))
(out / 'expected.bin').write_bytes(payload)

def cmd1(prx):
    header = bytearray(0x90)
    aes_key = bytes(range(16))
    mac_key = bytes(range(16, 32))
    struct.pack_into('<I', header, 0x60, 1)
    struct.pack_into('<II', header, 0x70, len(payload), 0x80)
    data = aes(aes_key, payload)
    header[0x20:0x30] = cmac(mac_key, header[0x60:0x90])
    header[0x30:0x40] = cmac(mac_key, header[0x60:0x90] + prx[:0x80] + data)
    header[:0x20] = aes(kirk1, aes_key + mac_key)
    return header, data

def base(tag):
    p = bytearray(0x150 + len(payload))
    p[:4] = b'~PSP'
    p[0x0a:0x12] = b'fixture\0'
    struct.pack_into('<II', p, 0x28, len(payload), len(p))
    struct.pack_into('<I', p, 0xd0, tag)
    return p

# Legacy EBOOT key format: numeric key words serialized little-endian.
p = base(0x08000000)
h, data = cmd1(p)
seed = b''.join(struct.pack('<I', x) for x in array(prx_source, 'g_keyEBOOT1xx'))
key = vault[0x4b * 16:0x4c * 16]
block = xor(aes(key, xor(h[:0x70], seed[0x20:0x90])), seed[0x14:0x84]) + h[0x70:]
p[0x110:0x150] = block[:0x40]
p[0x80:0xd0] = block[0x40:]
p[0xd4:0xe8] = hashlib.sha1(seed[:0x14] + p[0xe8:0x110] + block + p[:0x80]).digest()
p[0x150:] = data
(out / 'type0.prx').write_bytes(p)

# Newer type 2: byte key, expanded seed, and authenticated little-endian CMD1 header.
p = base(0x0b2b90f0)
h, data = cmd1(p)
key = vault[0x5c * 16:0x5d * 16]
seed_key = bytes(array(prx_source, 'keys_9DC14891_1'))
expanded = b''.join(bytes([i]) + seed_key[1:] for i in range(9))
seed = aes(key, expanded, decrypt=True)
kh = xor(aes(key, xor(h[:0x40], seed[0x50:0x90])), seed[0x10:0x50])
metadata = bytes(h[0x70:0x80])
identity = bytes(range(0x10))
digest = hashlib.sha1(p[0xd0:0xd4] + seed[:0x10] + bytes(0x58) + identity + kh + metadata + p[:0x80]).digest()
packed = identity + digest + kh
packed = aes(key, packed[:0x60]) + packed[0x60:]
p[0x140:0x150] = packed[:0x10]
p[0x12c:0x140] = packed[0x10:0x24]
p[0x80:0xb0] = packed[0x24:0x54]
p[0xc0:0xd0] = packed[0x54:0x64]
p[0xb0:0xc0] = metadata
p[0x150:] = data
(out / 'type2.prx').write_bytes(p)
print('Generated type0/type2 synthetic PRXs and expected plaintext')
