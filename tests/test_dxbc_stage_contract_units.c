#include "common/shader_stage.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_parser_internal.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/usil.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define PROGRAM_CAPACITY 512u

static void write_u32_le(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t instruction1(uint32_t opcode, uint32_t control) {
    return UINT32_C(0x01000000) | (control << 11u) | opcode;
}

static uint32_t instruction2(uint32_t opcode, uint32_t control) {
    return UINT32_C(0x02000000) | (control << 11u) | opcode;
}

static uint32_t instruction3(uint32_t opcode, uint32_t control) {
    return UINT32_C(0x03000000) | (control << 11u) | opcode;
}

static bool make_program(uint8_t bytes[PROGRAM_CAPACITY],
                         uint32_t version_token,
                         const uint32_t* instructions,
                         size_t instruction_word_count,
                         size_t* out_size) {
    const size_t program_word_count = instruction_word_count + 2u;
    const size_t payload_size = program_word_count * sizeof(uint32_t);
    const size_t total_size = 36u + 8u + payload_size;
    if (!instructions || !out_size || total_size > PROGRAM_CAPACITY)
        return false;
    memset(bytes, 0, PROGRAM_CAPACITY);
    memcpy(bytes, "DXBC", 4);
    write_u32_le(bytes + 20u, 1u);
    write_u32_le(bytes + 24u, (uint32_t)total_size);
    write_u32_le(bytes + 28u, 1u);
    write_u32_le(bytes + 32u, 36u);
    memcpy(bytes + 36u, "SHEX", 4);
    write_u32_le(bytes + 40u, (uint32_t)payload_size);
    write_u32_le(bytes + 44u, version_token);
    write_u32_le(bytes + 48u, (uint32_t)program_word_count);
    for (size_t i = 0; i < instruction_word_count; ++i) {
        write_u32_le(bytes + 52u + i * 4u, instructions[i]);
    }
    uint8_t hash[16];
    if (!dxbc_compute_hash(bytes, total_size, hash)) return false;
    memcpy(bytes + 4u, hash, sizeof(hash));
    *out_size = total_size;
    return true;
}

static bool decode_words(const uint32_t* words, size_t word_count,
                         uint32_t version, DXBCStageContract* contract,
                         DXBCStageContractDiagnostic* contract_diagnostic) {
    uint8_t bytes[PROGRAM_CAPACITY];
    size_t size;
    if (!make_program(bytes, version, words, word_count, &size)) return false;
    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    dxbc_document_init(&document);
    if (!dxbc_document_parse(&document, bytes, size, &document_diagnostic)) {
        dxbc_document_free(&document);
        return false;
    }
    bool decoded = dxbc_stage_contract_decode_document(
        &document, contract, contract_diagnostic);
    dxbc_document_free(&document);
    return decoded;
}

static bool expect_contract_error(const uint32_t* words, size_t word_count,
                                  uint32_t version,
                                  DXBCStageContractStatus expected) {
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    dxbc_stage_contract_init(&contract);
    bool decoded = decode_words(words, word_count, version, &contract,
                                &diagnostic);
    CHECK(!decoded);
    CHECK(diagnostic.status == expected);
    CHECK(strcmp(dxbc_stage_contract_status_name(diagnostic.status),
                 "unknown") != 0);
    dxbc_stage_contract_free(&contract);
    return true;
}

static bool verify_stage_domains(void) {
    static const UnityCompilerProgramStage expected_compiler[] = {
        UNITY_COMPILER_PROGRAM_VERTEX,
        UNITY_COMPILER_PROGRAM_FRAGMENT,
        UNITY_COMPILER_PROGRAM_GEOMETRY,
        UNITY_COMPILER_PROGRAM_HULL,
        UNITY_COMPILER_PROGRAM_DOMAIN,
        UNITY_COMPILER_PROGRAM_RAY_TRACING,
    };
    static const uint32_t expected_mask_bits[] = {
        UINT32_C(0x02), UINT32_C(0x04), UINT32_C(0x08),
        UINT32_C(0x10), UINT32_C(0x20), UINT32_C(0x40),
    };
    static const DXBCProgramType expected_dxbc[] = {
        DXBC_PROGRAM_TYPE_VERTEX,
        DXBC_PROGRAM_TYPE_PIXEL,
        DXBC_PROGRAM_TYPE_GEOMETRY,
        DXBC_PROGRAM_TYPE_HULL,
        DXBC_PROGRAM_TYPE_DOMAIN,
    };
    for (int stage = 0; stage < UNITY_SERIALIZED_STAGE_COUNT; ++stage) {
        UnityCompilerProgramStage compiler =
            UNITY_COMPILER_PROGRAM_INVALID;
        uint32_t mask = 0;
        CHECK(shader_stage_serialized_to_compiler(
            (UnitySerializedProgramStage)stage, &compiler));
        CHECK(compiler == expected_compiler[stage]);
        CHECK(shader_stage_serialized_program_mask_bit(
            (UnitySerializedProgramStage)stage, &mask));
        CHECK(mask == expected_mask_bits[stage]);
        UnitySerializedProgramStage round_trip =
            UNITY_SERIALIZED_STAGE_INVALID;
        CHECK(shader_stage_compiler_to_serialized(compiler, &round_trip));
        CHECK(round_trip == (UnitySerializedProgramStage)stage);
        if (stage < UNITY_SERIALIZED_STAGE_RAY_TRACING) {
            DXBCProgramType program_type = DXBC_PROGRAM_TYPE_INVALID;
            CHECK(shader_stage_serialized_to_dxbc(
                (UnitySerializedProgramStage)stage, &program_type));
            CHECK(program_type == expected_dxbc[stage]);
            round_trip = UNITY_SERIALIZED_STAGE_INVALID;
            CHECK(shader_stage_dxbc_to_serialized(program_type,
                                                  &round_trip));
            CHECK(round_trip == (UnitySerializedProgramStage)stage);
        }
    }
    UnitySerializedProgramStage serialized = UNITY_SERIALIZED_STAGE_INVALID;
    CHECK(!shader_stage_compiler_to_serialized(
        UNITY_COMPILER_PROGRAM_COMPUTE, &serialized));
    DXBCProgramType dxbc = DXBC_PROGRAM_TYPE_INVALID;
    CHECK(!shader_stage_serialized_to_dxbc(
        UNITY_SERIALIZED_STAGE_RAY_TRACING, &dxbc));

    ShaderStageTuple tuple = {
        .serialized_stage = UNITY_SERIALIZED_STAGE_GEOMETRY,
        .compiler_program = UNITY_COMPILER_PROGRAM_GEOMETRY,
        .serialized_program_mask = UINT32_C(0x0e),
        .gpu_program_type = UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM40,
        .dxbc_program_type = DXBC_PROGRAM_TYPE_GEOMETRY,
        .shader_model_major = 4,
        .shader_model_minor = 0,
    };
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_OK);
    tuple.compiler_program = UNITY_COMPILER_PROGRAM_HULL;
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_COMPILER_PROGRAM_MISMATCH);
    tuple.compiler_program = UNITY_COMPILER_PROGRAM_GEOMETRY;
    tuple.serialized_program_mask = UINT32_C(0x06);
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_PROGRAM_MASK_MISMATCH);
    tuple.serialized_program_mask = UINT32_C(0x0e);
    tuple.dxbc_program_type = DXBC_PROGRAM_TYPE_HULL;
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_DXBC_PROGRAM_MISMATCH);
    tuple.dxbc_program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
    tuple.gpu_program_type = UNITY_GPU_PROGRAM_D3D11_GEOMETRY_SM50;
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_GPU_PROGRAM_MISMATCH);
    tuple.shader_model_major = 6;
    CHECK(shader_stage_validate_d3d11_tuple(&tuple) ==
          SHADER_STAGE_TUPLE_SHADER_MODEL_MISMATCH);
    return true;
}

static bool verify_operand_types(void) {
    CHECK(OPERAND_TYPE_INPUT_PRIMITIVE_ID == 11);
    CHECK(OPERAND_TYPE_RASTERIZER == 14);
    CHECK(OPERAND_TYPE_STREAM == 16);
    CHECK(OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID == 22);
    CHECK(OPERAND_TYPE_JOIN_INSTANCE_ID == 24);
    CHECK(OPERAND_TYPE_INPUT_PATCH_CONSTANT == 27);
    CHECK(OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY == 31);
    CHECK(OPERAND_TYPE_INPUT_COVERAGE_MASK == 35);
    CHECK(OPERAND_TYPE_INPUT_GS_INSTANCE_ID == 37);
    CHECK(OPERAND_TYPE_OUTPUT_STENCIL_REF == 41);
    CHECK(OPERAND_TYPE_INNER_COVERAGE == 42);
    CHECK(OPERAND_TYPE_UNKNOWN == 0xff);

    static const struct {
        DXBCOperandType type;
        const char* text;
    } cases[] = {
        {OPERAND_TYPE_INPUT_PRIMITIVE_ID, "vPrim"},
        {OPERAND_TYPE_RASTERIZER, "rasterizer"},
        {OPERAND_TYPE_STREAM, "m"},
        {OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID, "vOutputControlPointID"},
        {OPERAND_TYPE_FORK_INSTANCE_ID, "vForkInstanceID"},
        {OPERAND_TYPE_JOIN_INSTANCE_ID, "vJoinInstanceID"},
        {OPERAND_TYPE_INPUT_CONTROL_POINT, "vicp"},
        {OPERAND_TYPE_OUTPUT_CONTROL_POINT, "vocp"},
        {OPERAND_TYPE_INPUT_PATCH_CONSTANT, "vpc"},
        {OPERAND_TYPE_DOMAIN_LOCATION, "vDomain"},
        {OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY, "g"},
        {OPERAND_TYPE_INPUT_COVERAGE_MASK, "vCoverage"},
        {OPERAND_TYPE_INPUT_GS_INSTANCE_ID, "vGSInstanceID"},
        {OPERAND_TYPE_OUTPUT_STENCIL_REF, "oStencilRef"},
        {OPERAND_TYPE_INNER_COVERAGE, "vInnerCoverage"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t bytes[4];
        write_u32_le(bytes, 1u | ((uint32_t)cases[i].type << 12u));
        ByteStream stream;
        stream_init(&stream, bytes, sizeof(bytes));
        DXBCOperand* operand = parse_operand_recursive(
            &stream, stream.size, DXBC_OPERAND_CONTEXT_EXECUTABLE, 54u,
            NULL);
        CHECK(operand != NULL);
        CHECK(operand->type == cases[i].type);
        CHECK(strcmp(operand->text, cases[i].text) == 0);
        free_operand(operand);
        mem_free(operand, sizeof(*operand));
    }
    return true;
}

static bool verify_geometry_contract(void) {
    const uint32_t valid[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        UINT32_C(0x0300008f), UINT32_C(0x00110000), 0,
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 0), 6,
        instruction2(206, 0), 2,
        instruction1(62, 0),
    };
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    dxbc_stage_contract_init(&contract);
    CHECK(decode_words(valid, sizeof(valid) / sizeof(valid[0]),
                       UINT32_C(0x00020050), &contract, &diagnostic));
    CHECK(contract.program_type == DXBC_PROGRAM_TYPE_GEOMETRY);
    CHECK(contract.input_primitive == DXBC_INPUT_PRIMITIVE_TRIANGLE);
    CHECK(contract.output_topology == DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP);
    CHECK(contract.max_output_vertex_count == 6);
    CHECK(contract.geometry_instance_count == 2);
    CHECK(contract.declared_stream_mask == 1);
    CHECK(contract.referenced_stream_mask == 0);
    CHECK(contract.geometry_effect_count == 0);

    DXBCContainer semantic;
    memset(&semantic, 0, sizeof(semantic));
    strcpy(semantic.shader_type_model, "gs_5_0");
    semantic.has_executable_program = true;
    semantic.program_type = DXBC_PROGRAM_TYPE_GEOMETRY;
    semantic.major_version = 5;
    semantic.minor_version = 0;
    CHECK(dxbc_stage_contract_validate_container(&contract, &semantic,
                                                 &diagnostic));
    /* Mutable presentation text is not stage authority. */
    strcpy(semantic.shader_type_model, "hs_5_0");
    CHECK(dxbc_stage_contract_validate_container(&contract, &semantic,
                                                 &diagnostic));
    semantic.program_type = DXBC_PROGRAM_TYPE_HULL;
    CHECK(!dxbc_stage_contract_validate_container(&contract, &semantic,
                                                  &diagnostic));
    CHECK(diagnostic.status == DXBC_STAGE_CONTRACT_CONTAINER_MISMATCH);
    dxbc_stage_contract_free(&contract);

    const uint32_t invalid_bits[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 1), 6, instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        invalid_bits, sizeof(invalid_bits) / sizeof(invalid_bits[0]),
        UINT32_C(0x00020050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS));

    const uint32_t missing_scalar_payload[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction1(94, 0), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        missing_scalar_payload,
        sizeof(missing_scalar_payload) / sizeof(missing_scalar_payload[0]),
        UINT32_C(0x00020050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH));

    const uint32_t duplicate[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 0), 6, instruction2(94, 0), 7,
        instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        duplicate, sizeof(duplicate) / sizeof(duplicate[0]),
        UINT32_C(0x00020050),
        DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION));

    const uint32_t bad_order[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction1(62, 0), instruction2(94, 0), 6,
    };
    CHECK(expect_contract_error(
        bad_order, sizeof(bad_order) / sizeof(bad_order[0]),
        UINT32_C(0x00020050), DXBC_STAGE_CONTRACT_DECLARATION_ORDER));

    const uint32_t ordered_effects[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 0), 3,
        instruction1(19, 0), instruction1(19, 0),
        instruction1(9, 0), instruction1(62, 0),
    };
    dxbc_stage_contract_init(&contract);
    CHECK(decode_words(ordered_effects,
                       sizeof(ordered_effects) / sizeof(ordered_effects[0]),
                       UINT32_C(0x00020050), &contract, &diagnostic));
    CHECK(contract.geometry_effect_count == 3);
    CHECK(contract.referenced_stream_mask == 1);
    CHECK(contract.geometry_effects[0].kind ==
          DXBC_GEOMETRY_EFFECT_APPEND);
    CHECK(contract.geometry_effects[0].instruction_index == 3);
    CHECK(!contract.geometry_effects[0].explicit_stream);
    CHECK(contract.geometry_effects[1].kind ==
          DXBC_GEOMETRY_EFFECT_APPEND);
    CHECK(contract.geometry_effects[2].kind ==
          DXBC_GEOMETRY_EFFECT_RESTART_STRIP);
    dxbc_stage_contract_free(&contract);

    const uint32_t explicit_stream[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_LINE),
        instruction3(143, 0), UINT32_C(0x00110000), 0,
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_LINE_STRIP),
        instruction2(94, 0), 2,
        instruction3(117, 0), UINT32_C(0x00110000), 0,
        instruction3(118, 0), UINT32_C(0x00110000), 0,
        instruction1(62, 0),
    };
    dxbc_stage_contract_init(&contract);
    CHECK(decode_words(explicit_stream,
                       sizeof(explicit_stream) / sizeof(explicit_stream[0]),
                       UINT32_C(0x00020050), &contract, &diagnostic));
    CHECK(contract.declared_stream_mask == 1);
    CHECK(contract.referenced_stream_mask == 1);
    CHECK(contract.geometry_effect_count == 2);
    CHECK(contract.geometry_effects[0].explicit_stream);
    CHECK(contract.geometry_effects[0].stream_id == 0);
    CHECK(contract.geometry_effects[1].explicit_stream);
    dxbc_stage_contract_free(&contract);

    const uint32_t undeclared_explicit_stream[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 0), 3,
        instruction3(117, 0), UINT32_C(0x00110000), 1,
        instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        undeclared_explicit_stream,
        sizeof(undeclared_explicit_stream) /
            sizeof(undeclared_explicit_stream[0]),
        UINT32_C(0x00020050), DXBC_STAGE_CONTRACT_MISSING_DECLARATION));

    const uint32_t implicit_with_declared_stream[] = {
        instruction1(93, DXBC_INPUT_PRIMITIVE_TRIANGLE),
        instruction3(143, 0), UINT32_C(0x00110000), 0,
        instruction1(92, DXBC_OUTPUT_TOPOLOGY_TRIANGLE_STRIP),
        instruction2(94, 0), 3,
        instruction1(19, 0), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        implicit_with_declared_stream,
        sizeof(implicit_with_declared_stream) /
            sizeof(implicit_with_declared_stream[0]),
        UINT32_C(0x00020050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    return true;
}

static bool verify_hull_contract(void) {
    const uint32_t valid[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, DXBC_TESSELLATOR_DOMAIN_TRIANGLE),
        instruction1(150, DXBC_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD),
        instruction1(151, DXBC_TESSELLATOR_OUTPUT_TRIANGLE_CW),
        instruction2(152, 0), UINT32_C(0x42000000),
        instruction1(115, 0), instruction2(153, 0), 3,
        instruction1(62, 0), instruction1(115, 0), instruction1(62, 0),
    };
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    dxbc_stage_contract_init(&contract);
    CHECK(decode_words(valid, sizeof(valid) / sizeof(valid[0]),
                       UINT32_C(0x00030050), &contract, &diagnostic));
    CHECK(contract.program_type == DXBC_PROGRAM_TYPE_HULL);
    CHECK(contract.input_control_point_count == 3);
    CHECK(contract.output_control_point_count == 3);
    CHECK(contract.max_tessellation_factor == 32.0f);
    CHECK(contract.hull_phase_count == 2);
    CHECK(contract.hull_phases[0].kind == DXBC_HULL_PHASE_FORK);
    CHECK(contract.hull_phases[0].instance_count_declared);
    CHECK(contract.hull_phases[0].instance_count == 3);
    CHECK(!contract.hull_phases[1].instance_count_declared);
    CHECK(contract.hull_phases[1].instance_count == 1);
    dxbc_stage_contract_free(&contract);

    const uint32_t nan_factor[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction2(152, 0), UINT32_C(0x7fc00000),
        instruction1(115, 0), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        nan_factor, sizeof(nan_factor) / sizeof(nan_factor[0]),
        UINT32_C(0x00030050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));

    const uint32_t truncated_instance_count[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction1(115, 0), instruction1(153, 0), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        truncated_instance_count,
        sizeof(truncated_instance_count) /
            sizeof(truncated_instance_count[0]),
        UINT32_C(0x00030050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH));

    const uint32_t zero_instance_count[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction1(115, 0), instruction2(153, 0), 0,
        instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        zero_instance_count,
        sizeof(zero_instance_count) / sizeof(zero_instance_count[0]),
        UINT32_C(0x00030050),
        DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));

    const uint32_t bad_phase_order[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction1(116, 0), instruction1(62, 0),
        instruction1(115, 0), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        bad_phase_order,
        sizeof(bad_phase_order) / sizeof(bad_phase_order[0]),
        UINT32_C(0x00030050), DXBC_STAGE_CONTRACT_PHASE_ORDER));

    const uint32_t late_index_range[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction1(115, 0), instruction1(62, 0),
        UINT32_C(0x0400005b), UINT32_C(0x00102012), 0, 3,
    };
    CHECK(expect_contract_error(
        late_index_range,
        sizeof(late_index_range) / sizeof(late_index_range[0]),
        UINT32_C(0x00030050),
        DXBC_STAGE_CONTRACT_DECLARATION_ORDER));

    /* The semantic decoder must preserve scalar declaration grammar and the
     * ordered phase-local index range, not reinterpret scalar DWORDs as
     * operand tokens or merge the two fork phases. */
    const uint32_t semantic_phase_program[] = {
        instruction1(113, 0), instruction1(147, 3), instruction1(148, 3),
        instruction1(149, 2), instruction1(150, 3), instruction1(151, 3),
        instruction1(115, 0), instruction2(153, 0), 3,
        UINT32_C(0x0400005b), UINT32_C(0x00102012), 0, 3,
        instruction1(62, 0), instruction1(115, 0), instruction1(62, 0),
    };
    uint8_t bytes[PROGRAM_CAPACITY];
    size_t size = 0;
    CHECK(make_program(bytes, UINT32_C(0x00030050),
                       semantic_phase_program,
                       sizeof(semantic_phase_program) /
                           sizeof(semantic_phase_program[0]),
                       &size));
    DXBCDocument document;
    DXBCDocumentDiagnostic document_diagnostic;
    DXBCContainer semantic;
    dxbc_document_init(&document);
    memset(&semantic, 0, sizeof(semantic));
    dxbc_stage_contract_init(&contract);
    CHECK(dxbc_document_parse(&document, bytes, size,
                              &document_diagnostic));
    CHECK(dxbc_document_decode_semantic(&document, &semantic));
    CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract,
                                     &diagnostic));
    const DXBCInstruction *instance_declaration = NULL;
    const DXBCInstruction *index_range_declaration = NULL;
    for (int instruction = 0; instruction < semantic.instruction_count;
         ++instruction) {
        if (semantic.instructions[instruction].opcode == 153)
            instance_declaration = &semantic.instructions[instruction];
        if (semantic.instructions[instruction].opcode == 91)
            index_range_declaration = &semantic.instructions[instruction];
    }
    CHECK(instance_declaration != NULL);
    CHECK(instance_declaration->operand_count == 1);
    CHECK(instance_declaration->operands[0].immediate_word_count == 1);
    CHECK(instance_declaration->operands[0].immediate_words[0] == 3);
    CHECK(index_range_declaration != NULL);
    CHECK(index_range_declaration->operand_count == 2);
    CHECK(index_range_declaration->operands[1].immediate_word_count == 1);
    CHECK(index_range_declaration->operands[1].immediate_words[0] == 3);

    USILProgram usil;
    CHECK(usil_translate_with_stage_contract(&usil, &semantic, &contract));
    CHECK(usil.tessellation.valid);
    CHECK(usil.tessellation.phase_count == 2);
    CHECK(usil.tessellation.phases[0].kind == DXBC_HULL_PHASE_FORK);
    CHECK(usil.tessellation.phases[0].instance_count_declared);
    CHECK(usil.tessellation.phases[0].instance_count == 3);
    CHECK(usil.tessellation.phases[0].first_instruction_index == 0);
    CHECK(usil.tessellation.phases[0].end_instruction_index == 1);
    CHECK(usil.tessellation.phases[1].first_instruction_index == 1);
    CHECK(usil.tessellation.phases[1].end_instruction_index == 2);
    CHECK(usil.index_range_count == 1);
    CHECK(usil.index_ranges[0].register_count == 3);
    CHECK(usil.index_ranges[0].hull_phase_index == 0);
    CHECK(usil.index_ranges[0].operand.type == OPERAND_TYPE_OUTPUT);
    usil_free(&usil);
    dxbc_free(&semantic);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return true;
}

static bool verify_domain_contract(void) {
    const uint32_t valid[] = {
        instruction1(147, 3),
        instruction1(149, DXBC_TESSELLATOR_DOMAIN_TRIANGLE),
        instruction1(62, 0),
    };
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    dxbc_stage_contract_init(&contract);
    CHECK(decode_words(valid, sizeof(valid) / sizeof(valid[0]),
                       UINT32_C(0x00040050), &contract, &diagnostic));
    CHECK(contract.program_type == DXBC_PROGRAM_TYPE_DOMAIN);
    CHECK(contract.input_control_point_count == 3);
    CHECK(contract.tessellator_domain == DXBC_TESSELLATOR_DOMAIN_TRIANGLE);
    dxbc_stage_contract_free(&contract);

    const uint32_t missing_domain[] = {
        instruction1(147, 3), instruction1(62, 0),
    };
    CHECK(expect_contract_error(
        missing_domain,
        sizeof(missing_domain) / sizeof(missing_domain[0]),
        UINT32_C(0x00040050), DXBC_STAGE_CONTRACT_MISSING_DECLARATION));
    return true;
}

int main(void) {
    const size_t allocations = g_allocations_count;
    const size_t bytes = g_allocated_bytes;
    if (!verify_stage_domains() || !verify_operand_types() ||
        !verify_geometry_contract() || !verify_hull_contract() ||
        !verify_domain_contract()) {
        return 1;
    }
    if (g_allocations_count != allocations || g_allocated_bytes != bytes) {
        fprintf(stderr, "allocation leak: %zu/%zu -> %zu/%zu\n",
                allocations, bytes, g_allocations_count, g_allocated_bytes);
        return 1;
    }
    puts("DXBC stage contract unit tests passed");
    return 0;
}
