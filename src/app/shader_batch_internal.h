// SPDX-License-Identifier: GPL-3.0-only

#ifndef SHADER_BATCH_INTERNAL_H
#define SHADER_BATCH_INTERNAL_H

#include "app/shader_batch.h"

/* Transfers staged graphics paths into the exact failure ledger only for
 * companions marked as possible publication residue. The caller retains
 * ownership of every non-transferred path. */
void shader_batch_retain_graphics_publication_residue_paths(
    ShaderBatchRecordResult* result,
    char** owned_shader_path, char** owned_meta_path);

/* Derives the flat graphics filename from the exact decoded Shader name.
 * The full SerializedFile digest and PathID are appended only when another
 * graphics record in the same catalog has a colliding portable name. */
bool shader_batch_flat_graphics_filename(
    const ShaderCatalog* catalog, size_t record_index,
    const char* decoded_shader_name, char* output, size_t output_size);

#endif /* SHADER_BATCH_INTERNAL_H */
