#include "test_hlsl_compile_verify.h"
#include "d3dcompiler_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "translation/hlsl_emitter_internal.h"
#include "dxbc/dxbc_parser.h"
#include "translation/usil.h"
#include "translation/hlsl_emitter.h"

pfn_D3DCompile g_D3DCompile = NULL;
pfn_D3DDisassemble g_D3DDisassemble = NULL;

int g_shaders_total = 0;
int g_shaders_passed = 0;
int g_shaders_matched = 0;

static void sanitize_line(char* out, const char* in) {
    while (*in == ' ' || *in == '\t') in++;
    char* p = out;
    while (*in && *in != '\r' && *in != '\n') {
        if (*in == '/' && *(in + 1) == '/') {
            break;
        }
        *p++ = *in++;
    }
    *p = '\0';
    p--;
    while (p >= out && (*p == ' ' || *p == '\t')) {
        *p = '\0';
        p--;
    }
}

static void sort_constant_buffers(char** lines, int count) {
    int* indices = malloc(count * sizeof(int));
    if (!indices) return;
    int cb_count = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(lines[i], "dcl_constantbuffer", 18) == 0) {
            indices[cb_count++] = i;
        }
    }
    for (int i = 0; i < cb_count - 1; i++) {
        for (int j = i + 1; j < cb_count; j++) {
            int idx_a = indices[i];
            int idx_b = indices[j];
            const char* cb_a = strstr(lines[idx_a], "CB");
            const char* cb_b = strstr(lines[idx_b], "CB");
            int val_a = cb_a ? atoi(cb_a + 2) : 0;
            int val_b = cb_b ? atoi(cb_b + 2) : 0;
            if (val_a > val_b) {
                char* temp = lines[idx_a];
                lines[idx_a] = lines[idx_b];
                lines[idx_b] = temp;
            }
        }
    }
    free(indices);
}

bool compare_disassembly(const char* orig_asm, const char* gen_asm, long long path_id, int stage, int sub_idx) {
    char** orig_lines = malloc(2000 * sizeof(char*));
    char** gen_lines = malloc(2000 * sizeof(char*));
    int orig_count = 0;
    int gen_count = 0;
    
    if (!orig_lines || !gen_lines) {
        free(orig_lines);
        free(gen_lines);
        return false;
    }
    
    const char* p = orig_asm;
    while (*p && orig_count < 2000) {
        char line[512];
        char* l = line;
        while (*p && *p != '\n' && (l - line) < 511) {
            *l++ = *p++;
        }
        if (*p == '\n') p++;
        *l = '\0';
        
        char clean[512];
        sanitize_line(clean, line);
        if (clean[0] != '\0') {
            orig_lines[orig_count++] = strdup(clean);
        }
    }
    
    p = gen_asm;
    while (*p && gen_count < 2000) {
        char line[512];
        char* l = line;
        while (*p && *p != '\n' && (l - line) < 511) {
            *l++ = *p++;
        }
        if (*p == '\n') p++;
        *l = '\0';
        
        char clean[512];
        sanitize_line(clean, line);
        if (clean[0] != '\0') {
            gen_lines[gen_count++] = strdup(clean);
        }
    }
    
    sort_constant_buffers(orig_lines, orig_count);
    sort_constant_buffers(gen_lines, gen_count);
    
    bool match = true;
    if (orig_count != gen_count) {
        match = false;
    } else {
        for (int i = 0; i < orig_count; i++) {
            if (strcmp(orig_lines[i], gen_lines[i]) != 0) {
                match = false;
                break;
            }
        }
    }
    
    if (!match) {
        CreateDirectoryA("failed_shaders", NULL);
        char filename_orig[256];
        char filename_gen[256];
        snprintf(filename_orig, sizeof(filename_orig), "failed_shaders/mismatch_%lld_%d_%d_original.asm", path_id, stage, sub_idx);
        snprintf(filename_gen, sizeof(filename_gen), "failed_shaders/mismatch_%lld_%d_%d_generated.asm", path_id, stage, sub_idx);
        
        FILE* f_orig = fopen(filename_orig, "w");
        if (f_orig) {
            for (int i = 0; i < orig_count; i++) {
                fprintf(f_orig, "%s\n", orig_lines[i]);
            }
            fclose(f_orig);
        }
        
        FILE* f_gen = fopen(filename_gen, "w");
        if (f_gen) {
            for (int i = 0; i < gen_count; i++) {
                fprintf(f_gen, "%s\n", gen_lines[i]);
            }
            fclose(f_gen);
        }
    }
    
    for (int i = 0; i < orig_count; i++) free(orig_lines[i]);
    for (int i = 0; i < gen_count; i++) free(gen_lines[i]);
    free(orig_lines);
    free(gen_lines);
    
    return match;
}

bool verify_subprogram_compilation(
    const PlayerSubProgramMetadata* sub_meta,
    const SerializedProgramParameters* params,
    long long path_id,
    int stage,
    int sub_idx
) {
    if (sub_meta->bytecode_length == 0 || sub_meta->bytecode == NULL) {
        return true;
    }

    DXBCContainer dxbc;
    memset(&dxbc, 0, sizeof(DXBCContainer));
    if (!dxbc_parse(&dxbc, sub_meta->bytecode, sub_meta->bytecode_length)) {
        LOG_ERROR("Failed to parse DXBC bytecode");
        return false;
    }

    USILProgram usil;
    memset(&usil, 0, sizeof(USILProgram));
    if (!usil_translate(&usil, &dxbc)) {
        LOG_ERROR("Failed to translate DXBC to USIL");
        dxbc_free(&dxbc);
        return false;
    }

    StringBuilder sb;
    sb_init(&sb);
    const HLSLEmitOptions emit_options =
        HLSL_EMIT_RECOMPILE_OPTIONS_INIT;
    bool emit_success = hlsl_emit_with_options(
        &usil, &sb, params, NULL, NULL, &emit_options);
    if (!emit_success) {
        LOG_ERROR("Failed to emit HLSL from USIL");
        usil_free(&usil);
        dxbc_free(&dxbc);
        sb_free(&sb);
        return false;
    }

    const char* hlsl_src = sb.buf;

    ID3DBlob* code_blob = NULL;
    ID3DBlob* error_blob = NULL;
    UINT flags1 = 0;
    if (strstr(dxbc.shader_type_model, "_5_0")) {
        flags1 = 0x9000;
    } else {
        flags1 = 0x1000;
    }

    const char* hook_log = getenv("DXBC_D3D_HOOK_LOG");
    g_enable_hook_logging = hook_log && strcmp(hook_log, "1") == 0;

    HRESULT hr = g_D3DCompile(
        hlsl_src,
        strlen(hlsl_src),
        NULL,
        NULL,
        NULL,
        "main",
        dxbc.shader_type_model,
        flags1,
        0,
        &code_blob,
        &error_blob
    );

    g_enable_hook_logging = false;

    bool success = false;
    if (SUCCEEDED(hr)) {
        success = true;
        if (code_blob) {
            printf("    [COMPILE OK] Target %s compiled successfully! (Size: %zu bytes)\n",
                   dxbc.shader_type_model, (size_t)code_blob->lpVtbl->GetBufferSize(code_blob));
            
            DXBCContainerView original;
            ID3DBlob* orig_dis = NULL;
            ID3DBlob* gen_dis = NULL;
            HRESULT hr_orig = E_FAIL;
            if (dxbc_container_view_first(sub_meta->bytecode,
                                          sub_meta->bytecode_length,
                                          &original)) {
                hr_orig = g_D3DDisassemble(original.data, original.size, 0,
                                           NULL, &orig_dis);
            }
            HRESULT hr_gen = g_D3DDisassemble(code_blob->lpVtbl->GetBufferPointer(code_blob), code_blob->lpVtbl->GetBufferSize(code_blob), 0, NULL, &gen_dis);
            
            if (SUCCEEDED(hr_orig) && SUCCEEDED(hr_gen)) {
                const char* orig_asm = (const char*)orig_dis->lpVtbl->GetBufferPointer(orig_dis);
                const char* gen_asm = (const char*)gen_dis->lpVtbl->GetBufferPointer(gen_dis);
                
                if (compare_disassembly(orig_asm, gen_asm, path_id, stage, sub_idx)) {
                    printf("    [MATCH 1:1] Disassembly matches original perfectly!\n");
                    g_shaders_matched++;
                } else {
                    printf("    [MISMATCH] Disassembly differs from original!\n");
                    
                    CreateDirectoryA("failed_shaders", NULL);
                    char filename[256];
                    snprintf(filename, sizeof(filename), "failed_shaders/failed_shader_%lld_%d_%d.hlsl", path_id, stage, sub_idx);
                    FILE* f_hlsl = fopen(filename, "w");
                    if (f_hlsl) {
                        fprintf(f_hlsl, "%s", hlsl_src);
                        fclose(f_hlsl);
                        printf("    [DEBUG] Wrote failed HLSL to %s\n", filename);
                    } else {
                        printf("    [DEBUG] Failed to open %s for writing\n", filename);
                        perror("    [DEBUG] fopen error");
                    }
                    printf("=== GENERATED HLSL START ===\n%s\n=== GENERATED HLSL END ===\n", hlsl_src);
                }
            } else {
                printf("    [WARNING] Disassembly failed (hr_orig=0x%08x, hr_gen=0x%08x)\n", (unsigned int)hr_orig, (unsigned int)hr_gen);
            }
            
            if (orig_dis) orig_dis->lpVtbl->Release(orig_dis);
            if (gen_dis) gen_dis->lpVtbl->Release(gen_dis);
            code_blob->lpVtbl->Release(code_blob);
        }
    } else {
        success = false;
        printf("    [COMPILE FAIL] Target %s compilation failed with HRESULT 0x%08x\n",
               dxbc.shader_type_model, (unsigned int)hr);
        
        CreateDirectoryA("failed_shaders", NULL);
        char filename[256];
        snprintf(filename, sizeof(filename), "failed_shaders/failed_shader_%lld_%d_%d.hlsl", path_id, stage, sub_idx);
        FILE* f_hlsl = fopen(filename, "w");
        if (f_hlsl) {
            fprintf(f_hlsl, "%s", hlsl_src);
            fclose(f_hlsl);
            printf("    [DEBUG] Wrote failed HLSL to %s\n", filename);
        }

        if (error_blob) {
            const char* error_str = (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob);
            printf("    [COMPILER ERROR MESSAGE]:\n%s\n", error_str);
            error_blob->lpVtbl->Release(error_blob);
        }
    }

    usil_free(&usil);
    dxbc_free(&dxbc);
    sb_free(&sb);

    return success;
}
