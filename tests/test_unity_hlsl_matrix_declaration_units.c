// SPDX-License-Identifier: GPL-3.0-only
#include "compiler/unity_hlsl_matrix_declaration.h"
#include "common/shader_stage.h"
#include "dxbc/dxbc_hash.h"
#include <stdio.h>
#include <string.h>
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); return 1; } } while (0)
static void word(uint8_t *bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[index] = (uint8_t)(value >> (index * 8));
}
/* A correctly hashed complete vertex RET-only container. It grants neither
 * a matrix witness nor source coverage, and must not launch the lazy broker. */
static int no_read_and_target_failures(void) {
    uint8_t bytes[96] = {0}, digest[16];
    memcpy(bytes, "DXBC", 4); word(bytes + 20, 1); word(bytes + 24, (uint32_t)sizeof(bytes));
    word(bytes + 28, 3); word(bytes + 32, 44); word(bytes + 36, 64); word(bytes + 40, 80);
    memcpy(bytes + 44, "SHDR", 4); word(bytes + 48, 12); word(bytes + 52, 0x00010050);
    word(bytes + 56, 3); word(bytes + 60, 0x0100003e);
    memcpy(bytes + 64, "ISGN", 4); word(bytes + 68, 8); word(bytes + 76, 8);
    memcpy(bytes + 80, "OSGN", 4); word(bytes + 84, 8); word(bytes + 92, 8);
    CHECK(dxbc_compute_hash(bytes, sizeof(bytes), digest)); memcpy(bytes + 4, digest, sizeof(digest));
    UnityCompilerBroker *broker = unity_compiler_broker_create_lazy(".", "."); CHECK(broker);
    SnippetCompileContract contract; unity_compiler_snippet_contract_init(&contract);
    UnityCompilerSnippetCompileRequest request = {.snippet_source = "void vert(){}\n", .source_directory = ".",
        .source_basename = "NoRead.shader", .pass_name = "", .shader_type = 0, .program_mask = 2,
        .platform = 4, .contract = &contract};
    PlayerSubProgramMetadata player = {.program_type = UNITY_GPU_PROGRAM_D3D11_VERTEX_SM50,
        .bytecode = bytes, .bytecode_length = (uint32_t)sizeof(bytes)};
    UnityCompileProfile profile = {0};
    UnityHlslMatrixDeclarationInput input = {.request = &request, .profile = &profile, .player = &player,
        .target = bytes, .target_size = sizeof(bytes)};
    UnityHlslMatrixDeclarationReceipt *receipt = NULL; UnityHlslMatrixDeclarationDiagnostic diagnostic;
    UnityHlslMatrixDeclarationStatus initial = unity_hlsl_matrix_declaration_capture(broker, &input, &receipt, &diagnostic);
    if (initial != UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE) fprintf(stderr, "initial status %d, reads %d\n", initial, diagnostic.read_status);
    CHECK(initial == UNITY_HLSL_MATRIX_DECLARATION_NOT_APPLICABLE);
    CHECK(!receipt && !diagnostic.compile_attempted && !diagnostic.expansion_attempted);
    CHECK(diagnostic.read_status == HLSL_CURRENT_MATRIX_NOT_APPLICABLE);
    player.program_type = UNITY_GPU_PROGRAM_D3D11_PIXEL_SM50;
    CHECK(unity_hlsl_matrix_declaration_capture(broker, &input, &receipt, &diagnostic) == UNITY_HLSL_MATRIX_DECLARATION_TARGET_INVALID);
    player.program_type = UNITY_GPU_PROGRAM_D3D11_VERTEX_SM50;
    input.target_size--;
    CHECK(unity_hlsl_matrix_declaration_capture(broker, &input, &receipt, &diagnostic) == UNITY_HLSL_MATRIX_DECLARATION_TARGET_INVALID);
    input.target_size++; bytes[60] ^= 1;
    CHECK(unity_hlsl_matrix_declaration_capture(broker, &input, &receipt, &diagnostic) == UNITY_HLSL_MATRIX_DECLARATION_TARGET_INVALID);
    bytes[60] ^= 1;
    SerializedProgramParameters invalid = {.cb_count = -1}; input.current_parameters = &invalid;
    CHECK(unity_hlsl_matrix_declaration_capture(broker, &input, &receipt, &diagnostic) == UNITY_HLSL_MATRIX_DECLARATION_METADATA_REJECTED);
    CHECK(!receipt && !diagnostic.compile_attempted && !diagnostic.expansion_attempted);
    UnityCompilerBrokerStats stats; unity_compiler_broker_get_stats(broker, &stats);
    CHECK(!stats.submitted_requests && !stats.compiler_process_starts);
    CHECK(!unity_hlsl_matrix_declaration_replay(broker, NULL, &input));
    CHECK(!unity_hlsl_matrix_declaration_describe(NULL, NULL));
    CHECK(!unity_hlsl_matrix_declaration_field(NULL, 0, NULL));
    CHECK(!unity_hlsl_matrix_declaration_read(NULL, 0, NULL));
    unity_hlsl_matrix_declaration_free(NULL);
    unity_compiler_snippet_contract_free(&contract); unity_compiler_broker_destroy(broker);
    return 0;
}
int main(void) {
    if (no_read_and_target_failures()) return 1;
    puts("matrix receipt absence/target/metadata fail-closed units passed"); return 0;
}
