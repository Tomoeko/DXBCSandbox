// SPDX-License-Identifier: GPL-3.0-only

#include "hlsl_source_identifier.h"
#include "common/source_scan.h"
#include <string.h>

bool hlsl_source_identifier_valid(const char *name) {
    static const char *const reserved[] = {
        "asm", "asm_fragment", "auto", "break", "case", "catch", "cbuffer", "char", "class",
        "column_major", "compile", "compile_fragment", "const", "const_cast", "continue",
        "default", "delete", "discard", "do", "dynamic_cast", "else", "enum", "extern",
        "false", "for", "friend", "fxgroup", "goto", "groupshared", "if", "in", "inline",
        "inout", "interface", "literal", "long", "namespace", "new", "nointerpolation",
        "noperspective", "out", "packoffset", "pass", "pixelfragment", "precise", "private",
        "protected", "public", "register", "reinterpret_cast", "return", "row_major", "sample",
        "sampler", "shared", "short", "signed", "sizeof", "smooth", "static", "static_cast",
        "string", "struct", "switch", "tbuffer", "technique", "technique10", "technique11",
        "template", "this", "throw", "true", "try", "typedef", "typename", "uniform", "union",
        "unsigned", "using", "vector", "vertexfragment", "virtual", "void", "volatile", "while",
        "matrix", "snorm", "unorm", "point", "line", "triangle", "lineadj", "triangleadj",
        "centroid", "linear", "export", "stateblock", "stateblock_state",
        "InputPatch", "OutputPatch", "PointStream", "LineStream", "TriangleStream",
        "defined", "numthreads", "Buffer", "RWBuffer", "StructuredBuffer",
        "RWStructuredBuffer", "ByteAddressBuffer", "RWByteAddressBuffer", "AppendStructuredBuffer",
        "ConsumeStructuredBuffer", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray",
        "Texture2DMS", "Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray",
        "RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D",
        "SamplerState", "SamplerComparisonState", "GroupMemoryBarrier", "GroupMemoryBarrierWithGroupSync",
        "DeviceMemoryBarrier", "DeviceMemoryBarrierWithGroupSync", "AllMemoryBarrier", "AllMemoryBarrierWithGroupSync"
    };
    static const char *const scalars[] = {
        "bool", "int", "uint", "dword", "half", "float", "double", "min16float", "min10float",
        "min16int", "min12int", "min16uint"
    };
    if (!name || !name[0]) return false;
    size_t length = 0;
    while (length < 256 && name[length]) {
        const uint8_t value = (uint8_t)name[length];
        if (!(length ? source_scan_identifier_continue(value) : source_scan_identifier_start(value)))
            return false;
        ++length;
    }
    if (length == 256) return false;
    if (strcmp(name, "_") == 0 || strncmp(name, "__", 2) == 0 ||
        strncmp(name, "UNITY_", 6) == 0 || strncmp(name, "SHADER_", 7) == 0 ||
        strncmp(name, "SV_", 3) == 0) return false;
    for (size_t index = 0; index < sizeof(reserved) / sizeof(reserved[0]); ++index)
        if (strcmp(name, reserved[index]) == 0) return false;
    for (size_t index = 0; index < sizeof(scalars) / sizeof(scalars[0]); ++index) {
        const size_t scalar_length = strlen(scalars[index]);
        if (strncmp(name, scalars[index], scalar_length) != 0) continue;
        const char *suffix = name + scalar_length;
        if (!*suffix || (suffix[0] >= '1' && suffix[0] <= '4' &&
            (!suffix[1] || (suffix[1] == 'x' && suffix[2] >= '1' && suffix[2] <= '4' && !suffix[3]))))
            return false;
    }
    return true;
}
