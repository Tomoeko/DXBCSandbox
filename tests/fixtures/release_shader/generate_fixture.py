# SPDX-License-Identifier: GPL-3.0-only
"""Author a minimal Class48 v22 object for capture/evidence boundary tests.

This is synthetic wire data, not an extracted Unity asset. It has no subshaders,
programs, properties, dependencies or external assets and cannot render. The
pinned registry supplies its TypeTree. Actual compiled release checks are
separate, argument-driven tests of the same producer.
"""
from pathlib import Path
import struct


def u32(value):
    return struct.pack('<I', value)


def align(data, boundary=4):
    data.extend(bytes(-len(data) % boundary))


def string(data, value):
    data += u32(len(value)) + value
    align(data)


payload = bytearray()
name = b'Experiment/EmptyRelease'
string(payload, name)
payload += bytes(16)  # Properties, subshaders, keyword names/flags.
string(payload, name)
payload += bytes(16)  # Custom editor, fallback, dependencies, SRP editors.
payload += b'\0'  # DisableNoSubshadersMessage.
align(payload)
payload += bytes(32)  # Six archive arrays, dependencies, immutable textures.
payload += b'\0'  # ShaderIsBaked.
align(payload)

metadata = bytearray(b'2021.3.35f1\0' + u32(19) + b'\0' + u32(1))
metadata += struct.pack('<iBH', 48, 0, 0xffff)
metadata += bytes.fromhex('f0c184272a05e54447e36366ec4876a6')
metadata += u32(1)
align(metadata)
metadata += struct.pack('<qQIi', 7, 0, len(payload), 0)
metadata += bytes(12) + b'\0'
metadata_size = len(metadata)
align(metadata, 16)
data_offset = 48 + len(metadata)
header = struct.pack('>IIII', 0, 0, 22, 0) + bytes(4)
header += struct.pack('>IQQQ', metadata_size, data_offset + len(payload), data_offset, 0)
Path(__file__).with_name('empty.assets').write_bytes(header + metadata + payload)
