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


def fixture_bytes(name):
    """Build one empty, non-renderable object with the supplied UTF-8 name."""
    name = name.encode('utf-8')
    payload = bytearray()
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
    return header + metadata + payload


if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='Experiment/EmptyRelease')
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('empty.assets'))
    arguments = parser.parse_args()
    if not arguments.name or '\0' in arguments.name:
        parser.error('name must be nonempty and contain no NUL')
    arguments.output.write_bytes(fixture_bytes(arguments.name))
