// SPDX-License-Identifier: GPL-3.0-only

#include "io/shader_blob_archive.h"

#include "io/lz4_decompress.h"
#include <limits.h>
#include <string.h>

static bool value_array_is_values(const TypeTreeValue* array) {
    return array && array->type == VAL_TYPE_ARRAY &&
           array->array_val.count >= 0 &&
           array->array_val.storage == TYPETREE_ARRAY_VALUES &&
           (array->array_val.count == 0 || array->array_val.elements != NULL);
}

static int find_platform_index(const TypeTreeValue* platforms, int platform) {
    const TypeTreeValue* array = typetree_get_array(platforms);
    if (!array) return -1;
    int match = -1;
    for (int i = 0; i < array->array_val.count; i++) {
        int64_t value = 0;
        if (!typetree_array_get_int(array, i, &value) ||
            value < INT_MIN || value > INT_MAX) {
            return -1;
        }
        if ((int)value == platform) {
            if (match >= 0) return -1;
            match = i;
        }
    }
    return match;
}

typedef struct {
    const TypeTreeValue* platform_offsets;
    const TypeTreeValue* platform_compressed;
    const TypeTreeValue* platform_decompressed;
    const TypeTreeValue* blob_array;
    const uint8_t* packed_blob;
    size_t blob_length;
    int segment_count;
    uint32_t stage_count;
    uint64_t total_decompressed_bytes;
    bool has_packed_blob;
} ShaderBlobArchiveLayout;

typedef struct {
    size_t offset;
    size_t compressed_length;
    size_t decompressed_length;
} ShaderBlobSegmentInfo;

static bool shader_blob_segment_info(
    const ShaderBlobArchiveLayout* layout, int segment_index,
    ShaderBlobSegmentInfo* info) {
    if (!layout || !info || segment_index < 0 ||
        segment_index >= layout->segment_count) {
        return false;
    }
    uint64_t offset = 0U;
    uint64_t compressed_length = 0U;
    uint64_t decompressed_length = 0U;
    if (!typetree_array_get_uint(
            layout->platform_offsets, segment_index, &offset) ||
        !typetree_array_get_uint(
            layout->platform_compressed, segment_index,
            &compressed_length) ||
        !typetree_array_get_uint(
            layout->platform_decompressed, segment_index,
            &decompressed_length) ||
        offset > UINT32_MAX || compressed_length > INT_MAX ||
        decompressed_length > INT_MAX || compressed_length == 0U ||
        decompressed_length == 0U || offset > layout->blob_length ||
        compressed_length > layout->blob_length - (size_t)offset) {
        return false;
    }
    info->offset = (size_t)offset;
    info->compressed_length = (size_t)compressed_length;
    info->decompressed_length = (size_t)decompressed_length;
    return true;
}

static bool shader_blob_archive_layout_init(
    const TypeTreeValue* shader_value, int platform,
    ShaderBlobArchiveLayout* layout) {
    if (!shader_value || !layout) return false;
    memset(layout, 0, sizeof(*layout));

    /* Unity 2021.3 Shader::Transfer owns these archive planes at the Shader
     * root.  m_ParsedForm is a different SerializedShader authority and must
     * never be searched as a fallback or mixed with root fields. */
    const TypeTreeValue* platforms =
        typetree_find_child(shader_value, "platforms");
    const TypeTreeValue* offsets =
        typetree_find_child(shader_value, "offsets");
    const TypeTreeValue* compressed_lengths =
        typetree_find_child(shader_value, "compressedLengths");
    const TypeTreeValue* decompressed_lengths =
        typetree_find_child(shader_value, "decompressedLengths");
    const TypeTreeValue* compressed_blob =
        typetree_find_child(shader_value, "compressedBlob");
    const TypeTreeValue* stage_counts =
        typetree_find_child(shader_value, "stageCounts");
    int platform_index = find_platform_index(platforms, platform);
    if (platform_index < 0 || !offsets || !compressed_lengths ||
        !decompressed_lengths || !compressed_blob || !stage_counts) {
        return false;
    }

    const TypeTreeValue* offsets_outer = typetree_get_array(offsets);
    const TypeTreeValue* compressed_outer =
        typetree_get_array(compressed_lengths);
    const TypeTreeValue* decompressed_outer =
        typetree_get_array(decompressed_lengths);
    const TypeTreeValue* blob_array = typetree_get_array(compressed_blob);
    const TypeTreeValue* platforms_array = typetree_get_array(platforms);
    const TypeTreeValue* stage_counts_array =
        typetree_get_array(stage_counts);
    if (!value_array_is_values(platforms_array) ||
        !value_array_is_values(offsets_outer) ||
        !value_array_is_values(compressed_outer) ||
        !value_array_is_values(decompressed_outer) ||
        !value_array_is_values(stage_counts_array) || !blob_array ||
        offsets_outer->array_val.count != platforms_array->array_val.count ||
        compressed_outer->array_val.count !=
            platforms_array->array_val.count ||
        decompressed_outer->array_val.count !=
            platforms_array->array_val.count ||
        stage_counts_array->array_val.count !=
            platforms_array->array_val.count ||
        platform_index >= offsets_outer->array_val.count ||
        platform_index >= compressed_outer->array_val.count ||
        platform_index >= decompressed_outer->array_val.count ||
        platform_index >= stage_counts_array->array_val.count) {
        return false;
    }

    uint64_t stage_count = 0U;
    if (!typetree_array_get_uint(stage_counts_array, platform_index,
                                 &stage_count) ||
        stage_count > UINT32_MAX) {
        return false;
    }

    layout->platform_offsets = typetree_get_array(
        &offsets_outer->array_val.elements[platform_index]);
    layout->platform_compressed = typetree_get_array(
        &compressed_outer->array_val.elements[platform_index]);
    layout->platform_decompressed = typetree_get_array(
        &decompressed_outer->array_val.elements[platform_index]);
    if (!value_array_is_values(layout->platform_offsets) ||
        !value_array_is_values(layout->platform_compressed) ||
        !value_array_is_values(layout->platform_decompressed) ||
        layout->platform_offsets->array_val.count !=
            layout->platform_compressed->array_val.count ||
        layout->platform_offsets->array_val.count !=
            layout->platform_decompressed->array_val.count ||
        layout->platform_offsets->array_val.count <= 0) {
        return false;
    }

    if (blob_array->array_val.count < 0) return false;
    layout->blob_array = blob_array;
    layout->blob_length = (size_t)blob_array->array_val.count;
    size_t packed_blob_length = 0U;
    layout->has_packed_blob = typetree_get_byte_span(
        blob_array, &layout->packed_blob, &packed_blob_length);
    if (layout->has_packed_blob &&
        packed_blob_length != layout->blob_length) {
        return false;
    }
    layout->segment_count = layout->platform_offsets->array_val.count;
    layout->stage_count = (uint32_t)stage_count;

    uint64_t total_decompressed_bytes = 0U;
    for (int i = 0; i < layout->segment_count; ++i) {
        ShaderBlobSegmentInfo segment;
        if (!shader_blob_segment_info(layout, i, &segment) ||
            UINT64_MAX - total_decompressed_bytes <
                segment.decompressed_length) {
            return false;
        }
        total_decompressed_bytes += segment.decompressed_length;
    }
    layout->total_decompressed_bytes = total_decompressed_bytes;
    return true;
}

static bool shader_blob_decompress_segment(
    const ShaderBlobArchiveLayout* layout,
    const ShaderBlobSegmentInfo* segment, uint8_t* output) {
    if (!layout || !segment || !output ||
        segment->compressed_length > INT_MAX ||
        segment->decompressed_length > INT_MAX) {
        return false;
    }
    const uint8_t* compressed = NULL;
    uint8_t* owned_compressed = NULL;
    if (layout->has_packed_blob) {
        compressed = layout->packed_blob + segment->offset;
    } else {
        owned_compressed = mem_alloc(segment->compressed_length);
        if (!owned_compressed || !typetree_array_copy_bytes(
                layout->blob_array, segment->offset, owned_compressed,
                segment->compressed_length)) {
            if (owned_compressed) {
                mem_free(owned_compressed, segment->compressed_length);
            }
            return false;
        }
        compressed = owned_compressed;
    }
    int result = lz4_decompress_safe(
        compressed, output, (int)segment->compressed_length,
        (int)segment->decompressed_length);
    if (owned_compressed) {
        mem_free(owned_compressed, segment->compressed_length);
    }
    return result == (int)segment->decompressed_length;
}

static bool shader_blob_entry_table_header(
    const uint8_t* segment, size_t segment_length, int* entry_count,
    size_t* table_end) {
    if (!segment || segment_length < 4U || !entry_count || !table_end) {
        return false;
    }
    uint32_t raw_entry_count = 0U;
    memcpy(&raw_entry_count, segment, sizeof(raw_entry_count));
    raw_entry_count = read_le32(raw_entry_count);
    if (raw_entry_count > INT_MAX ||
        dxbc_size_multiply_overflows((size_t)raw_entry_count, 12U) ||
        dxbc_size_add_overflows(4U, (size_t)raw_entry_count * 12U)) {
        return false;
    }
    size_t end = 4U + (size_t)raw_entry_count * 12U;
    if (end > segment_length) return false;
    *entry_count = (int)raw_entry_count;
    *table_end = end;
    return true;
}

static bool shader_blob_validate_entries(
    const ShaderBlobArchiveLayout* layout, const uint8_t* first_segment,
    size_t first_segment_length, int entry_count, size_t table_end,
    BlobEntry* retained_entries) {
    if (!layout || !first_segment || entry_count < 0 ||
        table_end > first_segment_length) {
        return false;
    }
    for (int i = 0; i < entry_count; ++i) {
        uint32_t fields[3];
        memcpy(fields, first_segment + 4U + (size_t)i * 12U,
               sizeof(fields));
        BlobEntry entry;
        entry.offset = (int32_t)read_le32(fields[0]);
        entry.length = (int32_t)read_le32(fields[1]);
        entry.segment = (int32_t)read_le32(fields[2]);
        if (entry.segment < 0 || entry.segment >= layout->segment_count ||
            entry.offset < 0 || entry.length < 0) {
            return false;
        }
        ShaderBlobSegmentInfo segment;
        if (!shader_blob_segment_info(layout, entry.segment, &segment) ||
            (size_t)entry.offset > segment.decompressed_length ||
            (size_t)entry.length >
                segment.decompressed_length - (size_t)entry.offset ||
            (entry.segment == 0 && (size_t)entry.offset < table_end)) {
            return false;
        }
        if (retained_entries) retained_entries[i] = entry;
    }
    return true;
}

bool shader_blob_archive_inspect(const TypeTreeValue* shader_value,
                                 int platform,
                                 ShaderBlobArchiveInfo* info) {
    if (!shader_value || !info) return false;
    ShaderBlobArchiveLayout layout;
    if (!shader_blob_archive_layout_init(shader_value, platform, &layout)) {
        return false;
    }

    int entry_count = 0;
    for (int i = 0; i < layout.segment_count; ++i) {
        ShaderBlobSegmentInfo segment;
        if (!shader_blob_segment_info(&layout, i, &segment)) return false;
        uint8_t* decompressed = mem_alloc(segment.decompressed_length);
        if (!decompressed) return false;
        bool valid = shader_blob_decompress_segment(
            &layout, &segment, decompressed);
        if (valid && i == 0) {
            size_t table_end = 0U;
            valid = shader_blob_entry_table_header(
                        decompressed, segment.decompressed_length,
                        &entry_count, &table_end) &&
                    shader_blob_validate_entries(
                        &layout, decompressed, segment.decompressed_length,
                        entry_count, table_end, NULL);
        }
        mem_free(decompressed, segment.decompressed_length);
        if (!valid) return false;
    }

    ShaderBlobArchiveInfo candidate;
    candidate.entry_count = (size_t)entry_count;
    candidate.segment_count = (size_t)layout.segment_count;
    candidate.stage_count = layout.stage_count;
    candidate.total_decompressed_bytes = layout.total_decompressed_bytes;
    *info = candidate;
    return true;
}

void shader_blob_archive_close(ShaderBlobArchive* archive) {
    if (!archive) return;
    if (archive->segments) {
        for (int i = 0; i < archive->segment_count; i++) {
            if (archive->segments[i]) {
                mem_free(archive->segments[i], archive->segment_lengths[i]);
            }
        }
        mem_free(archive->segments,
                 (size_t)archive->segment_count * sizeof(uint8_t*));
    }
    if (archive->segment_lengths) {
        mem_free(archive->segment_lengths,
                 (size_t)archive->segment_count * sizeof(int));
    }
    if (archive->entries) {
        mem_free(archive->entries,
                 (size_t)archive->entry_count * sizeof(BlobEntry));
    }
    memset(archive, 0, sizeof(*archive));
}

bool shader_blob_archive_open(const TypeTreeValue* shader_value,
                              int platform,
                              ShaderBlobArchive* archive) {
    if (!shader_value || !archive) return false;
    memset(archive, 0, sizeof(*archive));

    ShaderBlobArchiveLayout layout;
    if (!shader_blob_archive_layout_init(shader_value, platform, &layout) ||
        dxbc_size_multiply_overflows(
            (size_t)layout.segment_count, sizeof(*archive->segments)) ||
        dxbc_size_multiply_overflows(
            (size_t)layout.segment_count,
            sizeof(*archive->segment_lengths))) {
        return false;
    }

    archive->segment_count = layout.segment_count;
    archive->segments = mem_alloc(
        (size_t)archive->segment_count * sizeof(*archive->segments));
    if (archive->segments) {
        memset(archive->segments, 0,
               (size_t)archive->segment_count * sizeof(*archive->segments));
    }
    archive->segment_lengths = mem_alloc(
        (size_t)archive->segment_count * sizeof(*archive->segment_lengths));
    if (archive->segment_lengths) {
        memset(archive->segment_lengths, 0,
               (size_t)archive->segment_count *
                   sizeof(*archive->segment_lengths));
    }
    if (!archive->segments || !archive->segment_lengths) {
        shader_blob_archive_close(archive);
        return false;
    }

    for (int i = 0; i < archive->segment_count; ++i) {
        ShaderBlobSegmentInfo segment;
        if (!shader_blob_segment_info(&layout, i, &segment)) {
            shader_blob_archive_close(archive);
            return false;
        }
        archive->segment_lengths[i] = (int)segment.decompressed_length;
        archive->segments[i] = mem_alloc(segment.decompressed_length);
        if (!archive->segments[i]) {
            shader_blob_archive_close(archive);
            return false;
        }
        if (!shader_blob_decompress_segment(
                &layout, &segment, archive->segments[i])) {
            shader_blob_archive_close(archive);
            return false;
        }
    }

    size_t table_end = 0U;
    if (!shader_blob_entry_table_header(
            archive->segments[0], (size_t)archive->segment_lengths[0],
            &archive->entry_count, &table_end)) {
        shader_blob_archive_close(archive);
        return false;
    }
    if (archive->entry_count > 0) {
        if (dxbc_size_multiply_overflows(
                (size_t)archive->entry_count, sizeof(*archive->entries))) {
            shader_blob_archive_close(archive);
            return false;
        }
        archive->entries = mem_alloc(
            (size_t)archive->entry_count * sizeof(*archive->entries));
        if (!archive->entries) {
            shader_blob_archive_close(archive);
            return false;
        }
    }
    if (!shader_blob_validate_entries(
            &layout, archive->segments[0],
            (size_t)archive->segment_lengths[0], archive->entry_count,
            table_end, archive->entries)) {
        shader_blob_archive_close(archive);
        return false;
    }
    archive->stage_count = layout.stage_count;
    return true;
}

bool shader_blob_archive_get(const ShaderBlobArchive* archive,
                             int entry_index,
                             const uint8_t** payload,
                             size_t* payload_length) {
    if (!archive || !payload || !payload_length || entry_index < 0 ||
        entry_index >= archive->entry_count) {
        return false;
    }
    const BlobEntry* entry = &archive->entries[entry_index];
    if (entry->segment < 0 || entry->segment >= archive->segment_count ||
        entry->offset < 0 || entry->length < 0) {
        return false;
    }
    size_t segment_length =
        (size_t)archive->segment_lengths[entry->segment];
    if ((size_t)entry->offset > segment_length ||
        (size_t)entry->length > segment_length - (size_t)entry->offset) {
        return false;
    }
    *payload = archive->segments[entry->segment] + entry->offset;
    *payload_length = (size_t)entry->length;
    return true;
}
