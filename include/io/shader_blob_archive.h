// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_BLOB_ARCHIVE_H
#define SHADER_BLOB_ARCHIVE_H

#include "common/common.h"
#include "io/typetree.h"

typedef struct {
    int32_t offset;
    int32_t length;
    int32_t segment;
} BlobEntry;

typedef struct {
    BlobEntry* entries;
    int entry_count;
    uint8_t** segments;
    int* segment_lengths;
    int segment_count;
    /* Raw stageCounts element for the selected Unity 2021.3 platform plane.
     * Unity stores this value in ShaderBinaryData; its deeper semantics remain
     * intentionally uninterpreted here. */
    uint32_t stage_count;
} ShaderBlobArchive;

typedef struct {
    size_t entry_count;
    size_t segment_count;
    uint32_t stage_count;
    uint64_t total_decompressed_bytes;
} ShaderBlobArchiveInfo;

/*
 * Validates the selected platform archive exactly as open() does, including
 * every segment's LZ4 stream and every entry-table range, while retaining at
 * most one decompressed segment at a time.  Failure leaves info unchanged.
 */
bool shader_blob_archive_inspect(const TypeTreeValue* shader_value,
                                 int platform,
                                 ShaderBlobArchiveInfo* info);

bool shader_blob_archive_open(const TypeTreeValue* shader_value,
                              int platform,
                              ShaderBlobArchive* archive);
void shader_blob_archive_close(ShaderBlobArchive* archive);
bool shader_blob_archive_get(const ShaderBlobArchive* archive,
                             int entry_index,
                             const uint8_t** payload,
                             size_t* payload_length);

#endif
