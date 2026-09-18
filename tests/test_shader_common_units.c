#include "common/shader_artifact.h"
#include "io/shader_blob_archive.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void init_int_value(TypeTreeValue* value, int64_t integer) {
    memset(value, 0, sizeof(*value));
    value->type = VAL_TYPE_INT;
    value->int_val = integer;
}

static void init_value_array(TypeTreeValue* value, TypeTreeValue* elements,
                             int count) {
    memset(value, 0, sizeof(*value));
    value->type = VAL_TYPE_ARRAY;
    value->array_val.elements = elements;
    value->array_val.count = count;
    value->array_val.storage = TYPETREE_ARRAY_VALUES;
}

static int test_shader_artifact_names(void) {
    char artifact[256];
    CHECK(shader_artifact_filename(artifact, sizeof(artifact),
                                   "Hidden/Editor Gizmo", -123456789LL));
    CHECK(strcmp(artifact,
                 "shader_Hidden_Editor_Gizmo__-123456789.shader") == 0);
    CHECK(shader_artifact_filename(artifact, sizeof(artifact),
                                   "CON:*?<>|\\x", 7));
    CHECK(strcmp(artifact, "shader_CON_______x__7.shader") == 0);
    CHECK(!shader_artifact_filename(artifact, 8, "Hidden/Editor Gizmo", 1));
    CHECK(!shader_artifact_filename(artifact, sizeof(artifact), "", 1));

    CHECK(shader_flat_artifact_filename(
        artifact, sizeof(artifact), "Example/Layered Surface Cutout", NULL,
        101));
    CHECK(strcmp(artifact, "Example_Layered Surface Cutout.shader") == 0);
    CHECK(shader_flat_artifact_filename(
        artifact, sizeof(artifact), "Hidden/Effects/Analog Scan", NULL, 202));
    CHECK(strcmp(artifact, "Hidden_Effects_Analog Scan.shader") == 0);
    CHECK(shader_flat_artifact_filename(
        artifact, sizeof(artifact), "CON", NULL, 1));
    CHECK(strcmp(artifact, "_CON.shader") == 0);
    CHECK(shader_flat_artifact_filename(
        artifact, sizeof(artifact), "Hidden/Effects/Analog Scan",
        "0000000000000000000000000000000000000000000000000000000000000000",
        303));
    CHECK(strcmp(
        artifact,
        "Hidden_Effects_Analog Scan__"
        "0000000000000000000000000000000000000000000000000000000000000000_"
        "303.shader") == 0);
    CHECK(!shader_flat_artifact_filename(
        artifact, 8, "Hidden/Effects/Analog Scan", NULL, 1));
    CHECK(!shader_flat_artifact_filename(
        artifact, sizeof(artifact), "", NULL, 1));
    CHECK(!shader_flat_artifact_filename(
        artifact, sizeof(artifact), "Valid", "NOT-HEX", 1));
    return 0;
}

static int test_shader_blob_archive(void) {
    const size_t baseline_bytes = g_allocated_bytes;
    const size_t baseline_allocations = g_allocations_count;
    uint8_t decompressed_segment[20] = {
        1, 0, 0, 0,       /* one BlobEntry */
        16, 0, 0, 0,      /* payload offset */
        4, 0, 0, 0,       /* payload length */
        0, 0, 0, 0,       /* segment index */
        'D', 'X', 'B', 'C'
    };
    uint8_t compressed_segment[22];
    compressed_segment[0] = 0xf0; /* 15 literal bytes */
    compressed_segment[1] = 5;    /* plus 5 = 20 literals */
    memcpy(compressed_segment + 2, decompressed_segment,
           sizeof(decompressed_segment));

    TypeTreeValue shader_root;
    TypeTreeValue shader_fields[7];
    TypeTreeValue platform_values[1];
    TypeTreeValue offsets_outer_values[1];
    TypeTreeValue offset_values[1];
    TypeTreeValue compressed_outer_values[1];
    TypeTreeValue compressed_length_values[1];
    TypeTreeValue decompressed_outer_values[1];
    TypeTreeValue decompressed_length_values[1];
    TypeTreeValue stage_count_values[1];
    TypeTreeValue parsed_form_fields[2];
    TypeTreeValue parsed_platform_values[1];
    TypeTreeValue parsed_stage_count_values[1];
    memset(&shader_root, 0, sizeof(shader_root));
    memset(shader_fields, 0, sizeof(shader_fields));
    memset(parsed_form_fields, 0, sizeof(parsed_form_fields));
    shader_root.type = VAL_TYPE_STRUCT;
    shader_root.struct_val.members = shader_fields;
    shader_root.struct_val.count = 7;

    init_int_value(&platform_values[0], 4);
    init_value_array(&shader_fields[0], platform_values, 1);
    shader_fields[0].name = "platforms";

    init_int_value(&offset_values[0], 0);
    init_value_array(&offsets_outer_values[0], offset_values, 1);
    init_value_array(&shader_fields[1], offsets_outer_values, 1);
    shader_fields[1].name = "offsets";

    init_int_value(&compressed_length_values[0],
                   (int64_t)sizeof(compressed_segment));
    init_value_array(&compressed_outer_values[0],
                     compressed_length_values, 1);
    init_value_array(&shader_fields[2], compressed_outer_values, 1);
    shader_fields[2].name = "compressedLengths";

    init_int_value(&decompressed_length_values[0],
                   (int64_t)sizeof(decompressed_segment));
    init_value_array(&decompressed_outer_values[0],
                     decompressed_length_values, 1);
    init_value_array(&shader_fields[3], decompressed_outer_values, 1);
    shader_fields[3].name = "decompressedLengths";

    shader_fields[4].type = VAL_TYPE_ARRAY;
    shader_fields[4].array_val.count = (int)sizeof(compressed_segment);
    shader_fields[4].array_val.storage = TYPETREE_ARRAY_PACKED_BYTES;
    shader_fields[4].array_val.packed_bytes = compressed_segment;
    shader_fields[4].name = "compressedBlob";

    init_int_value(&stage_count_values[0], 6);
    init_value_array(&shader_fields[5], stage_count_values, 1);
    shader_fields[5].name = "stageCounts";

    /* Conflicting archive-looking fields under m_ParsedForm must not become
     * an alternate authority for Shader-root binary data. */
    init_int_value(&parsed_platform_values[0], 99);
    init_value_array(&parsed_form_fields[0], parsed_platform_values, 1);
    parsed_form_fields[0].name = "platforms";
    init_int_value(&parsed_stage_count_values[0], 999);
    init_value_array(&parsed_form_fields[1], parsed_stage_count_values, 1);
    parsed_form_fields[1].name = "stageCounts";
    shader_fields[6].type = VAL_TYPE_STRUCT;
    shader_fields[6].struct_val.members = parsed_form_fields;
    shader_fields[6].struct_val.count = 2;
    shader_fields[6].name = "m_ParsedForm";

    ShaderBlobArchive packed_archive;
    CHECK(shader_blob_archive_open(&shader_root, 4, &packed_archive));
    CHECK(packed_archive.segment_count == 1);
    CHECK(packed_archive.entry_count == 1);
    CHECK(packed_archive.stage_count == 6);
    const uint8_t* archive_payload = NULL;
    size_t archive_payload_size = 0;
    CHECK(shader_blob_archive_get(&packed_archive, 0, &archive_payload,
                                  &archive_payload_size));
    CHECK(archive_payload_size == 4);
    CHECK(memcmp(archive_payload, "DXBC", 4) == 0);
    shader_blob_archive_close(&packed_archive);
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    ShaderBlobArchiveInfo archive_info = {99U, 98U, 97U, 96U};
    CHECK(shader_blob_archive_inspect(&shader_root, 4, &archive_info));
    CHECK(archive_info.entry_count == 1U);
    CHECK(archive_info.segment_count == 1U);
    CHECK(archive_info.stage_count == 6U);
    CHECK(archive_info.total_decompressed_bytes ==
          sizeof(decompressed_segment));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    /* The nested conflict does not add a platform or substitute for a
     * missing root field. */
    ShaderBlobArchive conflicting_authority;
    CHECK(!shader_blob_archive_open(&shader_root, 99,
                                    &conflicting_authority));
    shader_fields[5].name = "";
    CHECK(!shader_blob_archive_open(&shader_root, 4,
                                    &conflicting_authority));
    shader_fields[5].name = "stageCounts";
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    /* Every per-platform plane must have exactly one row per platform. */
    shader_fields[1].array_val.count = 0;
    ShaderBlobArchive mismatched_planes;
    CHECK(!shader_blob_archive_open(&shader_root, 4, &mismatched_planes));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    shader_fields[1].array_val.count = 1;

    shader_fields[5].array_val.count = 0;
    ShaderBlobArchive mismatched_stage_counts;
    CHECK(!shader_blob_archive_open(&shader_root, 4,
                                    &mismatched_stage_counts));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    shader_fields[5].array_val.count = 1;

    stage_count_values[0].int_val = (int64_t)UINT32_MAX + 1;
    ShaderBlobArchive oversized_stage_count;
    CHECK(!shader_blob_archive_open(&shader_root, 4,
                                    &oversized_stage_count));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    stage_count_values[0].int_val = 6;

    /* A malformed outer array must fail before its elements are indexed. */
    shader_fields[1].array_val.storage = TYPETREE_ARRAY_PACKED_BYTES;
    shader_fields[1].array_val.elements = NULL;
    ShaderBlobArchive wrong_outer_shape;
    CHECK(!shader_blob_archive_open(&shader_root, 4, &wrong_outer_shape));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    shader_fields[1].array_val.storage = TYPETREE_ARRAY_VALUES;
    shader_fields[1].array_val.elements = offsets_outer_values;

    decompressed_length_values[0].int_val =
        (int64_t)sizeof(decompressed_segment) + 1;
    ShaderBlobArchive truncated_archive;
    CHECK(!shader_blob_archive_open(&shader_root, 4, &truncated_archive));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    decompressed_length_values[0].int_val =
        (int64_t)sizeof(decompressed_segment);

    /* Inspection must validate every selected LZ4 stream, not just the first
     * segment containing the entry table.  Its output is transactional. */
    uint8_t two_segment_blob[sizeof(compressed_segment) + 1U];
    memcpy(two_segment_blob, compressed_segment, sizeof(compressed_segment));
    two_segment_blob[sizeof(compressed_segment)] = 0x10U;
    TypeTreeValue two_offsets[2];
    TypeTreeValue two_compressed_lengths[2];
    TypeTreeValue two_decompressed_lengths[2];
    init_int_value(&two_offsets[0], 0);
    init_int_value(&two_offsets[1], (int64_t)sizeof(compressed_segment));
    init_int_value(&two_compressed_lengths[0],
                   (int64_t)sizeof(compressed_segment));
    init_int_value(&two_compressed_lengths[1], 1);
    init_int_value(&two_decompressed_lengths[0],
                   (int64_t)sizeof(decompressed_segment));
    init_int_value(&two_decompressed_lengths[1], 1);
    init_value_array(&offsets_outer_values[0], two_offsets, 2);
    init_value_array(&compressed_outer_values[0], two_compressed_lengths, 2);
    init_value_array(&decompressed_outer_values[0],
                     two_decompressed_lengths, 2);
    shader_fields[4].array_val.count = (int)sizeof(two_segment_blob);
    shader_fields[4].array_val.packed_bytes = two_segment_blob;

    ShaderBlobArchiveInfo unchanged_info = {41U, 42U, 43U, 44U};
    CHECK(!shader_blob_archive_inspect(&shader_root, 4, &unchanged_info));
    CHECK(unchanged_info.entry_count == 41U);
    CHECK(unchanged_info.segment_count == 42U);
    CHECK(unchanged_info.stage_count == 43U);
    CHECK(unchanged_info.total_decompressed_bytes == 44U);
    ShaderBlobArchive malformed_later_archive;
    CHECK(!shader_blob_archive_open(
        &shader_root, 4, &malformed_later_archive));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);

    init_value_array(&offsets_outer_values[0], offset_values, 1);
    init_value_array(&compressed_outer_values[0],
                     compressed_length_values, 1);
    init_value_array(&decompressed_outer_values[0],
                     decompressed_length_values, 1);
    shader_fields[4].array_val.count = (int)sizeof(compressed_segment);
    shader_fields[4].array_val.packed_bytes = compressed_segment;

    TypeTreeValue legacy_blob_values[sizeof(compressed_segment)];
    for (size_t i = 0; i < sizeof(legacy_blob_values) /
                                sizeof(legacy_blob_values[0]); i++) {
        init_int_value(&legacy_blob_values[i], compressed_segment[i]);
    }
    init_value_array(&shader_fields[4], legacy_blob_values,
                     (int)sizeof(compressed_segment));
    shader_fields[4].name = "compressedBlob";
    ShaderBlobArchive legacy_archive;
    CHECK(shader_blob_archive_open(&shader_root, 4, &legacy_archive));
    CHECK(shader_blob_archive_get(&legacy_archive, 0, &archive_payload,
                                  &archive_payload_size));
    CHECK(archive_payload_size == 4);
    CHECK(memcmp(archive_payload, "DXBC", 4) == 0);
    shader_blob_archive_close(&legacy_archive);
    CHECK(shader_blob_archive_inspect(&shader_root, 4, &archive_info));
    CHECK(archive_info.entry_count == 1U);
    CHECK(archive_info.segment_count == 1U);
    CHECK(archive_info.stage_count == 6U);
    CHECK(archive_info.total_decompressed_bytes ==
          sizeof(decompressed_segment));
    CHECK(g_allocated_bytes == baseline_bytes);
    CHECK(g_allocations_count == baseline_allocations);
    return 0;
}

int main(void) {
    CHECK(test_shader_artifact_names() == 0);
    CHECK(test_shader_blob_archive() == 0);
    CHECK(g_allocations_count == 0U);
    CHECK(g_allocated_bytes == 0U);
    puts("DXBC shader common unit tests passed.");
    return 0;
}
