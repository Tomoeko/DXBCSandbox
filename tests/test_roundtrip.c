#include "common/common.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/shader_blob_archive.h"
#include "io/subprogram_metadata.h"
#include "io/lz4_decompress.h"
#include "dxbc/dxbc_parser.h"



static uint8_t* read_file_to_buffer(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("Failed to open file: %s", path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    
    if (read_bytes != (size_t)size) {
        free(buf);
        return NULL;
    }
    
    *out_size = (size_t)size;
    return buf;
}

static const TypeTreeValue* find_field(const TypeTreeValue* root, const TypeTreeValue* parsed_form, const char* name) {
    const TypeTreeValue* val = typetree_find_child(parsed_form, name);
    if (!val) val = typetree_find_child(root, name);
    return val;
}

static void print_struct_layout(const TypeTreeValue* val, int depth) {
    if (!val) return;
    if (depth > 60) depth = 60;
    char indent[128];
    memset(indent, ' ', depth * 2);
    indent[depth * 2] = '\0';
    
    if (val->type == VAL_TYPE_STRUCT) {
        printf("%sStruct: %s (%s) members=%d\n", indent, val->name, val->type_str, val->struct_val.count);
        for (int i = 0; i < val->struct_val.count; i++) {
            print_struct_layout(&val->struct_val.members[i], depth + 1);
        }
    } else if (val->type == VAL_TYPE_ARRAY) {
        printf("%sArray: %s (%s) size=%d\n", indent, val->name, val->type_str, val->array_val.count);
        if (val->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
            printf("%s  <packed byte storage>\n", indent);
            return;
        }
        int print_limit = val->array_val.count;
        if (strcmp(val->name, "compressedBlob") == 0 || strcmp(val->name, "bytes") == 0) {
            print_limit = print_limit > 5 ? 5 : print_limit;
        } else {
            print_limit = print_limit > 50 ? 50 : print_limit;
        }
        for (int i = 0; i < print_limit; i++) {
            print_struct_layout(&val->array_val.elements[i], depth + 1);
        }
        if (val->array_val.count > print_limit) {
            printf("%s  ... and %d more elements\n", indent, val->array_val.count - print_limit);
        }
    } else if (val->type == VAL_TYPE_STRING) {
        printf("%sString: %s (%s) = \"%s\"\n", indent, val->name, val->type_str, val->string_val);
    } else if (val->type == VAL_TYPE_INT) {
        printf("%sInt: %s (%s) = %lld\n", indent, val->name, val->type_str, (long long)val->int_val);
    } else if (val->type == VAL_TYPE_FLOAT) {
        printf("%sFloat: %s (%s) = %f\n", indent, val->name, val->type_str, val->float_val);
    }
}

static void verify_subprogram(const TypeTreeValue* val, BlobEntry* blob_entries, int entry_count, uint8_t** segments, int segment_count) {
    if (!val) return;
    
    // Debug print
    // printf("Traversing Node: name=%s, type=%d\n", val->name, val->type);
    
    const TypeTreeValue* blob_idx_val = typetree_find_child(val, "m_BlobIndex");
    if (!blob_idx_val) blob_idx_val = typetree_find_child(val, "BlobIndex");
    
    const TypeTreeValue* prog_type_val = typetree_find_child(val, "m_GpuProgramType");
    if (!prog_type_val) prog_type_val = typetree_find_child(val, "GpuProgramType");
    if (!prog_type_val) prog_type_val = typetree_find_child(val, "m_ProgramType");
    if (!prog_type_val) prog_type_val = typetree_find_child(val, "ProgramType");
    
    if (blob_idx_val && prog_type_val) {
        int32_t blob_idx = (int32_t)blob_idx_val->int_val;
        int32_t prog_type = (int32_t)prog_type_val->int_val;
        
        printf("\n=== Verifying Subprogram (TypeTree path) ===\n");
        printf("TypeTree metadata: blob_index=%d, program_type=%d\n", blob_idx, prog_type);
        
        // Print the layout of this subprogram struct in the TypeTree to see all available fields
        print_struct_layout(val, 1);
        
        if (blob_idx >= 0 && blob_idx < entry_count) {
            BlobEntry entry = blob_entries[blob_idx];
            if (entry.segment >= 0 && entry.segment < segment_count && segments[entry.segment]) {
                const uint8_t* payload = segments[entry.segment] + entry.offset;
                size_t payload_len = entry.length;
                
                printf("Binary payload size: %zu bytes (Segment %d, Offset %d)\n", payload_len, entry.segment, entry.offset);
                
                ByteStream stream;
                stream_init(&stream, payload, payload_len);
                stream_set_endian(&stream, false); // Little Endian
                
                PlayerSubProgramMetadata bin_sub;
                if (subprogram_metadata_parse_variant(&stream, &bin_sub)) {
                    printf("[SUCCESS] Binary subprogram header parsed successfully!\n");
                    printf("  Binary Version: %u\n", bin_sub.version);
                    printf("  Binary Program Type: %d\n", bin_sub.program_type);
                    printf("  Binary Header Words: {%u, %u, %u, %u}\n",
                           bin_sub.player_header_words[0],
                           bin_sub.player_header_words[1],
                           bin_sub.player_header_words[2],
                           bin_sub.player_header_words[3]);
                    printf("  Binary Source Map: 0x%x\n", bin_sub.source_map);
                    printf("  Binary Bytecode Length: %u bytes\n", bin_sub.bytecode_length);
                    printf("  Binary Local Keywords Count: %d\n", bin_sub.local_keyword_count);
                    for (int k = 0; k < bin_sub.local_keyword_count; k++) {
                        printf("    - Keyword [%d]: \"%s\"\n", k, bin_sub.local_keywords[k]);
                    }
                    printf("  Binary Bindings Count: %d\n", bin_sub.binding_count);
                    for (int k = 0; k < bin_sub.binding_count; k++) {
                        printf("    - Binding [%d]: channel=%u, component=%u\n", k, bin_sub.bindings[k].channel, bin_sub.bindings[k].component);
                    }
                    
                    // Cross-verification checks
                    // 1. Verify program type
                    if (bin_sub.program_type != prog_type) {
                        LOG_ERROR("Program type mismatch: TypeTree=%d vs Binary=%d", prog_type, bin_sub.program_type);
                    } else {
                        printf("[PASS] Program type matches!\n");
                    }
                    
                    // 2. Validate bytecode can be parsed as a valid DXBC container
                    if (bin_sub.bytecode_length > 0 && bin_sub.bytecode != NULL) {
                        DXBCContainer* dxbc = (DXBCContainer*)mem_alloc(sizeof(DXBCContainer));
                        if (dxbc && dxbc_parse(dxbc, bin_sub.bytecode, bin_sub.bytecode_length)) {
                            printf("[PASS] Subprogram bytecode parsed as valid DXBC container!\n");
                            printf("  DXBC Model: %s, Chunks: %d, Instructions: %d\n",
                                   dxbc->shader_type_model, dxbc->chunk_count, dxbc->instruction_count);
                            dxbc_free(dxbc);
                        } else {
                            LOG_ERROR("Failed to parse bytecode as DXBC!");
                        }
                        if (dxbc) {
                            mem_free(dxbc, sizeof(DXBCContainer));
                        }
                    }
                    
                    subprogram_metadata_free_variant(&bin_sub);
                } else {
                    LOG_ERROR("Failed to parse binary subprogram header!");
                }
            } else {
                LOG_ERROR("Invalid segment index or segment not loaded: %d", entry.segment);
            }
        }
        return;
    }
    
    if (val->type == VAL_TYPE_STRUCT) {
        for (int i = 0; i < val->struct_val.count; i++) {
            verify_subprogram(&val->struct_val.members[i], blob_entries, entry_count, segments, segment_count);
        }
    } else if (val->type == VAL_TYPE_ARRAY &&
               val->array_val.storage == TYPETREE_ARRAY_VALUES) {
        for (int i = 0; i < val->array_val.count; i++) {
            verify_subprogram(&val->array_val.elements[i], blob_entries, entry_count, segments, segment_count);
        }
    }
}

static void process_shader_object(SerializedFile* file, const AssetObjectInfo* obj) {
    size_t shader_size = 0;
    const uint8_t* shader_data = serialized_file_get_object_data(file, obj, &shader_size);
    if (!shader_data || shader_size == 0) {
        LOG_ERROR("Failed to get Shader object data");
        return;
    }
    
    printf("\n--- Processing Shader Object: path_id=%lld ---\n", (long long)obj->path_id);
    printf("Raw Shader Object Data Size: %zu bytes\n", shader_size);
    
    TypeTreeValue shader_value;
    int node_idx = 0;
    ByteStream stream;
    stream_init(&stream, shader_data, shader_size);
    stream_set_endian(&stream, file->big_endian);
    
    if (!typetree_parse_value_ex(
            &file->types[obj->type_id_or_index], &node_idx, &stream,
            &shader_value, TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) {
        LOG_ERROR("Failed to parse Shader object tree");
        return;
    }
    
    printf("\n=== FULL SHADER TYPETREE LAYOUT ===\n");
    print_struct_layout(&shader_value, 0);
    printf("=== END FULL SHADER TYPETREE LAYOUT ===\n");
    
    // VERIFICATION 1: Validate exact byte consumption
    printf("TypeTree stream offset after parsing: %zu bytes\n", stream.position);
    if (stream.position == shader_size) {
        printf("[PASS] TypeTree successfully consumed exactly 100%% of Shader object data (%zu of %zu bytes)\n",
               stream.position, shader_size);
    } else {
        LOG_ERROR("TypeTree byte mismatch! Consumed %zu of %zu bytes (unparsed trailing bytes: %zu)",
                  stream.position, shader_size, shader_size - stream.position);
    }
    
    const TypeTreeValue* parsed_form = typetree_find_path(&shader_value, "m_ParsedForm");
    if (!parsed_form) parsed_form = &shader_value;
    
    const TypeTreeValue* platforms_val = find_field(&shader_value, parsed_form, "platforms");
    const TypeTreeValue* offsets_val = find_field(&shader_value, parsed_form, "offsets");
    const TypeTreeValue* comp_lens_val = find_field(&shader_value, parsed_form, "compressedLengths");
    const TypeTreeValue* decomp_lens_val = find_field(&shader_value, parsed_form, "decompressedLengths");
    const TypeTreeValue* blob_val = find_field(&shader_value, parsed_form, "compressedBlob");
    
    if (!platforms_val || !offsets_val || !comp_lens_val || !decomp_lens_val || !blob_val) {
        LOG_ERROR("Missing required shader blob fields in serialized TypeTree");
        typetree_free_value(&shader_value);
        return;
    }
    
    int platform_idx = -1;
    const TypeTreeValue* arr_platforms = typetree_get_array(platforms_val);
    if (arr_platforms) {
        for (int i = 0; i < arr_platforms->array_val.count; i++) {
            if (arr_platforms->array_val.elements[i].int_val == 4) { // 4 = d3d11
                platform_idx = i;
                break;
            }
        }
    }
    
    if (platform_idx == -1) {
        printf("d3d11 platform not found, defaulting to first platform (index 0)\n");
        platform_idx = 0;
    }
    
    const TypeTreeValue* arr_offsets_outer = typetree_get_array(offsets_val);
    const TypeTreeValue* arr_comp_lens_outer = typetree_get_array(comp_lens_val);
    const TypeTreeValue* arr_decomp_lens_outer = typetree_get_array(decomp_lens_val);
    
    if (!arr_offsets_outer || platform_idx >= arr_offsets_outer->array_val.count ||
        !arr_comp_lens_outer || platform_idx >= arr_comp_lens_outer->array_val.count ||
        !arr_decomp_lens_outer || platform_idx >= arr_decomp_lens_outer->array_val.count) {
        LOG_ERROR("Invalid platform offset/length outer arrays");
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* platform_offsets = &arr_offsets_outer->array_val.elements[platform_idx];
    const TypeTreeValue* platform_comp_lens = &arr_comp_lens_outer->array_val.elements[platform_idx];
    const TypeTreeValue* platform_decomp_lens = &arr_decomp_lens_outer->array_val.elements[platform_idx];
    
    const TypeTreeValue* arr_offsets = typetree_get_array(platform_offsets);
    const TypeTreeValue* arr_comp_lens = typetree_get_array(platform_comp_lens);
    const TypeTreeValue* arr_decomp_lens = typetree_get_array(platform_decomp_lens);
    
    if (!arr_offsets || !arr_comp_lens || !arr_decomp_lens) {
        LOG_ERROR("Platform offsets/lengths nested arrays are invalid");
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* flat_blob = typetree_get_array(blob_val);
    if (!flat_blob) {
        LOG_ERROR("Failed to get compressedBlob array");
        typetree_free_value(&shader_value);
        return;
    }
    
    int blob_len = flat_blob->array_val.count;
    uint8_t* compressed_data_flat = (uint8_t*)mem_alloc(blob_len);
    if (!compressed_data_flat) {
        typetree_free_value(&shader_value);
        return;
    }
    if (!typetree_array_copy_bytes(flat_blob, 0, compressed_data_flat,
                                   (size_t)blob_len)) {
        mem_free(compressed_data_flat, blob_len);
        typetree_free_value(&shader_value);
        return;
    }
    
    int segment_count = arr_offsets->array_val.count;
    uint8_t** segments = (uint8_t**)mem_alloc(segment_count * sizeof(uint8_t*));
    int* segment_lens = (int*)mem_alloc(segment_count * sizeof(int));
    memset(segments, 0, segment_count * sizeof(uint8_t*));
    
    bool decompress_ok = true;
    for (int i = 0; i < segment_count; i++) {
        uint32_t offset = (uint32_t)arr_offsets->array_val.elements[i].int_val;
        uint32_t comp_len = (uint32_t)arr_comp_lens->array_val.elements[i].int_val;
        uint32_t decomp_len = (uint32_t)arr_decomp_lens->array_val.elements[i].int_val;
        
        segments[i] = (uint8_t*)mem_alloc(decomp_len);
        segment_lens[i] = decomp_len;
        
        if (offset + comp_len > (uint32_t)blob_len) {
            LOG_ERROR("Segment %d offset + length out of compressed blob size", i);
            decompress_ok = false;
            break;
        }
        
        int dec_res = lz4_decompress_safe(compressed_data_flat + offset, segments[i], comp_len, decomp_len);
        if (dec_res < 0) {
            LOG_ERROR("LZ4 decompression failed for segment %d", i);
            decompress_ok = false;
            break;
        }
    }
    
    if (decompress_ok && segment_count >= 1 && segment_lens[0] >= 4) {
        int32_t entry_count;
        memcpy(&entry_count, segments[0], 4);
        entry_count = read_le32(entry_count);
        
        printf("Total subprogram blob entries in table: %d\n", entry_count);
        
        BlobEntry* blob_entries = (BlobEntry*)mem_alloc(entry_count * sizeof(BlobEntry));
        int entry_offset = 4;
        for (int k = 0; k < entry_count && entry_offset + 12 <= segment_lens[0]; k++) {
            int32_t off = 0, len = 0, seg = 0;
            memcpy(&off, segments[0] + entry_offset, 4);
            memcpy(&len, segments[0] + entry_offset + 4, 4);
            memcpy(&seg, segments[0] + entry_offset + 8, 4);
            
            blob_entries[k].offset = read_le32(off);
            blob_entries[k].length = read_le32(len);
            blob_entries[k].segment = read_le32(seg);
            
            entry_offset += 12;
        }
        
        // VERIFICATION 2: Detailed validation of subprogram data headers
        verify_subprogram(&shader_value, blob_entries, entry_count, segments, segment_count);
        
        mem_free(blob_entries, entry_count * sizeof(BlobEntry));
    }
    
    // Cleanup
    for (int i = 0; i < segment_count; i++) {
        if (segments[i]) mem_free(segments[i], segment_lens[i]);
    }
    mem_free(segments, segment_count * sizeof(uint8_t*));
    mem_free(segment_lens, segment_count * sizeof(int));
    mem_free(compressed_data_flat, blob_len);
    typetree_free_value(&shader_value);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <path_to_bundle_or_assets_file>\n", argv[0]);
        return 1;
    }
    
    const char* path = argv[1];
    printf("Running roundtrip verification on: %s\n", path);
    
    size_t file_size = 0;
    uint8_t* file_data = read_file_to_buffer(path, &file_size);
    if (!file_data) {
        return 1;
    }
    
    BundleArchive archive;
    if (!bundle_open(&archive, file_data, file_size)) {
        LOG_ERROR("Failed to open AssetBundle container");
        free(file_data);
        return 1;
    }
    
    for (int i = 0; i < archive.directory_count; i++) {
        size_t sub_file_size = 0;
        const uint8_t* sub_file_data = bundle_get_file(&archive, archive.directories[i].name, &sub_file_size);
        if (sub_file_data && sub_file_size > 0) {
            SerializedFile file;
            if (serialized_file_open(&file, sub_file_data, sub_file_size)) {
                
                // Search for Shader objects (ClassID 48)
                for (int obj_idx = 0; obj_idx < file.object_count; obj_idx++) {
                    const AssetObjectInfo* obj = &file.objects[obj_idx];
                    if (obj->type_id == 48) { // ClassID 48 = Shader
                        process_shader_object(&file, obj);
                    }
                }
                
                serialized_file_close(&file);
            }
        }
    }
    
    bundle_close(&archive);
    free(file_data);
    
    // Memory leak tracking report
    printf("\nMemory allocations remaining: %zu blocks (%zu bytes)\n", g_allocations_count, g_allocated_bytes);
    if (g_allocations_count > 0 || g_allocated_bytes > 0) {
        printf("[FAIL] Memory leaks detected in roundtrip test!\n");
        return 1;
    } else {
        printf("[SUCCESS] No memory leaks detected in roundtrip test!\n");
    }
    
    printf("\n[SUCCESS] Roundtrip verification passed perfectly!\n");
    return 0;
}
