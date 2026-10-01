// SPDX-License-Identifier: GPL-3.0-only

#include "common/string_builder.h"
#include "dxbc/dxbc_hash.h"
#include "dxbc/dxbc_stage_contract.h"
#include "translation/hlsl_emitter.h"
#include "translation/hlsl_compute_source_internal.h"
#include "translation/hlsl_source_quality.h"
#include "translation/hlsl_source_quality_internal.h"
#include "translation/usil_validation.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define INSTRUCTION(opcode, length) ((uint32_t)(opcode) | (uint32_t)(length) << 24u)
#define SYNC(flags) (INSTRUCTION(190u, 1u) | (uint32_t)(flags) << 11u)
#define GROUP_DECL INSTRUCTION(155u, 4u), 8u, 4u, 1u
#define TGSM_RAW(register_id, size) INSTRUCTION(159u, 4u), 0x0011f000u, register_id, size
#define TGSM_STRUCTURED(register_id, stride, count) \
    INSTRUCTION(160u, 5u), 0x0011f000u, register_id, stride, count
#define RETURN INSTRUCTION(62u, 1u)

static void write_u32(uint8_t* bytes, uint32_t value) {
    for (unsigned index = 0; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

/* Controlled token programs exercise the production lossless decoder,
 * semantic parser, contract, and projection without a compiler substitute. */
static bool parse_program(const uint32_t* words, size_t count, uint32_t version,
                           DXBCDocument* document, DXBCContainer* container) {
    uint8_t bytes[2048];
    const size_t size = 52u + count * 4u;
    if (size > sizeof(bytes)) return false;
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "DXBC", 4u);
    write_u32(bytes + 20u, 1u);
    write_u32(bytes + 24u, (uint32_t)size);
    write_u32(bytes + 28u, 1u);
    write_u32(bytes + 32u, 36u);
    memcpy(bytes + 36u, "SHEX", 4u);
    write_u32(bytes + 40u, (uint32_t)(size - 44u));
    write_u32(bytes + 44u, version);
    write_u32(bytes + 48u, (uint32_t)(count + 2u));
    for (size_t index = 0; index < count; ++index)
        write_u32(bytes + 52u + index * 4u, words[index]);
    if (!dxbc_compute_hash(bytes, size, bytes + 4u)) return false;
    DXBCDocumentDiagnostic diagnostic;
    return dxbc_document_parse(document, bytes, size, &diagnostic) &&
           (!container || dxbc_parse(container, bytes, size));
}

static bool expect_contract_failure(const uint32_t* words, size_t count,
                                      uint32_t version, DXBCStageContractStatus status) {
    DXBCDocument document;
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, count, version, &document, NULL));
    CHECK(!dxbc_stage_contract_decode_document(&document, &contract, &diagnostic));
    CHECK(diagnostic.status == status);
    CHECK(contract.program_type == DXBC_PROGRAM_TYPE_INVALID);
    CHECK(!contract.thread_group_shared_memory && !contract.memory_barriers);
    dxbc_stage_contract_free(&contract);
    dxbc_document_free(&document);
    return true;
}

static bool execution_metadata_and_barriers(void) {
    const uint32_t words[] = {
        GROUP_DECL,
        TGSM_RAW(0u, 256u),
        TGSM_STRUCTURED(7u, 16u, 32u),
        SYNC(2u), SYNC(3u), SYNC(4u), SYNC(5u), SYNC(8u), SYNC(9u),
        SYNC(6u), SYNC(7u), SYNC(10u), SYNC(11u), RETURN
    };
    DXBCDocument document;
    DXBCContainer container = {0};
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, COUNT(words), 0x00050050u, &document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &diagnostic));
    CHECK(contract.has_thread_group_size && contract.thread_group_size[0] == 8u &&
          contract.thread_group_size[1] == 4u && contract.thread_group_size[2] == 1u);
    CHECK(contract.thread_group_shared_memory_count == 2u &&
          contract.thread_group_shared_memory_bytes == 768u && contract.memory_barrier_count == 10u);
    CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
    CHECK(program.compute.valid && program.compute.shared_memory_count == 2u &&
          program.compute.shared_memory_bytes == 768u && program.compute.barrier_count == 10u &&
          program.compute.system_value_mask == 0u && program.instruction_count == 11);
    CHECK(program.compute.shared_memory != contract.thread_group_shared_memory);
    CHECK(!program.compute.shared_memory[0].structured &&
          program.compute.shared_memory[1].register_id == 7u &&
          program.compute.shared_memory[1].byte_stride == 16u &&
          program.compute.shared_memory[1].element_count == 32u);
    for (int index = 0; index < 10; ++index) {
        const USILInstruction* instruction = &program.instructions[index];
        USILEffectFlags effects;
        CHECK(instruction->opcode == USIL_OP_SYNC && instruction->operand_count == 0 &&
              instruction->source_instruction_index == (uint32_t)index + 3u &&
              instruction->sync_flags == contract.memory_barriers[index].flags &&
              !instruction->saturate && instruction->precise_mask == 0u);
        CHECK(usil_instruction_shape_valid(&program, instruction));
        CHECK(usil_instruction_effects(&program, instruction, &effects));
        CHECK((effects & USIL_EFFECT_MEMORY_BARRIER) != 0u &&
              ((effects & USIL_EFFECT_CONTROL) != 0u) == ((instruction->sync_flags & 1u) != 0u));
        USILInstruction changed = *instruction;
        changed.saturate = true;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
        changed = *instruction;
        changed.sync_flags = 12u;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
    }
    uint32_t old_dimension = container.instructions[0].operands[0].immediate_words[0];
    container.instructions[0].operands[0].immediate_words[0] = 4u;
    USILProgram rejected = {0};
    CHECK(!usil_translate_with_stage_contract(&rejected, &container, &contract));
    CHECK(!rejected.compute.shared_memory && !rejected.instructions && rejected.instruction_count == 0);
    container.instructions[0].operands[0].immediate_words[0] = old_dimension;
    container.instructions[3].token ^= 0x800u;
    CHECK(!usil_translate_with_stage_contract(&rejected, &container, &contract));
    container.instructions[3].token ^= 0x800u;
    contract.thread_group_shared_memory_bytes += 4u;
    CHECK(!usil_translate_with_stage_contract(&rejected, &container, &contract));
    contract.thread_group_shared_memory_bytes -= 4u;
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&container);
    dxbc_document_free(&document);
    return true;
}

static bool system_values_and_source_gate(void) {
    const uint32_t words[] = {
        GROUP_DECL,
        INSTRUCTION(95u, 2u), 0x00020012u, /* vThreadID.x */
        INSTRUCTION(95u, 2u), 0x00021032u, /* vThreadGroupID.xy */
        INSTRUCTION(95u, 2u), 0x00022052u, /* vThreadIDInGroup.xz */
        INSTRUCTION(95u, 2u), 0x00024000u, /* vThreadIDInGroupFlattened */
        RETURN
    };
    DXBCDocument document;
    DXBCContainer container = {0};
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, COUNT(words), 0x00050050u, &document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
    CHECK(program.compute.system_value_mask == 0x0fu && program.signature_declaration_count == 4);
    CHECK(program.signature_declarations[0].mask == 1u &&
          program.signature_declarations[1].mask == 3u &&
          program.signature_declarations[2].mask == 5u);
    CHECK(usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].mask = 8u;
    CHECK(!usil_signature_authority_is_valid(&program));
    program.signature_declarations[0].mask = 1u;
    for (int mode = HLSL_EMIT_MODE_RECOMPILE; mode <= HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE; ++mode) {
        StringBuilder output;
        HLSLEmitDiagnostic failure;
        HLSLSourceQualityResult quality;
        HLSLEmitOptions options = {0};
        options.mode = (HLSLEmitMode)mode;
        options.source_quality = &quality;
        sb_init(&output);
        CHECK(hlsl_emit_with_options_diagnostic(&program, &output, NULL, NULL, NULL,
                                               &options, &failure));
        CHECK(failure.status == HLSL_EMIT_STATUS_OK &&
              strstr(output.buf, "[numthreads(8, 4, 1)]") &&
              strstr(output.buf, "uint3 dispatchThreadId : SV_DispatchThreadID") &&
              strstr(output.buf, "uint3 groupId : SV_GroupID") &&
              strstr(output.buf, "uint3 groupThreadId : SV_GroupThreadID") &&
              strstr(output.buf, "uint groupIndex : SV_GroupIndex") &&
              strstr(output.buf, "return;") && !strstr(output.buf, "float4"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && quality.reasons == 0u &&
              quality.counts.inspected_units == 1u && quality.counts.incomplete_units == 0u &&
              quality.counts.unknown_provenance == 0u && quality.counts.residual_total == 0u &&
              quality.counts.emission_events == 8u);
        sb_free(&output);
    }
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&container);
    dxbc_document_free(&document);
    return true;
}

static bool declaration_rejections(void) {
    const uint32_t missing[] = {RETURN};
    const uint32_t duplicate[] = {GROUP_DECL, GROUP_DECL, RETURN};
    const uint32_t late[] = {GROUP_DECL, RETURN, TGSM_RAW(0u, 4u)};
    const uint32_t zero[] = {INSTRUCTION(155u, 4u), 0u, 1u, 1u, RETURN};
    const uint32_t oversized[] = {INSTRUCTION(155u, 4u), 1024u, 2u, 1u, RETURN};
    const uint32_t overflow[] = {GROUP_DECL, TGSM_STRUCTURED(0u, 0xfffffffcu, 0xffffffffu), RETURN};
    const uint32_t unaligned[] = {GROUP_DECL, TGSM_RAW(0u, 6u), RETURN};
    const uint32_t duplicate_memory[] = {GROUP_DECL, TGSM_RAW(0u, 4u), TGSM_STRUCTURED(0u, 4u, 1u), RETURN};
    const uint32_t oversized_total[] = {GROUP_DECL, TGSM_RAW(0u, 32768u), TGSM_RAW(1u, 4u), RETURN};
    const uint32_t invalid_binding[] = {GROUP_DECL, INSTRUCTION(159u, 4u), 0x0011e000u, 0u, 4u, RETURN};
    const uint32_t flags[] = {GROUP_DECL, SYNC(1u), RETURN};
    const uint32_t conflicting_flags[] = {GROUP_DECL, SYNC(12u), RETURN};
    const uint32_t unknown_flags[] = {GROUP_DECL, SYNC(18u), RETURN};
    const uint32_t invalid_length[] = {INSTRUCTION(155u, 3u), 1u, 1u, RETURN};
    CHECK(expect_contract_failure(missing, COUNT(missing), 0x00050050u, DXBC_STAGE_CONTRACT_MISSING_DECLARATION));
    CHECK(expect_contract_failure(duplicate, COUNT(duplicate), 0x00050050u, DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION));
    CHECK(expect_contract_failure(late, COUNT(late), 0x00050050u, DXBC_STAGE_CONTRACT_DECLARATION_ORDER));
    CHECK(expect_contract_failure(zero, COUNT(zero), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    CHECK(expect_contract_failure(oversized, COUNT(oversized), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    CHECK(expect_contract_failure(overflow, COUNT(overflow), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    CHECK(expect_contract_failure(unaligned, COUNT(unaligned), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    CHECK(expect_contract_failure(duplicate_memory, COUNT(duplicate_memory), 0x00050050u, DXBC_STAGE_CONTRACT_DUPLICATE_DECLARATION));
    CHECK(expect_contract_failure(oversized_total, COUNT(oversized_total), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    CHECK(expect_contract_failure(invalid_binding, COUNT(invalid_binding), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS));
    CHECK(expect_contract_failure(flags, COUNT(flags), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS));
    CHECK(expect_contract_failure(conflicting_flags, COUNT(conflicting_flags), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS));
    CHECK(expect_contract_failure(unknown_flags, COUNT(unknown_flags), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_BITS));
    CHECK(expect_contract_failure(invalid_length, COUNT(invalid_length), 0x00050050u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_LENGTH));
    CHECK(expect_contract_failure(duplicate, COUNT(duplicate), 0x00010050u, DXBC_STAGE_CONTRACT_DECLARATION_STAGE_MISMATCH));
    const uint32_t unsupported_model[] = {GROUP_DECL, TGSM_STRUCTURED(0u, 4u, 1u), RETURN};
    CHECK(expect_contract_failure(unsupported_model, COUNT(unsupported_model), 0x00050040u, DXBC_STAGE_CONTRACT_UNSUPPORTED_PROGRAM));
    const uint32_t invalid_cs4_z[] = {INSTRUCTION(155u, 4u), 1u, 1u, 2u, RETURN};
    CHECK(expect_contract_failure(invalid_cs4_z, COUNT(invalid_cs4_z), 0x00050040u, DXBC_STAGE_CONTRACT_INVALID_DECLARATION_VALUE));
    return true;
}

static bool shader_model_group_bounds(void) {
    const uint32_t versions[] = {0x00050040u, 0x00050041u, 0x00050050u};
    const uint32_t dimensions[][3] = {{768u, 1u, 1u}, {24u, 32u, 1u}, {4u, 4u, 64u}};
    for (size_t index = 0; index < COUNT(versions); ++index) {
        const uint32_t words[] = {INSTRUCTION(155u, 4u), dimensions[index][0],
                                 dimensions[index][1], dimensions[index][2], RETURN};
        DXBCDocument document;
        DXBCContainer container = {0};
        DXBCStageContract contract;
        DXBCStageContractDiagnostic diagnostic;
        USILProgram program = {0};
        dxbc_document_init(&document);
        dxbc_stage_contract_init(&contract);
        CHECK(parse_program(words, COUNT(words), versions[index], &document, &container));
        CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &diagnostic));
        CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
        CHECK(memcmp(program.compute.thread_group_size, dimensions[index],
                     sizeof(program.compute.thread_group_size)) == 0);
        usil_free(&program);
        dxbc_stage_contract_free(&contract);
        dxbc_free(&container);
        dxbc_document_free(&document);
    }
    return true;
}

#define TEMP_DEST(reg, lanes) (0x00100002u | (uint32_t)(lanes) << 4u), reg
#define TEMP_SOURCE(reg) 0x00100e46u, reg
#define BINDING(type, reg) (0x00100000u | (uint32_t)(type) << 12u), reg
#define MEMORY_DEST(type, reg, lanes) (0x00100002u | (uint32_t)(type) << 12u | (uint32_t)(lanes) << 4u), reg
#define MEMORY_SOURCE(type, reg) (0x00100e46u | (uint32_t)(type) << 12u), reg
#define SCALAR(value) 0x00004001u, value
#define VECTOR(x, y, z, w) 0x00004002u, x, y, z, w

static bool reject_expression_observer(void *context,
                                        const HLSLSourceQualityObservation *observation) {
    size_t *observations = context;
    ++*observations;
    return observation->kind != HLSL_SOURCE_OBSERVATION_EXPRESSION;
}

static bool source_rejected(const USILProgram *program, const HLSLEmitOptions *options,
                             const HLSLEmitNames *names) {
    StringBuilder source;
    HLSLEmitDiagnostic diagnostic;
    sb_init(&source);
    sb_append(&source, "prefix");
    const bool emitted = hlsl_emit_with_options_diagnostic(program, &source, NULL, NULL,
                                                           names, options, &diagnostic);
    CHECK(!emitted && source.failed && source.len == 6u && strcmp(source.buf, "prefix") == 0);
    CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK);
    sb_free(&source);
    return true;
}

typedef struct {
    const USILProgram *program;
    int next_instruction;
} BarrierSourceObservation;

static bool observe_barrier_ownership(void *context,
                                      const HLSLSourceQualityObservation *observation) {
    BarrierSourceObservation *ledger = context;
    const HLSLSourceQualityFacts *facts = &observation->facts;
    if (observation->kind != HLSL_SOURCE_OBSERVATION_EMISSION || facts->instruction_index < 0)
        return true;
    if (!facts->known || !facts->logical_operation || facts->artifacts || facts->lanes ||
        facts->instruction_index != ledger->next_instruction ||
        facts->source_instruction_index != ledger->program->instructions[ledger->next_instruction].source_instruction_index)
        return false;
    ++ledger->next_instruction;
    return true;
}

static bool barrier_intrinsic_source(void) {
    const uint32_t words[] = {
        GROUP_DECL, SYNC(2u), SYNC(8u), SYNC(10u), SYNC(3u), SYNC(9u), SYNC(11u), RETURN
    };
    const char *const calls[] = {
        "GroupMemoryBarrier();", "DeviceMemoryBarrier();", "AllMemoryBarrier();",
        "GroupMemoryBarrierWithGroupSync();", "DeviceMemoryBarrierWithGroupSync();",
        "AllMemoryBarrierWithGroupSync();"
    };
    DXBCDocument document;
    DXBCContainer container = {0};
    DXBCStageContract contract;
    DXBCStageContractDiagnostic stage_diagnostic;
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, COUNT(words), 0x00050050u, &document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &stage_diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
    CHECK(program.compute.barrier_count == 6u && program.instruction_count == 7);
    HLSLSourceQualityResult quality;
    HLSLEmitOptions options = {.source_quality = &quality,
                              .source_quality_observer = observe_barrier_ownership};
    for (int mode = HLSL_EMIT_MODE_RECOMPILE; mode <= HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE; ++mode) {
        BarrierSourceObservation ledger = {.program = &program};
        options.mode = (HLSLEmitMode)mode;
        options.source_quality_observer_context = &ledger;
        StringBuilder source;
        HLSLEmitDiagnostic diagnostic;
        sb_init(&source);
        CHECK(hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL,
                                               &options, &diagnostic));
        const char *position = source.buf;
        for (size_t index = 0; index < COUNT(calls); ++index) {
            position = strstr(position, calls[index]);
            CHECK(position != NULL);
            position += strlen(calls[index]);
        }
        CHECK(strstr(position, "return;") != NULL && ledger.next_instruction == 7);
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN && quality.reasons == 0u &&
              quality.counts.incomplete_units == 0u && quality.counts.unknown_provenance == 0u &&
              quality.counts.residual_total == 0u && quality.counts.logical_operations == 7u &&
              quality.counts.emission_events == 10u);
        sb_free(&source);
    }
    options.source_quality_observer = NULL;
    options.source_quality_observer_context = NULL;
    USILProgram changed = program;
    changed.compute.barrier_count = 5u;
    CHECK(source_rejected(&changed, &options, NULL));
    changed = program;
    changed.compute.barrier_count = 8u;
    CHECK(source_rejected(&changed, &options, NULL));
    changed = program;
    changed.shader_model_major = 4;
    strcpy(changed.shader_type_model, "cs_4_0");
    CHECK(source_rejected(&changed, &options, NULL));
    USILInstruction saved = program.instructions[0];
    const uint8_t unsupported_flags[] = {0u, 1u, 4u, 5u, 6u, 7u, 12u, 16u};
    for (size_t index = 0; index < COUNT(unsupported_flags); ++index) {
        program.instructions[0].sync_flags = unsupported_flags[index];
        CHECK(source_rejected(&program, &options, NULL));
    }
    program.instructions[0] = saved;
    program.instructions[0].resource_return_types[0] = 4u;
    CHECK(source_rejected(&program, &options, NULL));
    program.instructions[0] = saved;
    const char *reserved[] = {"AllMemoryBarrier"};
    options.reserved_preprocessor_identifiers = reserved;
    options.reserved_preprocessor_identifier_count = COUNT(reserved);
    CHECK(source_rejected(&program, &options, NULL));
    options.reserved_preprocessor_identifiers = NULL;
    options.reserved_preprocessor_identifier_count = 0;
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&container);
    dxbc_document_free(&document);

    const uint32_t controlled_flow[][8] = {
        {GROUP_DECL, INSTRUCTION(48u, 1u), SYNC(3u), INSTRUCTION(22u, 1u), RETURN},
        {GROUP_DECL, RETURN, SYNC(3u), RETURN, RETURN}
    };
    for (size_t index = 0; index < COUNT(controlled_flow); ++index) {
        dxbc_document_init(&document);
        dxbc_stage_contract_init(&contract);
        CHECK(parse_program(controlled_flow[index], COUNT(controlled_flow[index]), 0x00050050u,
                            &document, &container));
        CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &stage_diagnostic));
        CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
        CHECK(source_rejected(&program, &options, NULL));
        usil_free(&program);
        dxbc_stage_contract_free(&contract);
        dxbc_free(&container);
        dxbc_document_free(&document);
    }
    return true;
}

static bool unsigned_compute_source(void) {
    const uint32_t words[] = {
        GROUP_DECL,
        TGSM_RAW(0u, 256u), TGSM_STRUCTURED(7u, 16u, 32u),
        INSTRUCTION(95u, 2u), 0x00020072u,
        INSTRUCTION(104u, 2u), 2u,
        INSTRUCTION(54u, 4u), TEMP_DEST(0u, 7u), 0x00020e46u,
        INSTRUCTION(30u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(0u), SCALAR(1u),
        INSTRUCTION(1u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u), SCALAR(255u),
        INSTRUCTION(60u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u), SCALAR(16u),
        INSTRUCTION(87u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u), SCALAR(0xffffffffu),
        INSTRUCTION(59u, 5u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u),
        INSTRUCTION(41u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u), SCALAR(1u),
        INSTRUCTION(85u, 7u), TEMP_DEST(1u, 7u), TEMP_SOURCE(1u), SCALAR(2u),
        INSTRUCTION(54u, 4u), TEMP_DEST(0u, 1u), 0x0002002au,
        RETURN
    };
    DXBCDocument document;
    DXBCContainer container = {0};
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, COUNT(words), 0x00050050u, &document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
    HLSLEmitNames names = {.entry_point = "ProcessPositions"};
    HLSLSourceQualityResult quality;
    HLSLEmitOptions options = {.mode = HLSL_EMIT_MODE_HIGH_LEVEL_CANDIDATE, .source_quality = &quality};
    StringBuilder source;
    HLSLEmitDiagnostic failure;
    sb_init(&source);
    CHECK(hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, &names, &options, &failure));
    CHECK(failure.status == HLSL_EMIT_STATUS_OK && strstr(source.buf, "void ProcessPositions(") &&
          strstr(source.buf, "groupshared uint sharedMemory_0[64]") &&
          strstr(source.buf, "uint words[4]") &&
          strstr(source.buf, "groupshared SharedMemoryElement_7 sharedMemory_7[32]") &&
          strstr(source.buf, "uint3 value_0 = (dispatchThreadId)") &&
          strstr(source.buf, "dispatchThreadId.z") && !strstr(source.buf, "r0") &&
          !strstr(source.buf, "float4") && !strstr(source.buf, "asuint"));
    CHECK(quality.classification != HLSL_SOURCE_QUALITY_CLEAN &&
          quality.counts.unknown_provenance == 0u && quality.counts.raw_buffer_reconstruction == 2u &&
          quality.counts.instruction_assignments == 9u && quality.counts.semantic_projections == 1u &&
          quality.counts.register_storage == 0u && quality.counts.lane_transport == 0u);
    sb_free(&source);

    USILProgram changed = program;
    changed.compute.shared_memory_count = 0;
    changed.compute.shared_memory_bytes = 0;
    sb_init(&source);
    CHECK(hlsl_emit_with_options_diagnostic(&changed, &source, NULL, NULL, &names, &options, &failure));
    CHECK(quality.classification == HLSL_SOURCE_QUALITY_MIXED &&
          quality.reasons == HLSL_SOURCE_QUALITY_REASON_RESIDUAL &&
          quality.counts.incomplete_units == 0u && quality.counts.unknown_provenance == 0u &&
          quality.counts.raw_buffer_reconstruction == 0u &&
          quality.counts.instruction_assignments == 9u && quality.counts.residual_total == 9u);
    sb_free(&source);
    changed = program;
    changed.compute.thread_group_size[0] = 0u;
    CHECK(source_rejected(&changed, &options, &names));
    changed = program;
    changed.compute.shared_memory_bytes += 4u;
    CHECK(source_rejected(&changed, &options, &names));
    changed = program;
    changed.compute.system_value_mask ^= USIL_COMPUTE_GROUP_ID;
    CHECK(source_rejected(&changed, &options, &names));
    changed = program;
    changed.compute.barrier_count = 1u;
    CHECK(source_rejected(&changed, &options, &names));
    USILInstruction saved = program.instructions[1];
    program.instructions[1].operands[1].register_index = 99;
    program.instructions[1].operands[1].index_values[0] = 99u;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].operands[1].register_index = 1;
    program.instructions[1].operands[1].index_values[0] = 1u;
    CHECK(source_rejected(&program, &options, &names)); /* Undefined SSA source. */
    program.instructions[1] = saved;
    program.instructions[1].operands[1].swizzle[0] = 2u;
    CHECK(source_rejected(&program, &options, &names)); /* Register transport is not a logical projection. */
    program.instructions[1] = saved;
    program.instructions[1].opcode = USIL_OP_ADD;
    CHECK(source_rejected(&program, &options, &names)); /* Float domain has no source authority. */
    program.instructions[1] = saved;
    program.instructions[1].precise_mask = 1u;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].resource_stride = 16u;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].texel_offsets[0] = 1;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].resource_return_types[0] = 4u;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].geometry_stream_explicit = true;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    program.instructions[1].operands[1].has_abs = true;
    CHECK(source_rejected(&program, &options, &names));
    program.instructions[1] = saved;
    const char *reserved[] = {"dispatchThreadId"};
    options.reserved_preprocessor_identifiers = reserved;
    options.reserved_preprocessor_identifier_count = COUNT(reserved);
    CHECK(source_rejected(&program, &options, &names));
    options.reserved_preprocessor_identifiers = NULL;
    options.reserved_preprocessor_identifier_count = 0;
    HLSLEmitNames invalid = {.entry_point = "bad-name"};
    CHECK(source_rejected(&program, &options, &invalid));
    size_t observations = 0;
    options.source_quality_observer = reject_expression_observer;
    options.source_quality_observer_context = &observations;
    CHECK(source_rejected(&program, &options, &names));
    CHECK(observations > 1u && quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    options.source_quality_observer = NULL;
    options.source_quality_observer_context = NULL;
    sb_init(&source);
    CHECK(hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, &names, &options, &failure));
    sb_free(&source);
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&container);
    dxbc_document_free(&document);
    return true;
}

static bool memory_access_projection(void) {
    const uint32_t words[] = {
        GROUP_DECL,
        INSTRUCTION(162u, 4u), BINDING(7u, 0u), 16u,
        INSTRUCTION(161u, 3u), BINDING(7u, 1u),
        INSTRUCTION(158u, 4u), BINDING(30u, 0u), 16u,
        INSTRUCTION(157u, 3u), BINDING(30u, 1u),
        INSTRUCTION(156u, 4u) | (3u << 11u), BINDING(30u, 2u), 0x4444u,
        INSTRUCTION(158u, 4u) | 0x00810000u, BINDING(30u, 3u), 4u,
        TGSM_RAW(0u, 256u), TGSM_STRUCTURED(1u, 16u, 8u),
        INSTRUCTION(104u, 2u), 4u,
        INSTRUCTION(54u, 8u), TEMP_DEST(0u, 15u), VECTOR(0u, 1u, 2u, 3u),
        INSTRUCTION(165u, 7u), TEMP_DEST(0u, 15u), SCALAR(0u), MEMORY_SOURCE(7u, 1u),
        INSTRUCTION(166u, 7u), MEMORY_DEST(30u, 1u, 15u), SCALAR(0u), TEMP_SOURCE(0u),
        INSTRUCTION(167u, 9u), TEMP_DEST(0u, 15u), SCALAR(0u), SCALAR(0u), MEMORY_SOURCE(7u, 0u),
        INSTRUCTION(168u, 9u), MEMORY_DEST(30u, 0u, 15u), SCALAR(0u), SCALAR(0u), TEMP_SOURCE(0u),
        INSTRUCTION(165u, 7u), TEMP_DEST(0u, 15u), SCALAR(0u), MEMORY_SOURCE(31u, 0u),
        INSTRUCTION(166u, 7u), MEMORY_DEST(31u, 0u, 15u), SCALAR(0u), TEMP_SOURCE(0u),
        INSTRUCTION(167u, 9u), TEMP_DEST(0u, 15u), SCALAR(0u), SCALAR(0u), MEMORY_SOURCE(31u, 1u),
        INSTRUCTION(168u, 9u), MEMORY_DEST(31u, 1u, 15u), SCALAR(0u), SCALAR(0u), TEMP_SOURCE(0u),
        INSTRUCTION(163u, 10u), TEMP_DEST(0u, 15u), VECTOR(1u, 2u, 0u, 0u), MEMORY_SOURCE(30u, 2u),
        INSTRUCTION(164u, 10u), MEMORY_DEST(30u, 2u, 15u), VECTOR(1u, 2u, 0u, 0u), TEMP_SOURCE(0u),
        INSTRUCTION(173u, 7u), BINDING(30u, 1u), SCALAR(0u), SCALAR(1u),
        INSTRUCTION(171u, 10u), BINDING(30u, 0u), VECTOR(0u, 4u, 0u, 0u), SCALAR(1u),
        INSTRUCTION(180u, 9u), TEMP_DEST(1u, 1u), BINDING(31u, 0u), SCALAR(0u), SCALAR(1u),
        INSTRUCTION(185u, 14u), TEMP_DEST(1u, 1u), BINDING(30u, 0u),
            VECTOR(0u, 4u, 0u, 0u), SCALAR(1u), SCALAR(2u),
        INSTRUCTION(178u, 5u), TEMP_DEST(1u, 1u), BINDING(30u, 0u),
        INSTRUCTION(179u, 5u), TEMP_DEST(1u, 1u), BINDING(30u, 3u),
        RETURN
    };
    DXBCDocument document;
    DXBCContainer container = {0};
    DXBCStageContract contract;
    DXBCStageContractDiagnostic diagnostic;
    USILProgram program = {0};
    dxbc_document_init(&document);
    dxbc_stage_contract_init(&contract);
    CHECK(parse_program(words, COUNT(words), 0x00050050u, &document, &container));
    CHECK(dxbc_stage_contract_decode(&document, &container, &contract, &diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &container, &contract));
    size_t memory_count = 0u, shared_count = 0u, counter_count = 0u, atomic_count = 0u;
    for (int index = 0; index < program.instruction_count; ++index) {
        USILInstruction *instruction = &program.instructions[index];
        if (!usil_opcode_has_memory_access(instruction->opcode)) continue;
        USILMemoryAccess access;
        USILEffectFlags effects;
        CHECK(usil_instruction_memory_access(&program, instruction, &access));
        const int binding_operand = access.binding_operand;
        CHECK(usil_instruction_shape_valid(&program, instruction));
        CHECK(usil_instruction_effects(&program, instruction, &effects));
        ++memory_count;
        if (access.atomic) { ++atomic_count; CHECK((effects & USIL_EFFECT_ATOMIC) != 0u); }
        if (access.space == USIL_MEMORY_THREAD_GROUP) {
            ++shared_count;
            CHECK(access.shared_memory_byte_count != 0u);
            CHECK(!access.reads || (effects & USIL_EFFECT_THREAD_GROUP_READ) != 0u);
            CHECK(!access.writes || (effects & USIL_EFFECT_THREAD_GROUP_WRITE) != 0u);
            CHECK((effects & USIL_EFFECT_EXTERNAL_WRITE) == 0u);
        }
        if (access.kind == USIL_MEMORY_COUNTER) {
            ++counter_count;
            CHECK((effects & USIL_EFFECT_COUNTER) != 0u);
            CHECK(access.counter_mode == (access.register_id == 3u
                ? USIL_COUNTER_ORDER_PRESERVING : USIL_COUNTER_APPEND_INVOCATION));
            CHECK(access.globally_coherent == (access.register_id == 3u));
        }
        if (access.kind == USIL_MEMORY_TYPED) CHECK(access.address_lanes == 3u);
        for (int operand = 0; operand < instruction->operand_count; ++operand) {
            USILOperandUseInfo use;
            CHECK(usil_instruction_operand_use(&program, instruction, operand, &use));
            if (operand == access.binding_operand) CHECK(use.use == USIL_OPERAND_USE_RESOURCE_BINDING);
            else if (operand == access.destination_operand) CHECK(use.use == USIL_OPERAND_USE_DESTINATION);
            else CHECK(use.use == USIL_OPERAND_USE_SOURCE && use.source_lane_mask != 0u);
        }
        USILInstruction changed = *instruction;
        changed.saturate = true;
        CHECK(!usil_instruction_memory_access(&program, &changed, &access));
        changed = *instruction;
        changed.operand_count -= 1;
        CHECK(!usil_instruction_memory_access(&program, &changed, &access));
        changed = *instruction;
        changed.operands[binding_operand].register_index = 127;
        changed.operands[binding_operand].index_values[0] = 127u;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
        changed = *instruction;
        changed.precise_mask = 1u;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
        changed = *instruction;
        changed.operands[binding_operand].index_representations[0] = 1u;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
        changed = *instruction;
        changed.operands[binding_operand].has_abs = true;
        CHECK(!usil_instruction_shape_valid(&program, &changed));
        if (instruction->opcode == USIL_OP_IMM_ATOMIC_CMP_EXCH ||
            instruction->opcode == USIL_OP_IMM_ATOMIC_ALLOC) {
            changed = *instruction;
            changed.operands[0].destination_mask = 0x30u;
            CHECK(!usil_instruction_shape_valid(&program, &changed));
        }
    }
    CHECK(memory_count == 16u && shared_count == 5u && counter_count == 2u && atomic_count == 6u);
    for (int index = 0; index < program.instruction_count; ++index) {
        USILInstruction *instruction = &program.instructions[index];
        if (instruction->opcode == USIL_OP_IMM_ATOMIC_ALLOC) {
            USILInstruction changed = *instruction;
            changed.operands[1].register_index = 1;
            changed.operands[1].index_values[0] = 1u;
            CHECK(!usil_instruction_shape_valid(&program, &changed));
        }
        if (instruction->opcode == USIL_OP_LD_UAV_TYPED) {
            USILInstruction changed = *instruction;
            changed.resource_dimension[0] = 'x';
            CHECK(!usil_instruction_shape_valid(&program, &changed));
            changed = *instruction;
            changed.has_resource_return_types = true;
            memset(changed.resource_return_types, 5, sizeof(changed.resource_return_types));
            CHECK(!usil_instruction_shape_valid(&program, &changed));
        }
    }
    StringBuilder source;
    HLSLEmitDiagnostic failure;
    sb_init(&source);
    CHECK(!hlsl_emit_with_options_diagnostic(&program, &source, NULL, NULL, NULL, NULL, &failure));
    CHECK(failure.status == HLSL_EMIT_STATUS_UNSUPPORTED && failure.reason == HLSL_EMIT_REASON_UNSUPPORTED_STAGE);
    sb_free(&source);
    usil_free(&program);
    dxbc_stage_contract_free(&contract);
    dxbc_free(&container);
    dxbc_document_free(&document);
    return true;
}

/* The raw grammar follows the measured no-result typed atomic: no system
 * input or temporary declaration, one actual effect followed by RET. */
static const uint32_t atomic_words[] = {
    INSTRUCTION(106u, 1u) | (1u << 11u),
    INSTRUCTION(156u, 4u) | (3u << 11u), BINDING(30u, 0u), 0x4444u,
    INSTRUCTION(155u, 4u), 1u, 1u, 1u,
    INSTRUCTION(173u, 10u), BINDING(30u, 0u), VECTOR(0u, 0u, 0u, 0u), SCALAR(1u),
    RETURN
};

typedef struct {
    ASTExpr *roots[3];
    size_t root_count;
    size_t calls;
    size_t effect_count;
    size_t reject_root;
    size_t trigger;
    unsigned action;
    bool triggered;
    USILProgram *program;
    HLSLComputeTypedResource *resource;
    char *resource_name;
    char *entry_name;
    StringBuilder *published_source;
} AtomicObservations;

static void atomic_roots_dispose(AtomicObservations *observations) {
    for (size_t index = 0; index < observations->root_count; ++index)
        ast_free_expr(observations->roots[index]);
    observations->root_count = 0;
}

static bool atomic_retain_expression(void *context, ASTExpr *expression) {
    AtomicObservations *observations = context;
    if (observations->root_count == observations->reject_root ||
        observations->root_count == COUNT(observations->roots)) return false;
    /* A true return transfers this root. Inspect it only after emission has
     * finished; rejected roots remain with the emitter. */
    observations->roots[observations->root_count++] = expression;
    return true;
}

static bool atomic_observe(void *context, const HLSLSourceQualityObservation *observation) {
    AtomicObservations *observations = context;
    if (observation->stage != DXBC_PROGRAM_TYPE_COMPUTE || observation->reasons) return false;
    ++observations->calls;
    if (observation->kind == HLSL_SOURCE_OBSERVATION_EMISSION &&
        observation->facts.instruction_index >= 0) {
        const int index = observation->facts.instruction_index;
        if (index > 1 || !observation->facts.known || !observation->facts.logical_operation ||
            observation->facts.source_instruction_index != observations->program->instructions[index].source_instruction_index ||
            (size_t)index != observations->effect_count) return false;
        ++observations->effect_count;
    }
    if (!observations->trigger || observations->calls != observations->trigger) return true;
    observations->triggered = true;
    USILInstruction *atomic = &observations->program->instructions[0];
    switch (observations->action) {
        case 0: return false;
        case 1:
            atomic->operands[2].imm_values[0] = atomic->operands[2].immediate_words[0] = 7u;
            break;
        case 2:
            atomic->operands[1].imm_values[0] = atomic->operands[1].immediate_words[0] = 5u;
            break;
        case 3: observations->resource_name[0] = 'B'; break;
        case 4: observations->entry_name[0] = 'B'; break;
        case 5: observations->program->compute.thread_group_size[0] = 2u; break;
        case 6:
            observations->program->uavs[0].reg_idx = 3;
            atomic->operands[0].register_index = 3;
            atomic->operands[0].index_values[0] = 3u;
            observations->resource->binding_register = 3u;
            break;
        case 7: sb_append(observations->published_source, "/* inserted gap */"); break;
        default: return false;
    }
    return true;
}

static bool atomic_emit(USILProgram *program, HLSLComputeTypedResource *resource,
    char *entry_name, AtomicObservations *observations, StringBuilder *source,
    HLSLSourceQualityResult *quality, bool observe, HLSLEmitDiagnostic *diagnostic) {
    HLSLEmitOptions options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT;
    options.source_quality = quality;
    options.source_quality_observer = observe ? atomic_observe : NULL;
    options.source_quality_observer_context = observations;
    const HLSLEmitNames names = {.entry_point = entry_name};
    const HLSLComputeTypedSource typed = {.resources = resource, .resource_count = 1u,
        .retain_expression = atomic_retain_expression, .expression_context = observations};
    return hlsl_emit_compute_typed_stage(program, source, &names, &options, &typed, diagnostic);
}

static bool atomic_source_rejected(USILProgram *program, HLSLComputeTypedResource *resource,
                                   char *entry_name) {
    AtomicObservations observations = {.reject_root = SIZE_MAX, .program = program};
    StringBuilder source; sb_init(&source); sb_append(&source, "prefix");
    HLSLSourceQualityResult quality;
    HLSLEmitDiagnostic diagnostic;
    const bool emitted = atomic_emit(program, resource, entry_name, &observations,
        &source, &quality, false, &diagnostic);
    atomic_roots_dispose(&observations);
    CHECK(!emitted && source.failed && source.len == 6u && !strcmp(source.buf, "prefix"));
    CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK && quality.classification != HLSL_SOURCE_QUALITY_CLEAN);
    sb_free(&source);
    return true;
}

static bool check_atomic_roots(const AtomicObservations *observations, uint32_t value) {
    CHECK(observations->root_count == 3u);
    const ASTExpr *resource = observations->roots[0];
    CHECK(resource->kind == AST_EXPR_EMITTER_OPERAND && resource->operand_provenance.complete &&
        resource->operand_provenance.instruction_index == 0 &&
        resource->operand_provenance.source_instruction_index == 3u &&
        resource->operand_provenance.operand_index == 0);
    for (size_t index = 1; index < observations->root_count; ++index) {
        const ASTExpr *root = observations->roots[index];
        CHECK(root->logical_origin.complete &&
            root->logical_origin.scalar_type == (index == 1u ? AST_SCALAR_SINT32 : AST_SCALAR_UINT32) &&
            root->logical_origin.components == (index == 1u ? 2u : 1u) &&
            root->logical_origin.destination_lanes == (index == 1u ? 3u : 1u) &&
            root->logical_origin.instruction_index == 0 && root->logical_origin.source_instruction_index == 3u);
    }
    const ASTExpr *coordinates = observations->roots[1];
    CHECK(coordinates->kind == AST_EXPR_LITERAL && coordinates->u.literal.scalar_type == AST_SCALAR_SINT32 &&
        coordinates->u.literal.components == 2 && coordinates->u.literal.val[0] == 0u && coordinates->u.literal.val[1] == 0u);
    const ASTExpr *increment = observations->roots[2];
    CHECK(increment->kind == AST_EXPR_LITERAL && increment->u.literal.scalar_type == AST_SCALAR_UINT32 && increment->u.literal.components == 1 &&
        increment->u.literal.val[0] == value);
    return true;
}

static bool literal_atomic_source(void) {
    DXBCDocument document; dxbc_document_init(&document);
    DXBCContainer semantic = {0};
    DXBCStageContract contract; dxbc_stage_contract_init(&contract);
    DXBCStageContractDiagnostic contract_diagnostic;
    USILProgram program = {0};
    CHECK(parse_program(atomic_words, COUNT(atomic_words), 0x00050050u, &document, &semantic));
    CHECK(document.instruction_count == 5u && document.instructions[3].opcode == 173u &&
        document.instructions[3].token_count == 10u && document.instructions[4].opcode == 62u);
    CHECK(dxbc_stage_contract_decode(&document, &semantic, &contract, &contract_diagnostic));
    CHECK(usil_translate_with_stage_contract(&program, &semantic, &contract));
    CHECK(program.instruction_count == 2 && program.temp_count == 0 && program.compute.system_value_mask == 0u &&
        program.uav_count == 1 && program.compute.declaration_source_instruction_index == 2u);
    CHECK(program.instructions[0].opcode == USIL_OP_ATOMIC_IADD && program.instructions[0].operand_count == 3 &&
        program.instructions[0].source_instruction_index == 3u && program.instructions[1].source_instruction_index == 4u);
    USILMemoryAccess access; USILEffectFlags effects;
    CHECK(usil_instruction_memory_access(&program, &program.instructions[0], &access) &&
        usil_instruction_effects(&program, &program.instructions[0], &effects));
    CHECK(access.kind == USIL_MEMORY_TYPED && access.space == USIL_MEMORY_UNORDERED_ACCESS &&
        access.reads && access.writes && access.atomic && access.destination_operand == -1 &&
        access.binding_operand == 0 && access.address_operand == 1 && access.value_operand == 2 &&
        access.address_lanes == 3u && access.value_lanes == 1u && access.memory_component_lanes == 1u &&
        effects == (USIL_EFFECT_RESOURCE_READ | USIL_EFFECT_EXTERNAL_WRITE | USIL_EFFECT_ATOMIC));
    for (int index = 0; index < 3; ++index) {
        USILOperandUseInfo use;
        CHECK(usil_instruction_operand_use(&program, &program.instructions[0], index, &use));
        CHECK(use.use == (index ? USIL_OPERAND_USE_SOURCE : USIL_OPERAND_USE_RESOURCE_BINDING));
        CHECK(use.source_lane_mask == (index == 1 ? 3u : index == 2 ? 1u : 0u));
    }
    char resource_name[] = "AtomicCounts", entry_name[] = "AtomicKernel";
    HLSLComputeTypedResource resource = {.name = resource_name, .binding_register = 0u,
        .writable = true, .scalar_type = AST_SCALAR_UINT32, .scalar_atomic = true};
    AtomicObservations baseline = {.reject_root = SIZE_MAX, .program = &program};
    HLSLSourceQualityResult quality, baseline_quality;
    HLSLEmitDiagnostic diagnostic;
    StringBuilder source; sb_init(&source);
    CHECK(atomic_emit(&program, &resource, entry_name, &baseline, &source, &baseline_quality, true, &diagnostic));
    CHECK(baseline_quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !baseline_quality.reasons &&
        !baseline_quality.counts.incomplete_units && !baseline_quality.counts.residual_total &&
        !baseline_quality.counts.unknown_provenance && baseline.effect_count == 2u);
    CHECK(strstr(source.buf, "void AtomicKernel()") && strstr(source.buf, "InterlockedAdd(") &&
        !strstr(source.buf, "].x") && strstr(source.buf, "return;") && !strstr(source.buf, "dispatchThreadId"));
    CHECK(check_atomic_roots(&baseline, 1u));
    const size_t calls = baseline.calls;
    CHECK(calls >= 3u);
    /* Optional quality/observation requests do not change the source. The
     * existing compute contract deliberately has no expression-source map. */
    for (unsigned option = 0; option < 2; ++option) {
        StringBuilder plain; sb_init(&plain);
        AtomicObservations owned = {.reject_root = SIZE_MAX, .program = &program};
        CHECK(atomic_emit(&program, &resource, entry_name, &owned, &plain,
            option ? &quality : NULL, false, &diagnostic));
        CHECK(plain.len == source.len && !memcmp(plain.buf, source.buf, source.len));
        if (option) CHECK(hlsl_source_quality_results_equal(&baseline_quality, &quality));
        CHECK(check_atomic_roots(&owned, 1u));
        atomic_roots_dispose(&owned); sb_free(&plain);
    }
    StringBuilder declared; sb_init(&declared);
    AtomicObservations declared_roots = {.reject_root = SIZE_MAX, .program = &program};
    HLSLEmitOptions declared_options = HLSL_EMIT_HIGH_LEVEL_OPTIONS_INIT; declared_options.source_quality = &quality;
    const HLSLEmitNames declared_names = {.entry_point = entry_name};
    const HLSLComputeTypedSource declared_typed = {.resources = &resource, .resource_count = 1u,
        .emit_declarations = true, .retain_expression = atomic_retain_expression, .expression_context = &declared_roots};
    CHECK(hlsl_emit_compute_typed_stage(&program, &declared, &declared_names, &declared_options, &declared_typed, &diagnostic));
    CHECK(strstr(declared.buf, "RWTexture2D<uint> AtomicCounts : register(u0);\n") &&
        quality.classification == HLSL_SOURCE_QUALITY_CLEAN && !quality.reasons &&
        quality.counts.resource_declarations == 1u && !quality.counts.incomplete_units);
    CHECK(check_atomic_roots(&declared_roots, 1u));
    atomic_roots_dispose(&declared_roots); sb_free(&declared);
    const USILInstruction original = program.instructions[0];
    const USILComputeContract original_compute = program.compute;
    const USILUav original_uav = program.uavs[0];
    const HLSLComputeTypedResource original_resource = resource;
    const size_t triggers[] = {1u, calls / 2u, calls};
    for (unsigned action = 0; action < 8; ++action) for (size_t timing = 0; timing < COUNT(triggers); ++timing) {
        AtomicObservations changed = {.reject_root = SIZE_MAX, .trigger = triggers[timing], .action = action,
            .program = &program, .resource = &resource, .resource_name = resource_name, .entry_name = entry_name};
        StringBuilder rejected; sb_init(&rejected); sb_append(&rejected, "prefix");
        changed.published_source = &rejected;
        const bool emitted = atomic_emit(&program, &resource, entry_name, &changed, &rejected,
            &quality, true, &diagnostic);
        CHECK(changed.triggered && !emitted && rejected.failed && rejected.len == 6u && !strcmp(rejected.buf, "prefix"));
        CHECK(diagnostic.status != HLSL_EMIT_STATUS_OK && quality.classification == HLSL_SOURCE_QUALITY_FAILED);
        atomic_roots_dispose(&changed); sb_free(&rejected);
        if (action && action != 7u) {
            StringBuilder fresh; sb_init(&fresh);
            AtomicObservations owned = {.reject_root = SIZE_MAX, .program = &program};
            CHECK(atomic_emit(&program, &resource, entry_name, &owned, &fresh, &quality, false, &diagnostic));
            CHECK(quality.classification == HLSL_SOURCE_QUALITY_CLEAN &&
                (action == 6u || fresh.len != source.len || memcmp(fresh.buf, source.buf, source.len)));
            atomic_roots_dispose(&owned); sb_free(&fresh);
        }
        program.instructions[0] = original; program.compute = original_compute; program.uavs[0] = original_uav;
        resource = original_resource; resource_name[0] = 'A'; entry_name[0] = 'A';
        StringBuilder restored; sb_init(&restored);
        AtomicObservations owned = {.reject_root = SIZE_MAX, .program = &program};
        CHECK(atomic_emit(&program, &resource, entry_name, &owned, &restored, &quality, false, &diagnostic));
        CHECK(restored.len == source.len && !memcmp(restored.buf, source.buf, source.len) &&
            hlsl_source_quality_results_equal(&quality, &baseline_quality));
        atomic_roots_dispose(&owned); sb_free(&restored);
    }
    for (size_t reject = 0; reject < 3; ++reject) {
        AtomicObservations owned = {.reject_root = reject, .program = &program};
        StringBuilder rejected; sb_init(&rejected); sb_append(&rejected, "prefix");
        CHECK(!atomic_emit(&program, &resource, entry_name, &owned, &rejected, &quality, false, &diagnostic));
        CHECK(owned.root_count == reject && rejected.failed && rejected.len == 6u && !strcmp(rejected.buf, "prefix"));
        CHECK(quality.classification == HLSL_SOURCE_QUALITY_FAILED);
        atomic_roots_dispose(&owned); sb_free(&rejected);
    }
    /* The actual final closing-syntax observation still precedes publication.
     * Its append allocates an initially empty caller builder; rollback must
     * remove the inserted bytes and terminate it even with an empty prefix. */
    StringBuilder empty; sb_init(&empty);
    AtomicObservations empty_gap = {.reject_root = SIZE_MAX, .trigger = calls, .action = 7u,
        .program = &program, .published_source = &empty};
    CHECK(!atomic_emit(&program, &resource, entry_name, &empty_gap, &empty, &quality, true, &diagnostic));
    CHECK(empty_gap.triggered && empty.failed && !empty.len && (!empty.buf || empty.buf[0] == '\0') &&
        quality.classification == HLSL_SOURCE_QUALITY_FAILED);
    atomic_roots_dispose(&empty_gap); sb_free(&empty);
    StringBuilder fresh_after_gap; sb_init(&fresh_after_gap);
    AtomicObservations after_gap = {.reject_root = SIZE_MAX, .program = &program};
    CHECK(atomic_emit(&program, &resource, entry_name, &after_gap, &fresh_after_gap, &quality, false, &diagnostic));
    CHECK(fresh_after_gap.len == source.len && !memcmp(fresh_after_gap.buf, source.buf, source.len) &&
        hlsl_source_quality_results_equal(&quality, &baseline_quality));
    atomic_roots_dispose(&after_gap); sb_free(&fresh_after_gap);
    /* Mutations are made to the actual decoded owner, without synthesizing
     * parsed authority or deriving scalar width from the return formats alone. */
    for (unsigned mutation = 0; mutation < 20; ++mutation) {
        USILInstruction *atomic = &program.instructions[0];
        switch (mutation) {
            case 0: atomic->operand_count = 2; break;
            case 1: atomic->saturate = true; break;
            case 2: atomic->precise_mask = 1u; break;
            case 3: atomic->operands[2].has_neg = true; break;
            case 4: atomic->operands[1].has_abs = true; break;
            case 5: atomic->operands[2].min_precision = 1u; break;
            case 6: atomic->operands[0].register_index = 1; atomic->operands[0].index_values[0] = 1u; break;
            case 7: atomic->operands[1].imm_values[2] = atomic->operands[1].immediate_words[2] = 1u; break;
            case 8: atomic->operands[2].imm_value_count = atomic->operands[2].immediate_word_count = 4; break;
            case 9: atomic->operands[2].immediate_words[0] ^= 1u; break;
            case 10: program.uavs[0].return_types[0] = 3u; break;
            case 11: strcpy(program.uavs[0].dimension, "3d"); break;
            case 12: program.uavs[0].globally_coherent = true; break;
            case 13: program.uavs[0].has_order_preserving_counter = true; break;
            case 14: program.compute.system_value_mask = USIL_COMPUTE_DISPATCH_THREAD_ID; break;
            case 15: program.temp_count = 1; break;
            case 16: resource.scalar_type = AST_SCALAR_FLOAT32; break;
            case 17: resource.writable = false; break;
            case 18: program.compute.thread_group_size[0] = 0u; break;
            case 19: resource.scalar_atomic = false; break;
        }
        const bool rejected = atomic_source_rejected(&program, &resource, entry_name);
        program.instructions[0] = original; program.compute = original_compute; program.uavs[0] = original_uav;
        resource = original_resource; program.temp_count = 0;
        if (!rejected) fprintf(stderr, "literal atomic boundary mutation=%u\n", mutation);
        CHECK(rejected);
    }
    /* The roots own their names, raw literal words and decoded origins after
     * all original decoder storage has been released. */
    usil_free(&program); dxbc_stage_contract_free(&contract); dxbc_free(&semantic); dxbc_document_free(&document);
    resource_name[0] = 'Z'; entry_name[0] = 'Z';
    CHECK(check_atomic_roots(&baseline, 1u));
    StringBuilder retained; sb_init(&retained); ast_format_expr(baseline.roots[0], &retained);
    CHECK(sb_ok(&retained) && strstr(retained.buf, "AtomicCounts"));
    atomic_roots_dispose(&baseline); sb_free(&retained); sb_free(&source);
    /* Malformed raw instruction extents fail the actual document parser;
     * they never acquire the semantic authority used by the source tests. */
    uint32_t malformed[COUNT(atomic_words)]; memcpy(malformed, atomic_words, sizeof(malformed));
    malformed[9] = 173u;
    dxbc_document_init(&document);
    CHECK(!parse_program(malformed, COUNT(malformed), 0x00050050u, &document, NULL));
    dxbc_document_free(&document);
    memcpy(malformed, atomic_words, sizeof(malformed)); malformed[9] = INSTRUCTION(173u, 12u);
    dxbc_document_init(&document);
    CHECK(!parse_program(malformed, COUNT(malformed), 0x00050050u, &document, NULL));
    dxbc_document_free(&document);
    return true;
}

int main(void) {
    if (!execution_metadata_and_barriers() || !system_values_and_source_gate() ||
        !declaration_rejections() || !shader_model_group_bounds() || !memory_access_projection() ||
        !unsigned_compute_source() || !barrier_intrinsic_source() || !literal_atomic_source()) return 1;
    puts("compute execution-contract projection tests passed");
    return 0;
}
