# SPDX-License-Identifier: GPL-3.0-only
"""Write synthetic v22 metadata for Class141/Class30 integration tests.

This is authored wire data, not an extracted Unity player. Empty dependency
arrays and null PPtrs deliberately keep this fixture independent of assets.
Real player captures remain a separate execution check.
"""
from pathlib import Path
import struct


def u32(value):
    return struct.pack('<I', value)


def align(data, boundary=4):
    data.extend(bytes(-len(data) % boundary))


version = b'2021.3.35f1'
build = bytearray(4 * 4 + 16 + 15)  # Empty string arrays, GUID, flags.
align(build)
build += u32(len(version)) + version
align(build)
build += u32(1) + u32(2)  # Only GraphicsDeviceType.Direct3D11.

caps = bytearray(8 * 16)  # Builtin modes and null PPtr<Shader> values.
caps += bytes(4 * 4 + 2 * 12 + 4 * 4)  # Video, arrays, materials, sort.
caps += bytes(3 * 16)  # Three serialized tier settings.
caps += u32(1) + u32(4)  # One ShaderCompilerPlatform: D3D11.
for _ in range(3):
    caps += u32(2) + u32(147179016) + u32(0)  # fixed_bitset<33>.
caps += bytes(4 + 4 + 4 + 4 + 2)  # Booleans, mask, empty SRP map.

metadata = bytearray(version + b'\0' + u32(19) + b'\0' + u32(2))
for class_id, type_hash in (
    (141, '575bf9902bd61f97e962a3a175b5c67a'),
    (30, '9b60e765e21d199e3eaef5540018e8da'),
):
    metadata += struct.pack('<iBH', class_id, 0, 0xffff) + bytes.fromhex(type_hash)
metadata += u32(2)
# Header is 48 bytes, so metadata alignment equals file alignment.
for index, (offset, payload) in enumerate(((0, build), (len(build), caps))):
    align(metadata)
    metadata += struct.pack('<qQIi', index + 1, offset, len(payload), index)
metadata += bytes(12) + b'\0'  # Scripts, externals, ref types, user data.
metadata_size = len(metadata)
align(metadata, 16)
data_offset = 48 + len(metadata)
file_size = data_offset + len(build) + len(caps)
header = struct.pack('>IIII', 0, 0, 22, 0) + bytes(4)
header += struct.pack('>IQQQ', metadata_size, file_size, data_offset, 0)
Path(__file__).with_name('metadata.assets').write_bytes(header + metadata + build + caps)
