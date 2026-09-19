#include "compiler/unity_reflection_certificate.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                                     \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct {
    PlayerSubProgramMetadata player;
    PlayerSubProgramBindChannel input;
    SerializedProgramParameters parameters;
    SerializedConstantBuffer buffer;
    SerializedVariable variables[3];
    SerializedResourceParam resources[5];
} ReflectionFixture;

static void initialize_fixture(ReflectionFixture* fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->input.channel = 0U;
    fixture->input.component = 0U;
    fixture->player.source_map = 1U;
    fixture->player.binding_count = 1;
    fixture->player.bindings = &fixture->input;

    fixture->parameters.is_binary = false;
    fixture->parameters.cb_count = 1;
    fixture->parameters.constant_buffers = &fixture->buffer;
    fixture->parameters.res_count = 5;
    fixture->parameters.resources = fixture->resources;
    fixture->buffer.name = "ProbeCB";
    fixture->buffer.size = 112U;
    fixture->buffer.var_count = 3;
    fixture->buffer.variables = fixture->variables;

    fixture->variables[0].name = "ProbeColor";
    fixture->variables[0].layout[0] = 0U;
    fixture->variables[0].layout[2] = 0U;
    fixture->variables[0].layout[3] = 4U;

    fixture->variables[1].name = "ProbeMatrix";
    fixture->variables[1].layout[0] = 16U;
    fixture->variables[1].layout[2] = 0U;
    fixture->variables[1].layout[3] = 4U;
    fixture->variables[1].layout[4] = 1U;

    fixture->variables[2].name = "ProbeValues";
    fixture->variables[2].layout[0] = 80U;
    fixture->variables[2].layout[1] = 2U;
    fixture->variables[2].layout[2] = 0U;
    fixture->variables[2].layout[3] = 1U;

    fixture->resources[0] = (SerializedResourceParam){
        .name = "ProbeCB",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER,
        .bind_index = 0U,
        .array_size = 1U,
        .extra = {1U, 0U},
    };
    fixture->resources[1] = (SerializedResourceParam){
        .name = "ProbeTex",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 0U,
        .dimension = 2U,
        .sampler_index = 0U,
        .extra = {0U, 4U},
    };
    fixture->resources[2] = (SerializedResourceParam){
        .name = "",
        .bind_type = SERIALIZED_RESOURCE_SAMPLER,
        .bind_index = 0U,
        .sampler_state = 84U,
        .extra = {84U, 0U},
    };
    fixture->resources[3] = (SerializedResourceParam){
        .name = "ProbeInput",
        .bind_type = SERIALIZED_RESOURCE_BUFFER,
        .bind_index = 1U,
        .array_size = 1U,
        .extra = {1U, 0U},
    };
    fixture->resources[4] = (SerializedResourceParam){
        .name = "ProbeOutput",
        .bind_type = SERIALIZED_RESOURCE_UAV,
        .bind_index = 0U,
        .original_index = 3U,
        .extra = {3U, 0U},
    };
}

static int parse_records(
    const char* const* text, size_t count,
    UnityCompilerReflectionRecord* records) {
    memset(records, 0, count * sizeof(*records));
    for (size_t index = 0U; index < count; ++index) {
        if (!unity_compiler_reflection_record_parse(text[index],
                                                    &records[index])) {
            for (size_t prior = 0U; prior < index; ++prior) {
                unity_compiler_reflection_record_free(&records[prior]);
            }
            return 0;
        }
    }
    return 1;
}

static void free_records(
    UnityCompilerReflectionRecord* records, size_t count) {
    for (size_t index = 0U; index < count; ++index) {
        unity_compiler_reflection_record_free(&records[index]);
    }
}

static int test_complete_binding_certificate(void) {
    ReflectionFixture fixture;
    initialize_fixture(&fixture);
    static const char* const text[] = {
        "input: 0 0",
        "cb: ProbeCB 112 3",
        "const: ProbeColor 0 0 0 1 4 0",
        "const: ProbeMatrix 16 0 1 4 4 0",
        "const: ProbeValues 80 0 0 1 1 2",
        "cbbind: ProbeCB 0",
        "texbind: ProbeTex 0 0 0 2",
        "sampler: 84 0",
        "bufferbind: ProbeInput 1 1",
        "uavbind: ProbeOutput 0 3",
        "stats: 8 1 0 2",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_record_count == 10U);
    CHECK(report.expected_non_input_record_count == 9U);
    CHECK(report.observed_record_count == 10U);
    CHECK(report.matched_record_count == 10U);
    CHECK(report.ignored_stats_record_count == 1U);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(report.expected_bindings_digest, report.observed_bindings_digest, 32) == 0);
    uint8_t baseline_digest[32];
    memcpy(baseline_digest, report.expected_bindings_digest, 32);

    /* Resource order and constant order within one buffer are immaterial. */
    UnityCompilerReflectionRecord swap = records[2];
    records[2] = records[4];
    records[4] = swap;
    swap = records[6];
    records[6] = records[9];
    records[9] = swap;
    CHECK(unity_reflection_certify_d3d11_bindings(
        &fixture.player, &fixture.parameters, NULL, records, 10U, &report) ==
        UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.observed_bindings_digest_valid);
    CHECK(memcmp(baseline_digest, report.observed_bindings_digest, 32) == 0);
    swap = records[2]; records[2] = records[4]; records[4] = swap;
    swap = records[6]; records[6] = records[9]; records[9] = swap;

    records[6].values[0] ^= 1;
    CHECK(unity_reflection_certify_d3d11_bindings(
        &fixture.player, &fixture.parameters, NULL, records, 11U, &report) ==
        UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(report.expected_bindings_digest, report.observed_bindings_digest, 32) != 0);
    records[6].values[0] ^= 1;

    size_t saved_count = records[6].value_count;
    records[6].value_count = UNITY_COMPILER_REFLECTION_MAX_VALUES + 1U;
    CHECK(unity_reflection_certify_d3d11_bindings(
        &fixture.player, &fixture.parameters, NULL, records, 11U, &report) ==
        UNITY_REFLECTION_CERTIFICATE_INVALID_ARGUMENT);
    CHECK(!report.expected_bindings_digest_valid && !report.observed_bindings_digest_valid);
    records[6].value_count = saved_count;


    /* Unity discards the callback's CB variable count during serialization.
     * It is neither an equality constraint nor a bound on retained children. */
    UnityCompilerReflectionRecord larger_member_count;
    CHECK(unity_compiler_reflection_record_parse(
        "cb: ProbeCB 112 -214", &larger_member_count));
    UnityCompilerReflectionRecord saved_cb = records[1];
    records[1] = larger_member_count;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.observed_bindings_digest_valid);
    CHECK(memcmp(baseline_digest, report.observed_bindings_digest, 32) == 0);
    unity_compiler_reflection_record_free(&records[1]);
    records[1] = saved_cb;

    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records + 1,
              sizeof(records) / sizeof(records[0]) - 1U, &report) ==
          UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD);
    CHECK(report.expected_record_index != SIZE_MAX);

    /* Common entries are an intersection and therefore mandatory, not a
     * permissive union of dormant resources. */
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records, 9U,
              &report) == UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD);
    CHECK(report.expected_record.present);
    CHECK(report.expected_record.kind ==
          UNITY_COMPILER_REFLECTION_UAV_BINDING);

    UnityCompilerReflectionRecord replacement;
    CHECK(unity_compiler_reflection_record_parse(
        "const: WrongColor 0 0 0 1 4 0", &replacement));
    UnityCompilerReflectionRecord saved = records[2];
    records[2] = replacement;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.observed_record_index == 2U);
    unity_compiler_reflection_record_free(&records[2]);
    records[2] = saved;

    /* Resource identifiers are runtime binding identity. When an emitter
     * substitutes t#/u# but preserves the register tuple, report the exact
     * expected name alongside the observed one while retaining hard failure. */
    CHECK(unity_compiler_reflection_record_parse(
        "bufferbind: t1 1 1", &replacement));
    saved = records[8];
    records[8] = replacement;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.expected_record.present && report.observed_record.present);
    CHECK(strcmp(report.expected_record.name_preview, "ProbeInput") == 0);
    CHECK(strcmp(report.observed_record.name_preview, "t1") == 0);
    CHECK(report.expected_record.values[0] == 1 &&
          report.expected_record.values[1] == 1);
    unity_compiler_reflection_record_free(&records[8]);
    records[8] = saved;

    CHECK(unity_compiler_reflection_record_parse(
        "cb: ProbeCB 128 3", &replacement));
    saved = records[1];
    records[1] = replacement;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.expected_record.present && report.observed_record.present);
    CHECK(report.expected_record.kind ==
          UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER);
    CHECK(strcmp(report.expected_record.name_preview, "ProbeCB") == 0);
    CHECK(report.expected_record.values[0] == 112);
    CHECK(report.observed_record.values[0] == 128);
    CHECK(report.expected_record.name_length == 7U);
    CHECK(memcmp(report.expected_record.name_sha256,
                 report.observed_record.name_sha256,
                 COMMON_SHA256_DIGEST_SIZE) == 0);
    unity_compiler_reflection_record_free(&records[1]);
    records[1] = saved;

    fixture.resources[1].extra[1] ^= 1U;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    fixture.resources[1].extra[1] ^= 1U;

    fixture.resources[0].array_size = 2U;
    fixture.resources[0].extra[0] = 2U;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    fixture.resources[0].extra[0] = 1U;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    fixture.resources[0].array_size = 1U;
    fixture.resources[0].extra[0] = 1U;

    fixture.resources[3].array_size = 2U;
    fixture.resources[3].extra[0] = 2U;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    fixture.resources[3].array_size = 1U;
    fixture.resources[3].extra[0] = 1U;

    fixture.resources[2].name = "NamedSampler";
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    fixture.resources[2].name = "";

    fixture.resources[2].sampler_state = UINT32_C(0x1000);
    fixture.resources[2].extra[0] = UINT32_C(0x1000);
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    fixture.resources[2].sampler_state = 84U;
    fixture.resources[2].extra[0] = 84U;

    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}

static int test_signed_sampler_sentinel_and_struct(void) {
    ReflectionFixture fixture;
    initialize_fixture(&fixture);
    fixture.player.binding_count = 0;
    fixture.player.source_map = 0U;
    fixture.parameters.res_count = 1;
    fixture.resources[0] = (SerializedResourceParam){
        .name = "DepthTex",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 2U,
        .sampler_index = UINT32_MAX,
        .dimension = 2U,
        .extra = {UINT32_MAX, 4U},
    };
    fixture.buffer.var_count = 0;
    fixture.buffer.size = 144U;
    SerializedVariable member = {
        .name = "member",
        .layout = {0U, 0U, 0U, 4U, 0U, 0U},
    };
    SerializedStructParam structure = {
        .name = "ProbeStruct",
        .layout = {112U, 2U, 16U},
        .member_count = 1,
        .members = &member,
    };
    fixture.buffer.struct_count = 1;
    fixture.buffer.struct_params = &structure;
    static const char* const text[] = {
        "cb: ProbeCB 144 1",
        "const: ProbeStruct 112 0 2 16 1 2",
        "const: ProbeStruct.member 0 0 0 1 4 0",
        "texbind: DepthTex 2 -1 0 2",
        "stats: 0 0 0 0",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(report.expected_bindings_digest, report.observed_bindings_digest, 32) == 0);

    /* Player residual blobs store the bare member too; Unity reconstructs
     * the callback spelling from the containing struct name. */
    fixture.parameters.is_binary = true;
    member.name = "member";
    memcpy(member.layout,
           (const uint32_t[]){0U, 1U, 4U, 0U, 0U, 0U},
           sizeof(member.layout));
    SerializedProgramParameters empty_common;
    serialized_program_parameters_init(&empty_common);
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &empty_common, &fixture.parameters, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(report.expected_bindings_digest, report.observed_bindings_digest, 32) == 0);
    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}

static int test_common_plus_residual_composition(void) {
    PlayerSubProgramBindChannel input = {0U, 0U};
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));
    player.binding_count = 1;
    player.bindings = &input;
    player.source_map = 1U;

    SerializedVariable common_variables[2] = {
        {
            .name = "unity_MatrixVP",
            .layout = {272U, 0U, 0U, 4U, 1U, 0U},
        },
        {
            .name = "FamilyOnlyValue",
            .layout = {352U, 0U, 0U, 4U, 0U, 0U},
        },
    };
    SerializedConstantBuffer common_buffer = {
        .name = "UnityPerFrame",
        .size = 368U,
        .has_is_partial = true,
        .is_partial = true,
        .var_count = 2,
        .variables = common_variables,
    };
    SerializedResourceParam common_binding = {
        .name = "UnityPerFrame",
        .bind_type = SERIALIZED_RESOURCE_CONSTANT_BUFFER,
        .bind_index = 1U,
        .array_size = 1U,
        .extra = {1U, 0U},
    };
    SerializedProgramParameters common;
    serialized_program_parameters_init(&common);
    common.cb_count = 1;
    common.constant_buffers = &common_buffer;
    common.res_count = 1;
    common.resources = &common_binding;

    SerializedConstantBuffer residual_buffer = {
        .name = "UnityPerFrame",
        .size = 336U,
    };
    SerializedResourceParam residual_texture = {
        .name = "ProbeTex",
        .bind_type = SERIALIZED_RESOURCE_TEXTURE,
        .bind_index = 0U,
        .sampler_index = UINT32_MAX,
        .dimension = 2U,
        .extra = {UINT32_MAX, 4U},
    };
    SerializedProgramParameters residual;
    serialized_program_parameters_init(&residual);
    residual.is_binary = true;
    residual.cb_count = 1;
    residual.constant_buffers = &residual_buffer;
    residual.res_count = 1;
    residual.resources = &residual_texture;

    static const char* const text[] = {
        "input: 0 0",
        "cb: UnityPerFrame 336 18",
        "const: unity_MatrixVP 272 0 1 4 4 0",
        "cbbind: UnityPerFrame 1",
        "texbind: ProbeTex 0 -1 0 2",
        "stats: 4 1 0 1",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.authority ==
          UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL);
    CHECK(report.expected_record_count == 5U);
    CHECK(report.observed_record_count == 5U);
    CHECK(report.matched_record_count == 5U);

    /* The count is irrecoverable, but the merged shell size is exact. */
    records[1].values[1] = -999;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    records[1].values[0] = 368;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    records[1].values[0] = 336;

    /* A partial common buffer describes a shader-family superset.  Its
     * larger family size is not a competing shell authority: the selected
     * binary residual supplies the exact 336-byte shell, and the variable
     * beginning at byte 352 is deterministically outside this variant. */
    residual_buffer.size = 272U;
    records[1].values[0] = 272;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    residual_buffer.size = 336U;
    records[1].values[0] = 336;

    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);

    common_buffer.is_partial = false;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    common_buffer.is_partial = true;

    residual_buffer.has_is_partial = true;
    residual_buffer.is_partial = true;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    residual_buffer.has_is_partial = false;
    residual_buffer.is_partial = false;
    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}

static int test_empty_binary_loose_slot_and_named_globals(void) {
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));

    SerializedProgramParameters common;
    serialized_program_parameters_init(&common);

    SerializedVariable named_variable = {
        .name = "NamedColor",
        /* Binary: type, rows, columns, matrix, array, byte offset. */
        .layout = {0U, 1U, 4U, 0U, 0U, 0U},
    };
    SerializedConstantBuffer residual_buffers[2] = {
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS,
            .size = 0U,
        },
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_NAMED,
            .size = 64U,
            .var_count = 1,
            .variables = &named_variable,
        },
    };
    SerializedProgramParameters residual;
    serialized_program_parameters_init(&residual);
    residual.is_binary = true;
    residual.cb_count = 2;
    residual.constant_buffers = residual_buffers;

    static const char* const text[] = {
        "cb: $Globals 64 1",
        "const: NamedColor 0 0 0 1 4 0",
        "stats: 1 0 0 0",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.authority ==
          UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL);
    CHECK(report.expected_record_count == 2U);
    CHECK(report.observed_record_count == 2U);
    CHECK(report.matched_record_count == 2U);
    CHECK(report.ignored_stats_record_count == 1U);
    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}

static int test_duplicate_named_globals_is_invalid(void) {
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));

    SerializedProgramParameters common;
    serialized_program_parameters_init(&common);

    SerializedConstantBuffer residual_buffers[2] = {
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_NAMED,
            .size = 16U,
        },
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_NAMED,
            .size = 32U,
        },
    };
    SerializedProgramParameters residual;
    serialized_program_parameters_init(&residual);
    residual.is_binary = true;
    residual.cb_count = 2;
    residual.constant_buffers = residual_buffers;

    static const char* const text[] = {
        "cb: $Globals 32 0",
        "stats: 3 0 0 0",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    CHECK(report.authority ==
          UNITY_REFLECTION_AUTHORITY_COMMON_PLUS_RESIDUAL);
    CHECK(report.observed_record_count == 1U);
    CHECK(report.ignored_stats_record_count == 1U);
    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}

static int test_nonempty_loose_globals_merge_and_synthesize(void) {
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));

    SerializedProgramParameters common;
    serialized_program_parameters_init(&common);

    SerializedVariable loose_variable = {
        .name = "LooseTint",
        .layout = {0U, 1U, 2U, 0U, 0U, 16U},
    };
    SerializedVariable named_variable = {
        .name = "NamedColor",
        .layout = {0U, 1U, 4U, 0U, 0U, 0U},
    };
    SerializedConstantBuffer residual_buffers[2] = {
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_LOOSE_PARAMETERS,
            .size = 64U,
            .var_count = 1,
            .variables = &loose_variable,
        },
        {
            .name = "$Globals",
            .role = SERIALIZED_CBUFFER_NAMED,
            .size = 64U,
            .var_count = 1,
            .variables = &named_variable,
        },
    };
    SerializedProgramParameters residual;
    serialized_program_parameters_init(&residual);
    residual.is_binary = true;
    residual.cb_count = 2;
    residual.constant_buffers = residual_buffers;

    static const char* const merged_text[] = {
        "cb: $Globals 64 2",
        "const: NamedColor 0 0 0 1 4 0",
        "const: LooseTint 16 0 0 1 2 0",
        "stats: 2 0 0 0",
    };
    UnityCompilerReflectionRecord merged_records[
        sizeof(merged_text) / sizeof(merged_text[0])];
    CHECK(parse_records(merged_text,
                        sizeof(merged_text) / sizeof(merged_text[0]),
                        merged_records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, merged_records,
              sizeof(merged_records) / sizeof(merged_records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_record_count == 3U);
    CHECK(report.observed_record_count == 3U);
    CHECK(report.matched_record_count == 3U);
    free_records(merged_records,
                 sizeof(merged_records) / sizeof(merged_records[0]));

    /* With no named shell, a nonempty loose area becomes the sole canonical
     * $Globals shell.  It must not disappear merely because its wire role is
     * distinct from a named constant buffer. */
    residual.cb_count = 1;
    residual_buffers[0].size = 32U;
    static const char* const synthetic_text[] = {
        "cb: $Globals 32 1",
        "const: LooseTint 16 0 0 1 2 0",
        "stats: 1 0 0 0",
    };
    UnityCompilerReflectionRecord synthetic_records[
        sizeof(synthetic_text) / sizeof(synthetic_text[0])];
    CHECK(parse_records(synthetic_text,
                        sizeof(synthetic_text) / sizeof(synthetic_text[0]),
                        synthetic_records));
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, &residual, synthetic_records,
              sizeof(synthetic_records) / sizeof(synthetic_records[0]),
              &report) == UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_record_count == 2U);
    CHECK(report.observed_record_count == 2U);
    CHECK(report.matched_record_count == 2U);
    CHECK(report.ignored_stats_record_count == 1U);
    free_records(synthetic_records,
                 sizeof(synthetic_records) / sizeof(synthetic_records[0]));
    return 0;
}

static int test_empty_buffer_rejects_unserialized_constant(void) {
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));
    SerializedConstantBuffer buffer = {
        .name = "EmptyCB",
        .size = 16U,
    };
    SerializedProgramParameters common;
    serialized_program_parameters_init(&common);
    common.cb_count = 1;
    common.constant_buffers = &buffer;
    static const char* const text[] = {
        "cb: EmptyCB 16 9",
        "const: HiddenBuiltin 0 0 0 1 4 0",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &common, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.observed_record_index == 1U);
    free_records(records, sizeof(records) / sizeof(records[0]));
    return 0;
}


static int test_source_map_reconstructs_unbound_input(void) {
    PlayerSubProgramBindChannel normal = {1U, 1U};
    PlayerSubProgramMetadata player;
    memset(&player, 0, sizeof(player));
    player.source_map = (1U << 1U) | (1U << 2U);
    player.binding_count = 1;
    player.bindings = &normal;
    SerializedProgramParameters parameters;
    serialized_program_parameters_init(&parameters);
    static const char* const text[] = {
        "input: 1 1",
        "input: 2 -1",
        "stats: 1 0 0 0",
    };
    UnityCompilerReflectionRecord records[
        sizeof(text) / sizeof(text[0])];
    CHECK(parse_records(text, sizeof(text) / sizeof(text[0]), records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &parameters, NULL, records,
              sizeof(records) / sizeof(records[0]), &report) ==
          UNITY_REFLECTION_CERTIFICATE_OK);
    CHECK(report.expected_record_count == 2U);
    CHECK(report.observed_record_count == 2U);
    CHECK(report.matched_record_count == 2U);
    free_records(records, sizeof(records) / sizeof(records[0]));

    player.source_map = 1U << 2U;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &player, &parameters, NULL, NULL, 0U, &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    return 0;
}

static int test_empty_binding_fingerprint(void) {
    PlayerSubProgramMetadata player = {0};
    SerializedProgramParameters common = {0};
    UnityReflectionCertificateReport report;
    /* Independently computed SHA-256 of the versioned domain and u64le(0). */
    static const uint8_t expected[32] = {0x58, 0x67, 0x0e, 0xe0, 0xca, 0x04, 0x8a, 0x55, 0x0a, 0x45, 0x49, 0x80, 0x2b, 0x65, 0x08, 0x5c, 0x4e, 0x6f, 0x40, 0xdf, 0xd0, 0x6e, 0xf2, 0x96, 0x0e, 0x1a, 0xe9, 0x0b, 0xa0, 0x27, 0x9c, 0x6c};
    CHECK(unity_reflection_certify_d3d11_bindings(
        &player, &common, NULL, NULL, 0, &report) == UNITY_REFLECTION_CERTIFICATE_OK);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(expected, report.expected_bindings_digest, 32) == 0);
    CHECK(memcmp(expected, report.observed_bindings_digest, 32) == 0);
    CHECK(report.expected_non_input_record_count == 0);
    return 0;
}

static int test_inventory_without_callbacks(void) {
    ReflectionFixture fixture;
    initialize_fixture(&fixture);
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, NULL, 0, &report) ==
          UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD);
    CHECK(report.expected_bindings_digest_valid && report.expected_non_input_record_count == 9);
    fixture.parameters.cb_count = fixture.parameters.res_count = 0;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, NULL, 0, &report) ==
          UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD);
    CHECK(report.expected_bindings_digest_valid && report.expected_record_count == 1 &&
          report.expected_non_input_record_count == 0);
    fixture.player.source_map = UINT32_MAX;
    CHECK(unity_reflection_certify_d3d11_bindings(
              &fixture.player, &fixture.parameters, NULL, NULL, 0, &report) ==
          UNITY_REFLECTION_CERTIFICATE_INVALID_METADATA);
    CHECK(!report.expected_bindings_digest_valid);
    return 0;
}

static int test_binding_fingerprint_buffer_scope(void) {
    PlayerSubProgramMetadata player = {0};
    SerializedVariable variables[2] = {{0}};
    variables[0].name = "First";
    variables[1].name = "Second";
    variables[0].layout[3] = variables[1].layout[3] = 4U;
    SerializedConstantBuffer buffers[2] = {
        {.name = "A", .size = 16U, .var_count = 1, .variables = &variables[0]},
        {.name = "B", .size = 16U, .var_count = 1, .variables = &variables[1]},
    };
    SerializedProgramParameters common = {0};
    common.cb_count = 2;
    common.constant_buffers = buffers;
    const char* const text[] = {
        "cb: A 16 1", "const: First 0 0 0 1 4 0",
        "cb: B 16 1", "stats: 1 0 0 0", "const: Second 0 0 0 1 4 0",
    };
    UnityCompilerReflectionRecord records[5];
    CHECK(parse_records(text, 5, records));
    UnityReflectionCertificateReport report;
    CHECK(unity_reflection_certify_d3d11_bindings(
        &player, &common, NULL, records, 5, &report) ==
        UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.expected_bindings_digest_valid && report.observed_bindings_digest_valid);
    CHECK(memcmp(report.expected_bindings_digest, report.observed_bindings_digest, 32) == 0);
    uint8_t baseline[32];
    memcpy(baseline, report.observed_bindings_digest, 32);
    UnityCompilerReflectionRecord reordered[] = {
        records[2], records[3], records[4], records[0], records[1]
    };
    CHECK(unity_reflection_certify_d3d11_bindings(
        &player, &common, NULL, reordered, 5, &report) ==
        UNITY_REFLECTION_CERTIFICATE_COMPATIBLE);
    CHECK(report.observed_bindings_digest_valid);
    CHECK(memcmp(baseline, report.observed_bindings_digest, 32) == 0);

    /* Same unscoped multiset, but each constant belongs to the wrong buffer. */
    UnityCompilerReflectionRecord swap = records[1];
    records[1] = records[4]; records[4] = swap;
    CHECK(unity_reflection_certify_d3d11_bindings(
        &player, &common, NULL, records, 5, &report) ==
        UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(report.observed_bindings_digest_valid);
    CHECK(memcmp(baseline, report.observed_bindings_digest, 32) != 0);
    CHECK(unity_reflection_certify_d3d11_bindings(
        &player, &common, NULL, records + 1, 4, &report) ==
        UNITY_REFLECTION_CERTIFICATE_EXTRA_RECORD);
    CHECK(!report.observed_bindings_digest_valid);
    free_records(records, 5);
    return 0;
}

int main(void) {
    CHECK(test_inventory_without_callbacks() == 0);
    CHECK(test_empty_binding_fingerprint() == 0);
    CHECK(test_binding_fingerprint_buffer_scope() == 0);
    CHECK(test_complete_binding_certificate() == 0);
    CHECK(test_signed_sampler_sentinel_and_struct() == 0);
    CHECK(test_common_plus_residual_composition() == 0);
    CHECK(test_empty_binary_loose_slot_and_named_globals() == 0);
    CHECK(test_duplicate_named_globals_is_invalid() == 0);
    CHECK(test_nonempty_loose_globals_merge_and_synthesize() == 0);
    CHECK(test_empty_buffer_rejects_unserialized_constant() == 0);
    CHECK(test_source_map_reconstructs_unbound_input() == 0);
    CHECK(strcmp(unity_reflection_certificate_status_name(
                     UNITY_REFLECTION_CERTIFICATE_MISSING_RECORD),
                 "missing-record") == 0);
    puts("reflection certificate unit tests passed");
    return 0;
}
