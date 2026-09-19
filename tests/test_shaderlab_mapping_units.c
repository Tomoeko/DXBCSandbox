// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_shaderlab_mapping.h"

#include <limits.h>
#include <stdio.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    SerializedPass passes[6] = {0};
    SerializedSubShader subshaders[2] = {0};
    SerializedShader shader = {0};
    int d3d = 4;
    int glcore = 15;
    SerializedSubProgram d3d_program = {0};
    SerializedSubProgram gl_program = {0};
    d3d_program.program_type = 15;
    gl_program.program_type = 6;
    for (int i = 0; i < 6; ++i) {
        if (i != 0 && i != 3 && i != 5)
            continue;
        passes[i].has_serialized_platforms = true;
        passes[i].platform_count = 1;
        passes[i].platforms = i == 3 ? &glcore : &d3d;
        passes[i].subprogram_count[0] = 1;
        passes[i].subprograms[0] = i == 3 ? &gl_program : &d3d_program;
    }
    passes[0].state.gpuProgramID = 501;
    passes[5].state.gpuProgramID = 602;
    passes[1].pass_type = 1;
    passes[2].pass_type = 2;
    subshaders[0].passes = passes;
    subshaders[0].pass_count = 4;
    subshaders[1].passes = passes + 4;
    subshaders[1].pass_count = 2;
    shader.subshaders = subshaders;
    shader.subshader_count = 2;

    /* Unknown authority remains in the denominator. Only the explicit foreign
     * pass and the non-program pass kinds are omitted from ordinal routing. */
    CHECK(unity_shaderlab_snippet_count(&shader) == 3);
    const int indices[] = {0, -1, -1, -1, 1, 2};
    PreprocessedSnippet snippets[4] = {0};
    for (int i = 0; i < 4; ++i) {
        snippets[i].source = "controlled source";
        snippets[i].has_contract = true;
        snippets[i].gpu_program_id = 100 - i;
    }
    PreprocessResult generated = {.snippets = snippets, .snippet_count = 3};
    for (int i = 0; i < 6; ++i) {
        CHECK(unity_shaderlab_pass_at(&shader, i) == &passes[i]);
        CHECK(unity_shaderlab_pass_snippet_index(&shader, i) == indices[i]);
        int found = 42;
        const PreprocessedSnippet *snippet =
            unity_shaderlab_find_generated_snippet(&shader, i, &generated, &found);
        CHECK(found == indices[i]);
        CHECK(snippet == (indices[i] < 0 ? NULL : &snippets[indices[i]]));
    }
    CHECK(unity_shaderlab_pass_has_d3d11(&passes[0]));
    CHECK(!unity_shaderlab_pass_has_d3d11(&passes[3]));
    CHECK(!unity_shaderlab_pass_has_d3d11(&passes[4]));
    CHECK(unity_shaderlab_pass_emits_snippet(&passes[4]));
    CHECK(!unity_shaderlab_pass_emits_snippet(NULL));
    CHECK(!unity_shaderlab_pass_has_d3d11(NULL));
    passes[1].pass_type = 0;
    passes[1].use_name = "Dependency/PASS";
    CHECK(unity_shaderlab_snippet_count(&shader) == 3);
    passes[3].has_serialized_platforms = false;
    CHECK(unity_shaderlab_snippet_count(&shader) == 4);
    CHECK(unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) == NULL);
    passes[3].has_serialized_platforms = true;

    for (int count = 0; count <= 4; ++count) {
        generated.snippet_count = count;
        CHECK((unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) != NULL) ==
              (count == 3));
    }
    generated.snippet_count = 3;
    snippets[0].has_contract = false;
    CHECK(unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) == NULL);
    snippets[0].has_contract = true;
    snippets[0].source = NULL;
    CHECK(unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) == NULL);
    snippets[0].source = "controlled source";
    generated.snippets = NULL;
    CHECK(unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) == NULL);
    CHECK(unity_shaderlab_find_original_snippet(&shader, 0, &generated) == NULL);
    generated.snippets = snippets;
    generated.snippet_count = -1;
    CHECK(unity_shaderlab_find_generated_snippet(&shader, 0, &generated, NULL) == NULL);
    CHECK(unity_shaderlab_find_original_snippet(&shader, 0, &generated) == NULL);
    generated.snippet_count = 3;

    /* Original source is queried by its unique recorded GPU program ID even
     * when its order differs. A duplicate ID can never choose the first row. */
    snippets[0].gpu_program_id = 602;
    snippets[1].gpu_program_id = 501;
    CHECK(unity_shaderlab_find_original_snippet(&shader, 0, &generated) == &snippets[1]);
    CHECK(unity_shaderlab_find_original_snippet(&shader, 5, &generated) == &snippets[0]);
    snippets[2].gpu_program_id = 501;
    CHECK(unity_shaderlab_find_original_snippet(&shader, 0, &generated) == NULL);
    CHECK(unity_shaderlab_pass_at(&shader, -1) == NULL);
    CHECK(unity_shaderlab_pass_at(&shader, 6) == NULL);
    CHECK(unity_shaderlab_pass_snippet_index(&shader, 6) == -1);

    for (int mutation = 0; mutation < 6; ++mutation) {
        SerializedShader bad = shader;
        SerializedSubShader bad_subshaders[2] = {subshaders[0], subshaders[1]};
        bad.subshaders = bad_subshaders;
        switch (mutation) {
        case 0:
            bad.subshader_count = -1;
            break;
        case 1:
            bad.subshaders = NULL;
            break;
        case 2:
            bad_subshaders[0].pass_count = -1;
            break;
        case 3:
            bad_subshaders[0].passes = NULL;
            break;
        case 4:
            bad_subshaders[0].pass_count = INT_MAX;
            break;
        case 5:
            bad_subshaders[1].passes = NULL;
            break;
        }
        CHECK(unity_shaderlab_snippet_count(&bad) == -1);
        CHECK(unity_shaderlab_pass_at(&bad, 0) == NULL);
        CHECK(unity_shaderlab_pass_snippet_index(&bad, 0) == -1);
        CHECK(unity_shaderlab_find_generated_snippet(&bad, 0, &generated, NULL) == NULL);
        CHECK(unity_shaderlab_find_original_snippet(&bad, 0, &generated) == NULL);
    }
    CHECK(unity_shaderlab_snippet_count(NULL) == -1);
    puts("ShaderLab snippet mapping tests passed.");
    return 0;
}
