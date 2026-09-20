// SPDX-License-Identifier: GPL-3.0-only

#include "compiler/unity_player_profile.h"
#include "common/file_io.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define TEST_PID() ((unsigned long)_getpid())
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_PID() ((unsigned long)getpid())
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_RMDIR(path) rmdir(path)
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static WholeShaderSubjectDescriptor make_subject(const UnityPlayerProfileSummary *profile) {
    WholeShaderSubjectDescriptor descriptor = {0};
    descriptor.target_shader_path_id = 1;
    descriptor.target_class_id = 48;
    descriptor.serialized_target_platform = 19U;
    descriptor.build_platform = 19U;
    descriptor.compiler_platform = 4;
    descriptor.graphics_api = 2U;
    descriptor.source_residency = WHOLE_SHADER_SOURCE_STANDALONE_SERIALIZED_FILE;
    descriptor.target_member_identity = "";
    descriptor.candidate_logical_name = "Experiment/ProfileFixture";
    descriptor.unity_version = "2021.3.35f1";
    memcpy(descriptor.compiler_profile_digest, profile->compiler_profile_digest, 32U);
    memcpy(descriptor.player_profile_digest, profile->profile_digest, 32U);
    return descriptor;
}

static int check_evidence(const UnityPlayerProfileAuthority *authority,
                          const WholeShaderSubjectDescriptor *descriptor,
                          WholeShaderPlaneStatus expected_status) {
    WholeShaderSubject *subject = NULL;
    WholeShaderEvidence *evidence = NULL;
    CHECK(whole_shader_subject_create(&subject, descriptor) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(unity_player_profile_make_evidence(authority, subject, &evidence) ==
          WHOLE_SHADER_EVIDENCE_OK);
    WholeShaderEvidenceSummary summary;
    CHECK(whole_shader_evidence_describe(evidence, &summary) == WHOLE_SHADER_EVIDENCE_OK);
    CHECK(summary.plane == WHOLE_SHADER_PLANE_PLAYER_PROFILE);
    CHECK(summary.status == expected_status && summary.complete);
    CHECK(summary.expected_item_count == 1U && summary.observed_item_count == 1U);
    CHECK(summary.matched_item_count == (expected_status == WHOLE_SHADER_PLANE_PASS ? 1U : 0U));
    WholeShaderCertificateInput *input = NULL;
    WholeShaderCertificateReport report;
    CHECK(whole_shader_certificate_input_create(
              &input, subject, WHOLE_SHADER_PLANE_BIT(WHOLE_SHADER_PLANE_PLAYER_PROFILE)) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_evaluate(input, &report) ==
          (expected_status == WHOLE_SHADER_PLANE_PASS
               ? WHOLE_SHADER_CERTIFICATE_OK
               : WHOLE_SHADER_CERTIFICATE_REQUESTED_PLANE_FAILED));
    CHECK(report.requested_scope_certified == (expected_status == WHOLE_SHADER_PLANE_PASS));
    CHECK(!report.d3d11_logical_equivalence_certified);
    CHECK(!report.d3d11_byte_equivalence_certified && !report.finite_pixel_observations_certified);
    whole_shader_certificate_input_free(input);
    CHECK(whole_shader_certificate_input_create(&input, subject,
                                                WHOLE_SHADER_D3D11_LOGICAL_REQUIRED_MASK) ==
          WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(whole_shader_certificate_input_add_evidence(input, evidence) ==
          WHOLE_SHADER_CERTIFICATE_ADD_OK);
    CHECK(whole_shader_certificate_evaluate(input, &report) != WHOLE_SHADER_CERTIFICATE_OK);
    CHECK(!report.requested_scope_certified && !report.d3d11_logical_equivalence_certified);
    whole_shader_certificate_input_free(input);
    whole_shader_evidence_free(evidence);
    whole_shader_subject_free(subject);
    return 0;
}

static int check_package(const CommonFileBytes *bytes, const UnityCompileProfile *compiler,
                          const WholeShaderSubjectDescriptor *subject) {
    char root[128], file[192], alias[192], extra[192];
    CHECK(snprintf(root, sizeof(root), "player-package-%lu", TEST_PID()) > 0);
    CHECK(snprintf(file, sizeof(file), "%s/metadata", root) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/alias", root) > 0);
    CHECK(snprintf(extra, sizeof(extra), "%s/additional", root) > 0);
    CHECK(TEST_MKDIR(root) == 0);
    CHECK(common_file_write_new_atomic(file, bytes->data, bytes->size) == COMMON_FILE_OK);
    CHECK(common_file_write_new_atomic(alias, bytes->data, bytes->size) == COMMON_FILE_OK);
    const ShaderRuntimeCaptureLimits bounds = {16U, 8U, 4U, 4096U, 1024U * 1024U,
                                               4U * 1024U * 1024U};
    UnityPlayerPackageAuthority *package = NULL, *other = NULL;
    UnityPlayerPackageDiagnostic diagnostic;
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &bounds, compiler, 7U,
                                              &package, &diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    CHECK(diagnostic.capture_status == SHADER_RUNTIME_CAPTURE_OK);
    CHECK(diagnostic.profile_status == UNITY_PLAYER_PROFILE_OK);
    UnityPlayerPackageSummary captured, repeated;
    CHECK(unity_player_package_describe(package, &captured));
    CHECK(captured.image.file_count == 2U && captured.image.total_bytes == bytes->size * 2U);
    ShaderRuntimeFileIdentity member, alias_member;
    CHECK(unity_player_package_find_file(package, "metadata", &member));
    CHECK(unity_player_package_find_file(package, "alias", &alias_member));
    CHECK(member.size == bytes->size && alias_member.size == bytes->size);
    CHECK(strcmp(member.relative_path, "metadata") == 0);
    CHECK(strcmp(alias_member.relative_path, "alias") == 0);
    CHECK(memcmp(member.content_digest, captured.player.serialized_file_digest, 32U) == 0);
    CHECK(memcmp(member.content_digest, alias_member.content_digest, 32U) == 0);
    const ShaderRuntimeFileIdentity retained = member;
    CHECK(!unity_player_package_find_file(NULL, "metadata", &member));
    CHECK(!unity_player_package_find_file(package, "../metadata", &member));
    CHECK(!unity_player_package_find_file(package, "metadata", NULL));
    CHECK(memcmp(&member, &retained, sizeof(member)) == 0);
    CHECK(memcmp(captured.player.profile_digest, subject->player_profile_digest, 32U) == 0);
    CHECK(check_evidence(unity_player_package_profile(package), subject,
                          WHOLE_SHADER_PLANE_PASS) == 0);
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &bounds, compiler, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    CHECK(unity_player_package_describe(other, &repeated));
    CHECK(memcmp(captured.package_digest, repeated.package_digest, 32U) == 0);
    unity_player_package_free(other);
    CHECK(unity_player_package_capture_d3d11(root, "alias", &bounds, compiler, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    CHECK(unity_player_package_describe(other, &repeated));
    CHECK(memcmp(captured.image.image_digest, repeated.image.image_digest, 32U) == 0);
    CHECK(memcmp(captured.player.profile_digest, repeated.player.profile_digest, 32U) == 0);
    CHECK(memcmp(captured.package_digest, repeated.package_digest, 32U) != 0);
    unity_player_package_free(other);

    CHECK(common_file_write_new_atomic(extra, "extra", 5U) == COMMON_FILE_OK);
    CHECK(!unity_player_package_find_file(package, "additional", &member));
    CHECK(memcmp(&member, &retained, sizeof(member)) == 0);
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &bounds, compiler, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_OK);
    CHECK(unity_player_package_describe(other, &repeated));
    CHECK(memcmp(captured.player.profile_digest, repeated.player.profile_digest, 32U) == 0);
    CHECK(unity_player_package_find_file(other, "additional", &member));
    CHECK(member.size == 5U);
    CHECK(memcmp(captured.image.image_digest, repeated.image.image_digest, 32U) != 0);
    CHECK(memcmp(captured.package_digest, repeated.package_digest, 32U) != 0);
    unity_player_package_free(other);
    CHECK(unity_player_package_capture_d3d11(root, "additional", &bounds, compiler, 7U,
                                              &other, &diagnostic) ==
          UNITY_PLAYER_PACKAGE_PROFILE_REJECTED);
    CHECK(!other && diagnostic.profile_status == UNITY_PLAYER_PROFILE_SERIALIZED_FILE_INVALID);
    CHECK(unity_player_package_capture_d3d11(root, "absent", &bounds, compiler, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_METADATA_ABSENT);
    CHECK(!other);
    const char *invalid[] = {"../metadata", "./metadata", "/metadata", "nested/../metadata",
                            "nested\\metadata", "", "metadata/"};
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(unity_player_package_capture_d3d11(root, invalid[i], &bounds, compiler, 7U,
                                                  &other, &diagnostic) ==
              UNITY_PLAYER_PACKAGE_INVALID_ARGUMENT);
        CHECK(!other);
    }
    UnityCompileProfile changed = *compiler;
    changed.d3d11_capabilities ^= 1U;
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &bounds, &changed, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_PROFILE_REJECTED);
    CHECK(!other && diagnostic.profile_status == UNITY_PLAYER_PROFILE_CAPABILITIES_MISMATCH);
    ShaderRuntimeCaptureLimits too_small = bounds;
    too_small.max_total_bytes = bytes->size;
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &too_small, compiler, 7U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_CAPTURE_FAILED);
    CHECK(!other && diagnostic.capture_status == SHADER_RUNTIME_CAPTURE_LIMIT_EXCEEDED);
    CHECK(unity_player_package_capture_d3d11(root, "metadata", &bounds, compiler, 8U,
                                              &other, &diagnostic) == UNITY_PLAYER_PACKAGE_INVALID_ARGUMENT);
    CHECK(!other);

    CHECK(remove(extra) == 0 && remove(alias) == 0 && remove(file) == 0);
    CHECK(TEST_RMDIR(root) == 0);
    /* Sealed authorities retain identities after the files disappear. */
    CHECK(unity_player_package_describe(package, &repeated));
    CHECK(memcmp(captured.package_digest, repeated.package_digest, 32U) == 0);
    CHECK(unity_player_package_find_file(package, "metadata", &member));
    CHECK(member.relative_path == retained.relative_path && member.size == retained.size);
    CHECK(memcmp(member.content_digest, retained.content_digest, 32U) == 0);
    CHECK(check_evidence(unity_player_package_profile(package), subject,
                          WHOLE_SHADER_PLANE_PASS) == 0);
    unity_player_package_free(package);
    CHECK(!unity_player_package_profile(NULL) && !unity_player_package_describe(NULL, &repeated));
    unity_player_package_free(NULL);
    return 0;
}

static int live_package(const char *root, const char *metadata, const char *profile_path) {
    UnityCompileProfile compiler;
    unity_compile_profile_init(&compiler);
    CHECK(unity_compile_profile_load(profile_path, &compiler) == UNITY_COMPILE_PROFILE_OK);
    const ShaderRuntimeCaptureLimits bounds = {4096U, 512U, 32U, 4096U, 512U * 1024U * 1024U,
                                               2U * 1024U * 1024U * 1024U};
    UnityPlayerPackageAuthority *package = NULL;
    UnityPlayerPackageDiagnostic diagnostic;
    const UnityPlayerPackageStatus status = unity_player_package_capture_d3d11(
        root, metadata, &bounds, &compiler, 7U, &package, &diagnostic);
    CHECK(status == UNITY_PLAYER_PACKAGE_OK);
    UnityPlayerPackageSummary summary;
    CHECK(unity_player_package_describe(package, &summary));
    const WholeShaderSubjectDescriptor subject = make_subject(&summary.player);
    CHECK(check_evidence(unity_player_package_profile(package), &subject, WHOLE_SHADER_PLANE_PASS) == 0);
    char image[65], player[65], membership[65];
    common_sha256_digest_to_hex(summary.image.image_digest, image);
    common_sha256_digest_to_hex(summary.player.profile_digest, player);
    common_sha256_digest_to_hex(summary.package_digest, membership);
    printf("package_files=%zu image=%s player=%s membership=%s\n", summary.image.file_count,
           image, player, membership);
    unity_player_package_free(package);
    return 0;
}

/* Optional paths exercise the same capture against a real private player and
 * separately captured compiler profile without adding either artifact to Git. */
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 3 || argc == 4);
    if (argc == 4)
        return live_package(argv[1], argv[2], argv[3]);
    const char *input = argc == 3 ? argv[1] : DXBC_TEST_PLAYER_PROFILE;
    CommonFileBytes bytes = {0};
    CHECK(common_file_read_regular(input, SIZE_MAX, &bytes) == COMMON_FILE_OK);
    UnityCompileProfile compiler;
    unity_compile_profile_init(&compiler);
    if (argc == 3) {
        CHECK(unity_compile_profile_load(argv[2], &compiler) == UNITY_COMPILE_PROFILE_OK);
    } else {
        compiler.build_platform = 19U;
        compiler.valid_apis = 311856U;
        compiler.d3d11_capabilities = 147179016U;
        compiler.glcore_capabilities = 147179016U;
        strcpy(compiler.provenance, "synthetic-player-profile-test");
    }
    UnityPlayerProfileAuthority *authority = NULL;
    UnityPlayerProfileDiagnostic diagnostic;
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 7U, &authority,
                                             &diagnostic) == UNITY_PLAYER_PROFILE_OK);
    CHECK(diagnostic.build_settings_status == UNITY_PLAYER_BUILD_SETTINGS_OK);
    CHECK(diagnostic.shader_caps_status == UNITY_PLAYER_SHADER_CAPS_OK);
    UnityPlayerProfileSummary captured;
    CHECK(unity_player_profile_describe(authority, &captured));
    uint8_t digest[32];
    common_sha256(bytes.data, bytes.size, digest);
    CHECK(memcmp(digest, captured.serialized_file_digest, 32U) == 0);
    CHECK(captured.selected_tiers == 7U);
    WholeShaderSubjectDescriptor descriptor = make_subject(&captured);
    CHECK(check_package(&bytes, &compiler, &descriptor) == 0);
    CHECK(check_evidence(authority, &descriptor, WHOLE_SHADER_PLANE_PASS) == 0);
    descriptor.player_profile_digest[0] ^= 1U;
    CHECK(check_evidence(authority, &descriptor, WHOLE_SHADER_PLANE_FAIL) == 0);
    descriptor.player_profile_digest[0] ^= 1U;

    for (unsigned mutation = 0U; mutation < 6U; ++mutation) {
        WholeShaderSubjectDescriptor changed = descriptor;
        switch (mutation) {
        case 0U:
            changed.build_platform = 5U;
            break;
        case 1U:
            changed.serialized_target_platform = 5U;
            break;
        case 2U:
            changed.compiler_platform = 15;
            break;
        case 3U:
            changed.graphics_api = 17U;
            break;
        case 4U:
            changed.unity_version = "2021.3.34f1";
            break;
        case 5U:
            changed.compiler_profile_digest[0] ^= 1U;
            break;
        }
        WholeShaderSubject *subject = NULL;
        WholeShaderEvidence *evidence = NULL;
        CHECK(whole_shader_subject_create(&subject, &changed) == WHOLE_SHADER_SUBJECT_OK);
        CHECK(unity_player_profile_make_evidence(authority, subject, &evidence) ==
              WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
        CHECK(evidence == NULL);
        whole_shader_subject_free(subject);
    }
    UnityPlayerProfileAuthority *rejected = NULL;
    UnityCompileProfile changed = compiler;
    changed.d3d11_capabilities ^= 1U;
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &changed, 7U, &rejected,
                                             &diagnostic) ==
          UNITY_PLAYER_PROFILE_CAPABILITIES_MISMATCH);
    CHECK(!rejected && diagnostic.mismatched_tier == 1U);
    changed = compiler;
    changed.build_platform = 5U;
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &changed, 7U, &rejected,
                                             &diagnostic) ==
          UNITY_PLAYER_PROFILE_BUILD_TARGET_MISMATCH);
    CHECK(!rejected);
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 0U, &rejected,
                                             &diagnostic) == UNITY_PLAYER_PROFILE_INVALID_ARGUMENT);
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 8U, &rejected,
                                             &diagnostic) == UNITY_PLAYER_PROFILE_INVALID_ARGUMENT);
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size - 1U, &compiler, 7U, &rejected,
                                             &diagnostic) ==
          UNITY_PLAYER_PROFILE_SERIALIZED_FILE_INVALID);
    CHECK(!rejected);
    CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 1U, &rejected,
                                             &diagnostic) == UNITY_PLAYER_PROFILE_OK);
    WholeShaderSubject *subject = NULL;
    WholeShaderEvidence *evidence = NULL;
    CHECK(whole_shader_subject_create(&subject, &descriptor) == WHOLE_SHADER_SUBJECT_OK);
    CHECK(unity_player_profile_make_evidence(rejected, subject, &evidence) ==
          WHOLE_SHADER_EVIDENCE_INVALID_ARGUMENT);
    CHECK(!evidence);
    whole_shader_subject_free(subject);
    unity_player_profile_free(rejected);
    rejected = NULL;

    if (argc == 1) {
        SerializedFile file;
        CHECK(serialized_file_open_metadata(&file, bytes.data, bytes.size));
        const AssetObjectInfo *build = serialized_file_get_object(&file, 1);
        const AssetObjectInfo *caps = serialized_file_get_object(&file, 2);
        CHECK(build && caps && build->byte_size == 72U && caps->byte_size == 294U);
        size_t build_start = (size_t)(file.data_offset + build->byte_offset);
        size_t caps_start = (size_t)(file.data_offset + caps->byte_offset);
        serialized_file_close(&file);
        bytes.data[build_start + 68U] = 17U; /* GLCore instead of D3D11. */
        CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 7U, &rejected,
                                                 &diagnostic) ==
              UNITY_PLAYER_PROFILE_GRAPHICS_API_UNSUPPORTED);
        bytes.data[build_start + 68U] = 2U;
        for (unsigned tier = 0U; tier < 3U; ++tier) {
            size_t low_word = caps_start + 244U + tier * 12U;
            bytes.data[low_word] ^= 1U;
            CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 7U,
                                                     &rejected, &diagnostic) ==
                  UNITY_PLAYER_PROFILE_CAPABILITIES_MISMATCH);
            CHECK(!rejected && diagnostic.mismatched_tier == tier + 1U);
            bytes.data[low_word] ^= 1U;
        }
        /* Changing a non-capability build field must also invalidate identity. */
        bytes.data[build_start + 16U] ^= 1U; /* Build GUID. */
        CHECK(unity_player_profile_capture_d3d11(bytes.data, bytes.size, &compiler, 7U, &rejected,
                                                 &diagnostic) == UNITY_PLAYER_PROFILE_OK);
        CHECK(check_evidence(rejected, &descriptor, WHOLE_SHADER_PLANE_FAIL) == 0);
        unity_player_profile_free(rejected);
    }
    common_file_bytes_dispose(&bytes);
    /* A capture never borrows the mutable source buffer. */
    UnityPlayerProfileSummary retained;
    CHECK(unity_player_profile_describe(authority, &retained));
    CHECK(memcmp(retained.profile_digest, captured.profile_digest, 32U) == 0);
    CHECK(check_evidence(authority, &descriptor, WHOLE_SHADER_PLANE_PASS) == 0);
    char hex[65];
    common_sha256_digest_to_hex(captured.profile_digest, hex);
    printf("captured_player_profile=%s tiers=1,2,3 checks=passed\n", hex);
    unity_player_profile_free(authority);
    return 0;
}
