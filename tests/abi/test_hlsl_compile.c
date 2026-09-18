// clang-format off

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "common/common.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/shader_blob_archive.h"
#include "io/serialized_shader.h"
#include "io/subprogram_metadata.h"
#include "io/lz4_decompress.h"
#include "dxbc/dxbc_parser.h"
#include "translation/usil.h"
#include "translation/hlsl_emitter.h"

#include "test_hlsl_compile_verify.h"
#include "d3dcompiler_hook.h"

static long long g_filter_path_id = -1;
static int g_filter_stage = -1;
static int g_filter_sub_idx = -1;



static uint8_t* read_file_to_buffer(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);
    if (read_bytes != (size_t)size) { free(buf); return NULL; }
    buf[size] = '\0';
    *out_size = (size_t)size;
    return buf;
}

static const TypeTreeValue* find_field(const TypeTreeValue* root, const TypeTreeValue* parsed_form, const char* name) {
    const TypeTreeValue* val = typetree_find_child(parsed_form, name);
    if (!val) val = typetree_find_child(root, name);
    return val;
}

static void process_shader_object(
    SerializedFile* file,
    const AssetObjectInfo* obj
) {
    if (g_filter_path_id != -1 && (long long)obj->path_id != g_filter_path_id) {
        return;
    }
    size_t shader_size = 0;
    const uint8_t* shader_data = serialized_file_get_object_data(file, obj, &shader_size);
    if (!shader_data || shader_size == 0) return;
    
    TypeTreeValue shader_value;
    int node_idx = 0;
    ByteStream stream;
    stream_init(&stream, shader_data, shader_size);
    stream_set_endian(&stream, file->big_endian);
    if (!typetree_parse_value_ex(
            &file->types[obj->type_id_or_index], &node_idx, &stream,
            &shader_value, TYPETREE_PARSE_PACK_COMPRESSED_BLOB)) return;
    
    if (stream.position != shader_size ||
        node_idx != file->types[obj->type_id_or_index].node_count) {
        typetree_free_value(&shader_value);
        return;
    }
    SerializedShaderSchemaProfile shader_profile;
    if (!serialized_shader_profile_from_unity_version(
            file->unity_version, &shader_profile)) {
        typetree_free_value(&shader_value);
        return;
    }
    SerializedShader shader;
    serialized_shader_init(&shader);
    if (!serialized_shader_parse_with_profile(
            &shader, &shader_value, shader_profile)) {
        typetree_free_value(&shader_value);
        return;
    }
    printf("[VERIFYING] Shader Name: %s, path_id=%lld\n", shader.name, (long long)obj->path_id);
    
    const TypeTreeValue* parsed_form = typetree_find_path(&shader_value, "m_ParsedForm");
    if (!parsed_form) parsed_form = &shader_value;
    
    const TypeTreeValue* platforms_val = find_field(&shader_value, parsed_form, "platforms");
    const TypeTreeValue* offsets_val = find_field(&shader_value, parsed_form, "offsets");
    const TypeTreeValue* comp_lens_val = find_field(&shader_value, parsed_form, "compressedLengths");
    const TypeTreeValue* decomp_lens_val = find_field(&shader_value, parsed_form, "decompressedLengths");
    const TypeTreeValue* blob_val = find_field(&shader_value, parsed_form, "compressedBlob");
    
    if (!platforms_val || !offsets_val || !comp_lens_val || !decomp_lens_val || !blob_val) {
        serialized_shader_free(&shader);
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
    if (platform_idx == -1) platform_idx = 0;
    
    const TypeTreeValue* arr_offsets_outer = typetree_get_array(offsets_val);
    const TypeTreeValue* arr_comp_lens_outer = typetree_get_array(comp_lens_val);
    const TypeTreeValue* arr_decomp_lens_outer = typetree_get_array(decomp_lens_val);
    
    if (!arr_offsets_outer || platform_idx >= arr_offsets_outer->array_val.count ||
        !arr_comp_lens_outer || platform_idx >= arr_comp_lens_outer->array_val.count ||
        !arr_decomp_lens_outer || platform_idx >= arr_decomp_lens_outer->array_val.count) {
        serialized_shader_free(&shader);
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
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    const TypeTreeValue* flat_blob = typetree_get_array(blob_val);
    if (!flat_blob) {
        serialized_shader_free(&shader);
        typetree_free_value(&shader_value);
        return;
    }
    
    int blob_len = flat_blob->array_val.count;
    uint8_t* compressed_data_flat = (uint8_t*)malloc(blob_len);
    if (!compressed_data_flat || !typetree_array_copy_bytes(
            flat_blob, 0, compressed_data_flat, (size_t)blob_len)) {
        free(compressed_data_flat);
        typetree_free_value(&shader_value);
        return;
    }
    
    int segment_count = arr_offsets->array_val.count;
    uint8_t** segments_local = (uint8_t**)malloc(segment_count * sizeof(uint8_t*));
    int* segment_lens = (int*)malloc(segment_count * sizeof(int));
    memset(segments_local, 0, segment_count * sizeof(uint8_t*));
    
    bool decompress_ok = true;
    for (int i = 0; i < segment_count; i++) {
        uint32_t offset = (uint32_t)arr_offsets->array_val.elements[i].int_val;
        uint32_t comp_len = (uint32_t)arr_comp_lens->array_val.elements[i].int_val;
        uint32_t decomp_len = (uint32_t)arr_decomp_lens->array_val.elements[i].int_val;
        segments_local[i] = (uint8_t*)malloc(decomp_len);
        segment_lens[i] = decomp_len;
        int dec_res = lz4_decompress_safe(compressed_data_flat + offset, segments_local[i], comp_len, decomp_len);
        if (dec_res < 0) { decompress_ok = false; break; }
    }
    
    if (decompress_ok && segment_count >= 1 && segment_lens[0] >= 4) {
        int32_t inner_entry_count;
        memcpy(&inner_entry_count, segments_local[0], 4);
        inner_entry_count = read_le32(inner_entry_count);
        BlobEntry* blob_entries_local = (BlobEntry*)malloc(inner_entry_count * sizeof(BlobEntry));
        int entry_offset = 4;
        for (int k = 0; k < inner_entry_count && entry_offset + 12 <= segment_lens[0]; k++) {
            int32_t off = 0, len = 0, seg = 0;
            memcpy(&off, segments_local[0] + entry_offset, 4);
            memcpy(&len, segments_local[0] + entry_offset + 4, 4);
            memcpy(&seg, segments_local[0] + entry_offset + 8, 4);
            blob_entries_local[k].offset = read_le32(off);
            blob_entries_local[k].length = read_le32(len);
            blob_entries_local[k].segment = read_le32(seg);
            entry_offset += 12;
        }
        
        // Loop over subprograms
        for (int subshader_idx = 0; subshader_idx < shader.subshader_count; subshader_idx++) {
            const SerializedSubShader* subshader = &shader.subshaders[subshader_idx];
            for (int pass_idx = 0; pass_idx < subshader->pass_count; pass_idx++) {
                const SerializedPass* pass = &subshader->passes[pass_idx];
                
                for (int stage_idx = 0; stage_idx < 2; stage_idx++) {
                    if (g_filter_stage != -1 && stage_idx != g_filter_stage) continue;
                    for (int sub_idx = 0; sub_idx < pass->subprogram_count[stage_idx]; sub_idx++) {
                        if (!serialized_pass_subprogram_is_platform(
                                pass, stage_idx, sub_idx, 4)) {
                            continue;
                        }
                        if (g_filter_sub_idx != -1 && sub_idx != g_filter_sub_idx) continue;
                        const SerializedSubProgram* ours_sub = &pass->subprograms[stage_idx][sub_idx];
                        int32_t b_idx = ours_sub->blob_index;
                        if (b_idx < 0 || b_idx >= inner_entry_count) continue;
                        
                        BlobEntry entry = blob_entries_local[b_idx];
                        if (entry.segment < 0 || entry.segment >= segment_count || !segments_local[entry.segment]) continue;
                        
                        const uint8_t* payload = segments_local[entry.segment] + entry.offset;
                        size_t payload_len = entry.length;
                        
                        ByteStream sub_stream;
                        stream_init(&sub_stream, payload, payload_len);
                        stream_set_endian(&sub_stream, false);
                        
                        PlayerSubProgramMetadata sub_meta;
                        if (subprogram_metadata_parse_variant(&sub_stream, &sub_meta)) {
                            
                            SerializedProgramParameters params;
                            serialized_program_parameters_init(&params);
                            bool has_params = false;
                            
                            int param_blob_idx = pass->subprogram_param_blob_indices[stage_idx][sub_idx];
                            if (param_blob_idx >= 0 && param_blob_idx < inner_entry_count) {
                                BlobEntry p_entry = blob_entries_local[param_blob_idx];
                                if (p_entry.segment >= 0 && p_entry.segment < segment_count && segments_local[p_entry.segment]) {
                                    const uint8_t* p_payload = segments_local[p_entry.segment] + p_entry.offset;
                                    size_t p_len = p_entry.length;
                                    
                                    ByteStream p_stream;
                                    stream_init(&p_stream, p_payload, p_len);
                                    stream_set_endian(&p_stream, false);
                                    
                                    if (subprogram_metadata_parse_parameters(&p_stream, &params)) {
                                        has_params = true;
                                    }
                                }
                            }
                            
                            g_shaders_total++;
                            printf("[VERIFYING] Shader object path_id=%lld, subprogram stage=%d, sub_idx=%d (blob_idx=%d)\n",
                                   (long long)obj->path_id, stage_idx, sub_idx, b_idx);
                            
                            if (verify_subprogram_compilation(&sub_meta, has_params ? &params : NULL, (long long)obj->path_id, stage_idx, sub_idx)) {
                                g_shaders_passed++;
                            } else {
                                LOG_ERROR("Verification failed for shader object path_id=%lld, stage=%d, subprogram_idx=%d",
                                          (long long)obj->path_id, stage_idx, sub_idx);
                            }
                            
                            serialized_program_parameters_free(&params);
                            subprogram_metadata_free_variant(&sub_meta);
                        }
                    }
                }
            }
        }
        
        free(blob_entries_local);
    }
    
    for (int i = 0; i < segment_count; i++) {
        if (segments_local[i]) free(segments_local[i]);
    }
    free(segments_local);
    free(segment_lens);
    free(compressed_data_flat);
    
    serialized_shader_free(&shader);
    typetree_free_value(&shader_value);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    system("rd /s /q failed_shaders 2>nul");
#else
    system("rm -rf failed_shaders");
#endif

    if (argc < 2) {
        printf("Usage: %s <path_to_collected_shaders.bundle> [target_path_id] [target_stage] [target_sub_idx]\n", argv[0]);
        return 1;
    }

    if (argc > 2) {
        g_filter_path_id = strtoll(argv[2], NULL, 10);
    }
    if (argc > 3) {
        g_filter_stage = atoi(argv[3]);
    }
    if (argc > 4) {
        g_filter_sub_idx = atoi(argv[4]);
    }

    // 1. Load D3DCompiler
    HMODULE mod = LoadLibraryA("d3dcompiler/D3DCompiler_47.dll");
    if (!mod) mod = LoadLibraryA("d3dcompiler\\D3DCompiler_47.dll");
    if (!mod) mod = LoadLibraryA("../d3dcompiler/D3DCompiler_47.dll");
    if (!mod) mod = LoadLibraryA("D3DCompiler_47.dll");
    if (!mod) {
        printf("[FAIL] Failed to load D3DCompiler_47.dll, error: %lu\n", GetLastError());
        return 1;
    }

    // 2. Resolve D3DCompile and D3DDisassemble entrypoints
    FARPROC compile_proc = GetProcAddress(mod, "D3DCompile");
    FARPROC disassemble_proc = GetProcAddress(mod, "D3DDisassemble");
    if (!compile_proc || !disassemble_proc) {
        printf("[FAIL] Failed to resolve D3DCompile or D3DDisassemble entrypoints!\n");
        FreeLibrary(mod);
        return 1;
    }
    _Static_assert(sizeof(g_D3DCompile) == sizeof(compile_proc),
                   "Windows function pointer size mismatch");
    _Static_assert(sizeof(g_D3DDisassemble) == sizeof(disassemble_proc),
                   "Windows function pointer size mismatch");
    memcpy(&g_D3DCompile, &compile_proc, sizeof(g_D3DCompile));
    memcpy(&g_D3DDisassemble, &disassemble_proc,
           sizeof(g_D3DDisassemble));
    printf("[INFO] Successfully loaded D3DCompiler_47.dll!\n");

    // 3. Initialize D3DCompiler runtime hooks
    init_d3dcompiler_hooks(mod);

    // 4. Load bundle archive
    size_t file_size = 0;
    uint8_t* file_data = read_file_to_buffer(argv[1], &file_size);
    if (!file_data) {
        printf("[FAIL] Failed to read bundle file: %s\n", argv[1]);
        shutdown_d3dcompiler_hooks();
        FreeLibrary(mod);
        return 1;
    }

    BundleArchive archive;
    if (!bundle_open(&archive, file_data, file_size)) {
        printf("[FAIL] Failed to parse BundleArchive from: %s\n", argv[1]);
        free(file_data);
        shutdown_d3dcompiler_hooks();
        FreeLibrary(mod);
        return 1;
    }

    printf("[INFO] Processing asset bundle files...\n");

    // 5. Traverse and compile verification loop
    for (int i = 0; i < archive.directory_count; i++) {
        size_t sub_file_size = 0;
        const uint8_t* sub_file_data = bundle_get_file(&archive, archive.directories[i].name, &sub_file_size);
        if (sub_file_data && sub_file_size > 0) {
            SerializedFile file;
            if (serialized_file_open(&file, sub_file_data, sub_file_size)) {
                for (int obj_idx = 0; obj_idx < file.object_count; obj_idx++) {
                    const AssetObjectInfo* obj = &file.objects[obj_idx];
                    if (obj->type_id == 48) { // 48 = Shader
                        process_shader_object(&file, obj);
                    }
                }
                serialized_file_close(&file);
            }
        }
    }

    // Cleanup
    bundle_close(&archive);
    free(file_data);
    
    // Shutdown hooks before unloading DLL
    shutdown_d3dcompiler_hooks();
    FreeLibrary(mod);

    printf("\n=== HLSL Translation Compilation Verification Summary ===\n");
    printf("Total shaders processed: %d\n", g_shaders_total);
    printf("Total shaders compiled successfully: %d\n", g_shaders_passed);
    printf("Total shaders matched 1:1: %d\n", g_shaders_matched);

    if (g_shaders_matched == g_shaders_total && g_shaders_total > 0) {
        printf("[SUCCESS] All translated shaders compiled and matched 1:1 perfectly!\n");
        return 0;
    } else {
        printf("[FAIL] Verification/matching failed or no shaders were processed!\n");
        return 1;
    }
}
