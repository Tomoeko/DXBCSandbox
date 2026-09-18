#include "app/glcore_link_certificate.h"
#include "io/serialized_glcore_target.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            return false;                                                    \
        }                                                                    \
    } while (0)

static const uint8_t k_linked_text[] =
    "#ifdef VERTEX\n"
    "void vert_main() {}\n"
    "#endif\n"
    "#ifdef FRAGMENT\n"
    "void frag_main() {}\n"
    "#endif\n";

enum {
    TEST_WRAPPER_CAPACITY = 512,
    TEST_BYTECODE_LENGTH_OFFSET = 28,
};

typedef struct {
    uint8_t wrapper[TEST_WRAPPER_CAPACITY];
    size_t wrapper_size;
    size_t text_offset;
    uint8_t* segments[1];
    int segment_lengths[1];
    BlobEntry entries[1];
    ShaderBlobArchive archive;

    int platforms[1];
    SerializedSubProgram programs[2];
    SerializedSubProgramIdentity identities[2];
    SerializedPass pass;
} SyntheticGLCoreFixture;

static void write_le32(uint8_t* destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)((value >> 8U) & 0xffU);
    destination[2] = (uint8_t)((value >> 16U) & 0xffU);
    destination[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static bool append_u32(SyntheticGLCoreFixture* fixture, uint32_t value) {
    if (!fixture || fixture->wrapper_size > TEST_WRAPPER_CAPACITY - 4U) {
        return false;
    }
    write_le32(fixture->wrapper + fixture->wrapper_size, value);
    fixture->wrapper_size += 4U;
    return true;
}

static bool build_wrapper(SyntheticGLCoreFixture* fixture,
                          int32_t program_type,
                          const uint8_t* text, size_t text_size) {
    if (!fixture || !text || text_size == 0U) return false;
    fixture->wrapper_size = 0U;
    if (!append_u32(fixture, UNITY_2021_3_PLAYER_BLOB_VERSION) ||
        !append_u32(fixture, (uint32_t)program_type) ||
        !append_u32(fixture, 11U) ||
        !append_u32(fixture, 22U) ||
        !append_u32(fixture, 33U) ||
        !append_u32(fixture, 44U) ||
        !append_u32(fixture, 0U) || /* local keyword count */
        !append_u32(fixture, (uint32_t)text_size)) {
        return false;
    }
    fixture->text_offset = fixture->wrapper_size;
    if (text_size > TEST_WRAPPER_CAPACITY - fixture->wrapper_size) {
        return false;
    }
    memcpy(fixture->wrapper + fixture->wrapper_size, text, text_size);
    fixture->wrapper_size += text_size;
    while ((fixture->wrapper_size & 3U) != 0U) {
        if (fixture->wrapper_size >= TEST_WRAPPER_CAPACITY) return false;
        fixture->wrapper[fixture->wrapper_size++] = 0U;
    }
    return append_u32(fixture, 7U) && /* source map */
           append_u32(fixture, 0U);   /* binding count */
}

static bool fixture_init(SyntheticGLCoreFixture* fixture) {
    if (!fixture) return false;
    memset(fixture, 0, sizeof(*fixture));
    if (!build_wrapper(fixture,
                       SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE,
                       k_linked_text, sizeof(k_linked_text) - 1U)) {
        return false;
    }

    fixture->segments[0] = fixture->wrapper;
    fixture->segment_lengths[0] = (int)fixture->wrapper_size;
    fixture->entries[0] = (BlobEntry){
        .offset = 0,
        .length = (int32_t)fixture->wrapper_size,
        .segment = 0,
    };
    fixture->archive = (ShaderBlobArchive){
        .entries = fixture->entries,
        .entry_count = 1,
        .segments = fixture->segments,
        .segment_lengths = fixture->segment_lengths,
        .segment_count = 1,
        .stage_count = SERIALIZED_GLCORE_ARCHIVE_STAGE_COUNT,
    };

    fixture->platforms[0] = SERIALIZED_GLCORE_PLATFORM;
    fixture->pass.has_serialized_platforms = true;
    fixture->pass.platform_count = 1;
    fixture->pass.platforms = fixture->platforms;
    fixture->pass.subprogram_count[UNITY_SERIALIZED_STAGE_VERTEX] = 2;
    fixture->pass.subprograms[UNITY_SERIALIZED_STAGE_VERTEX] =
        fixture->programs;
    fixture->pass.subprogram_identities[UNITY_SERIALIZED_STAGE_VERTEX] =
        fixture->identities;
    for (int index = 0; index < 2; ++index) {
        fixture->programs[index].blob_index = 0;
        fixture->programs[index].program_type =
            SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE;
        fixture->identities[index].hardware_tier_group =
            SERIALIZED_GLCORE_GENERIC_TIER_GROUP;
        fixture->identities[index].inner_subprogram_index = index;
        fixture->identities[index].keyword_scopes_are_explicit = true;
    }
    return true;
}

static SerializedGLCoreTargetInput fixture_input(
    const SyntheticGLCoreFixture* fixture, int subprogram_index) {
    const SerializedGLCoreTargetInput input = {
        .unity_version = "2021.3.29f1",
        .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
        .archive = &fixture->archive,
        .pass = &fixture->pass,
        .shader_path_id = 4815,
        .subshader_index = 2,
        .pass_index = 3,
        .stage_index = UNITY_SERIALIZED_STAGE_VERTEX,
        .flattened_subprogram_index = subprogram_index,
    };
    return input;
}

static bool test_exact_wrapper_and_trailing_newline(void) {
    SyntheticGLCoreFixture fixture;
    CHECK(fixture_init(&fixture));
    const size_t baseline_allocations = g_allocations_count;
    SerializedGLCoreTarget target;
    serialized_glcore_target_init(&target);
    SerializedGLCoreTargetInput input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_OK);
    CHECK(strcmp(target.unity_version, "2021.3.29f1") == 0);
    CHECK(target.wrapper_size == fixture.wrapper_size);
    CHECK(target.wrapper_bytes != fixture.wrapper);
    CHECK(memcmp(target.wrapper_bytes, fixture.wrapper,
                 fixture.wrapper_size) == 0);
    CHECK(target.released_text_size == sizeof(k_linked_text) - 1U);
    CHECK(target.released_text_offset == fixture.text_offset);
    CHECK(memcmp(target.released_text_bytes, k_linked_text,
                 sizeof(k_linked_text) - 1U) == 0);
    CHECK(target.released_text_bytes[target.released_text_size - 1U] == '\n');
    uint8_t* retained_wrapper = target.wrapper_bytes;
    SerializedGLCoreTargetInput rejected = input;
    rejected.compiler_platform = 4;
    CHECK(serialized_glcore_target_open(&target, &rejected) ==
          SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM);
    CHECK(target.wrapper_bytes == retained_wrapper);

    GLCoreLinkCertificateReport report;
    CHECK(glcore_link_certificate_compare_text(
              &target, "2021.3.29f1", k_linked_text,
              sizeof(k_linked_text) - 1U, &report) ==
          GLCORE_LINK_CERTIFICATE_OK);
    CHECK(report.serialized_target_available);
    CHECK(report.response_vector_valid);
    CHECK(report.released_text_exact);
    CHECK(report.first_differing_byte == SIZE_MAX);

    CHECK(glcore_link_certificate_compare_text(
              &target, "2021.3.29f1", k_linked_text,
              sizeof(k_linked_text) - 2U, &report) ==
          GLCORE_LINK_CERTIFICATE_TEXT_MISMATCH);
    CHECK(report.first_differing_byte == sizeof(k_linked_text) - 2U);
    CHECK(report.expected_size == sizeof(k_linked_text) - 1U);
    CHECK(report.actual_size == sizeof(k_linked_text) - 2U);

    const uint8_t saved = fixture.wrapper[fixture.text_offset];
    fixture.wrapper[fixture.text_offset] ^= 0x20U;
    CHECK(target.released_text_bytes[0] == saved);
    fixture.wrapper[fixture.text_offset] = saved;

    serialized_glcore_target_dispose(&target);
    CHECK(g_allocations_count == baseline_allocations);
    return true;
}

static bool test_repeated_blob_identity_and_owner(void) {
    SyntheticGLCoreFixture fixture;
    CHECK(fixture_init(&fixture));
    SerializedGLCoreTarget first;
    SerializedGLCoreTarget second;
    serialized_glcore_target_init(&first);
    serialized_glcore_target_init(&second);
    SerializedGLCoreTargetInput first_input = fixture_input(&fixture, 0);
    SerializedGLCoreTargetInput second_input = fixture_input(&fixture, 1);
    CHECK(serialized_glcore_target_open(&first, &first_input) ==
          SERIALIZED_GLCORE_TARGET_OK);
    CHECK(serialized_glcore_target_open(&second, &second_input) ==
          SERIALIZED_GLCORE_TARGET_OK);
    CHECK(first.owner.archive_entry_index == 0);
    CHECK(second.owner.archive_entry_index == 0);
    CHECK(first.owner.inner_subprogram_index == 0);
    CHECK(second.owner.inner_subprogram_index == 1);
    CHECK(first.wrapper_size == second.wrapper_size);
    CHECK(memcmp(first.wrapper_bytes, second.wrapper_bytes,
                 first.wrapper_size) == 0);

    const GLCoreLinkOutput output = {
        .stage = UNITY_SERIALIZED_STAGE_VERTEX,
        .bytes = first.released_text_bytes,
        .size = first.released_text_size,
    };
    GLCoreGeneratedLinkVector vector = {
        .unity_version = first.unity_version,
        .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
        .program_type = SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE,
        .owner = first.owner,
        .shape = GLCORE_LINK_VECTOR_COMBINED_ONLY,
        .outputs = &output,
        .output_count = 1U,
    };
    GLCoreLinkCertificateReport report;
    CHECK(glcore_link_certificate_compare_vector(&second, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_OWNER_MISMATCH);

    serialized_glcore_target_dispose(&second);
    serialized_glcore_target_dispose(&first);
    return true;
}

static bool test_malformed_wrapper_rejected(void) {
    SyntheticGLCoreFixture fixture;
    SerializedGLCoreTarget target;
    serialized_glcore_target_init(&target);

    CHECK(fixture_init(&fixture));
    write_le32(fixture.wrapper + TEST_BYTECODE_LENGTH_OFFSET,
               TEST_WRAPPER_CAPACITY);
    SerializedGLCoreTargetInput input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID);

    CHECK(fixture_init(&fixture));
    CHECK(fixture.wrapper_size < TEST_WRAPPER_CAPACITY);
    fixture.wrapper[fixture.wrapper_size++] = 0x7fU;
    fixture.entries[0].length = (int32_t)fixture.wrapper_size;
    fixture.segment_lengths[0] = (int)fixture.wrapper_size;
    input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRAPPER_INVALID);

    CHECK(fixture_init(&fixture));
    write_le32(fixture.wrapper, UNITY_2021_3_PLAYER_BLOB_VERSION + 1U);
    input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRAPPER_DIALECT_UNSUPPORTED);

    CHECK(fixture_init(&fixture));
    write_le32(fixture.wrapper + 4U, 7U);
    input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRAPPER_PROGRAM_TYPE_MISMATCH);

    CHECK(fixture_init(&fixture));
    char* serialized_keywords[] = {(char*)"GLCORE_TEST_KEYWORD"};
    fixture.programs[0].local_keyword_count = 1;
    fixture.programs[0].local_keywords = serialized_keywords;
    input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRAPPER_KEYWORDS_MISMATCH);

    CHECK(fixture_init(&fixture));
    const char fragment_marker[] = "#ifdef FRAGMENT";
    uint8_t* marker = (uint8_t*)strstr(
        (char*)fixture.wrapper + fixture.text_offset, fragment_marker);
    CHECK(marker != NULL);
    marker[1] = 'x';
    input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_LINK_TEXT_INVALID);

    serialized_glcore_target_dispose(&target);
    return true;
}

static bool test_platform_type_and_readiness_are_fail_closed(void) {
    SyntheticGLCoreFixture fixture;
    CHECK(fixture_init(&fixture));
    SerializedGLCoreTarget target;
    serialized_glcore_target_init(&target);
    SerializedGLCoreTargetInput input = fixture_input(&fixture, 0);

    input.compiler_platform = 4;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_WRONG_PLATFORM);

    input = fixture_input(&fixture, 0);
    fixture.platforms[0] = 4;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT);

    CHECK(fixture_init(&fixture));
    input = fixture_input(&fixture, 0);
    fixture.programs[0].program_type = 7;
    write_le32(fixture.wrapper + 4U, 7U);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_PROGRAM_TYPE_UNSUPPORTED);

    CHECK(fixture_init(&fixture));
    input = fixture_input(&fixture, 0);
    input.stage_index = UNITY_SERIALIZED_STAGE_FRAGMENT;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID);

    input = fixture_input(&fixture, 0);
    fixture.identities[0].hardware_tier_group = 2;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_LINK_OWNER_INVALID);

    CHECK(fixture_init(&fixture));
    input = fixture_input(&fixture, 0);
    fixture.archive.stage_count = 2U;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_ARCHIVE_STAGE_COUNT_INVALID);

    CHECK(fixture_init(&fixture));
    input = fixture_input(&fixture, 0);
    fixture.entries[0].offset = fixture.segment_lengths[0] + 1;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_ARCHIVE_INVALID);

    CHECK(fixture_init(&fixture));
    input = fixture_input(&fixture, 0);
    fixture.programs[0].blob_index = 1;
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_BLOB_INDEX_INVALID);

    int d3d_platform = 4;
    ShaderObject object;
    shader_object_init(&object);
    CHECK(serialized_glcore_object_readiness(&object) ==
          SERIALIZED_GLCORE_TARGET_NOT_DECODED);
    object.decoded = true;
    object.profile =
        SERIALIZED_SHADER_PROFILE_UNITY_2021_3_29F1_PLAYER_RESOURCES;
    object.shader.archive_platform_count = 1;
    object.shader.archive_platforms = &d3d_platform;
    CHECK(serialized_glcore_object_readiness(&object) ==
          SERIALIZED_GLCORE_TARGET_PLATFORM_ABSENT);
    /* Do not dispose a synthetic object that borrows a stack platform. */
    serialized_glcore_target_dispose(&target);
    return true;
}

static bool test_link_vectors_and_version_authority(void) {
    SyntheticGLCoreFixture fixture;
    CHECK(fixture_init(&fixture));
    SerializedGLCoreTarget target;
    serialized_glcore_target_init(&target);
    SerializedGLCoreTargetInput input = fixture_input(&fixture, 0);
    CHECK(serialized_glcore_target_open(&target, &input) ==
          SERIALIZED_GLCORE_TARGET_OK);

    GLCoreLinkOutput outputs[2] = {
        {
            .stage = UNITY_SERIALIZED_STAGE_VERTEX,
            .bytes = target.released_text_bytes,
            .size = target.released_text_size,
        },
        {
            .stage = UNITY_SERIALIZED_STAGE_FRAGMENT,
            .bytes = NULL,
            .size = 0U,
        },
    };
    GLCoreGeneratedLinkVector vector = {
        .unity_version = "2021.3.29f1",
        .compiler_platform = SERIALIZED_GLCORE_PLATFORM,
        .program_type = SERIALIZED_GLCORE_LINKED_PROGRAM_TYPE,
        .owner = target.owner,
        .shape = GLCORE_LINK_VECTOR_COMBINED_WITH_EMPTY_FRAGMENT,
        .outputs = outputs,
        .output_count = 2U,
    };
    GLCoreLinkCertificateReport report;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_OK);

    outputs[1].bytes = (const uint8_t*)"x";
    outputs[1].size = 1U;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID);
    outputs[1].bytes = NULL;
    outputs[1].size = 0U;

    outputs[0].stage = UNITY_SERIALIZED_STAGE_FRAGMENT;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID);
    outputs[0].stage = UNITY_SERIALIZED_STAGE_VERTEX;

    vector.output_count = 1U;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_VECTOR_SHAPE_INVALID);
    vector.output_count = 2U;

    vector.unity_version = "2021.3.35f1";
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_UNITY_VERSION_MISMATCH);
    vector.unity_version = "2021.3.29f1";
    vector.compiler_platform = 4;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_PLATFORM_MISMATCH);
    vector.compiler_platform = SERIALIZED_GLCORE_PLATFORM;
    vector.program_type = 7;
    CHECK(glcore_link_certificate_compare_vector(&target, &vector, &report) ==
          GLCORE_LINK_CERTIFICATE_PROGRAM_TYPE_MISMATCH);

    input.unity_version = "2021.3.30f1";
    SerializedGLCoreTarget unsupported;
    serialized_glcore_target_init(&unsupported);
    CHECK(serialized_glcore_target_open(&unsupported, &input) ==
          SERIALIZED_GLCORE_TARGET_UNSUPPORTED_UNITY_VERSION);
    serialized_glcore_target_dispose(&unsupported);
    serialized_glcore_target_dispose(&target);
    return true;
}

int main(void) {
    CHECK(test_exact_wrapper_and_trailing_newline());
    CHECK(test_repeated_blob_identity_and_owner());
    CHECK(test_malformed_wrapper_rejected());
    CHECK(test_platform_type_and_readiness_are_fail_closed());
    CHECK(test_link_vectors_and_version_authority());
    CHECK(g_allocations_count == 0U);
    puts("serialized GLCore target unit tests passed");
    return 0;
}
