// SPDX-License-Identifier: GPL-3.0-only

#include "common/common.h"
#include "common/file_io.h"
#include "common/sha256.h"
#include "common/windows_utf8.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"
#include "io/typetree_schema_profile.h"
#include "io/typetree_schema_registry.h"

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage: %s validate REGISTRY\n"
            "       %s inspect REGISTRY\n"
            "       %s create REGISTRY BUNDLE --sha256 HEX\n"
            "       %s extend INPUT_REGISTRY OUTPUT_REGISTRY BUNDLE "
            "--sha256 HEX\n"
            "       %s bind-version INPUT_REGISTRY OUTPUT_REGISTRY "
            "SERIALIZED_FILE --sha256 HEX --class-id ID\n",
            program, program, program, program, program);
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

static bool parse_sha256(const char* text,
                         uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!text || strlen(text) != COMMON_SHA256_DIGEST_SIZE * 2U) {
        return false;
    }
    for (size_t index = 0U; index < COMMON_SHA256_DIGEST_SIZE; ++index) {
        int high = hex_value((unsigned char)text[index * 2U]);
        int low = hex_value((unsigned char)text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        digest[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void fprint_sha256(
    FILE* output, const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    for (size_t index = 0U; index < COMMON_SHA256_DIGEST_SIZE; ++index) {
        fprintf(output, "%02x", digest[index]);
    }
}

static void print_sha256(
    const uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    fprint_sha256(stdout, digest);
}

static void fprint_hex(FILE* output, const uint8_t* bytes, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        fprintf(output, "%02x", bytes[index]);
    }
}

static const char* profile_name(TypeTreeSchemaProfileResult profile) {
    switch (profile) {
        case TYPETREE_SCHEMA_PROFILE_UNKNOWN: return "unknown";
        case TYPETREE_SCHEMA_PROFILE_VALID: return "known-valid";
        case TYPETREE_SCHEMA_PROFILE_INVALID: return "known-invalid";
        default: return "unknown-enum";
    }
}

static bool load_registry_canonical(
    const char* path, TypeTreeSchemaRegistry* registry,
    uint8_t out_digest[COMMON_SHA256_DIGEST_SIZE], size_t* out_size) {
    CommonFileBytes file;
    CommonFileStatus file_status = common_file_read_regular(
        path, SIZE_MAX, &file);
    if (file_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not read registry '%s': %s\n", path,
                common_file_status_name(file_status));
        return false;
    }

    TypeTreeSchemaStatus status =
        typetree_schema_registry_deserialize_replace(
            registry, file.data, file.size);
    uint8_t* canonical = NULL;
    size_t canonical_size = 0U;
    if (status == TYPETREE_SCHEMA_OK) {
        status = typetree_schema_registry_serialize(
            registry, &canonical, &canonical_size);
    }
    bool exact = status == TYPETREE_SCHEMA_OK &&
        canonical_size == file.size &&
        (file.size == 0U ||
         memcmp(canonical, file.data, file.size) == 0);
    if (!exact) {
        fprintf(stderr, "Registry '%s' is invalid or non-canonical: %s\n",
                path, typetree_schema_status_name(status));
    } else {
        common_sha256(file.data, file.size, out_digest);
        *out_size = file.size;
    }
    if (canonical) mem_free(canonical, canonical_size);
    common_file_bytes_dispose(&file);
    return exact;
}

static bool learn_pinned_bundle(
    TypeTreeSchemaRegistry* registry, const char* bundle_path,
    const uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE]) {
    CommonFileBytes file;
    CommonFileStatus file_status = common_file_read_regular(
        bundle_path, SIZE_MAX, &file);
    if (file_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not read bundle '%s': %s\n", bundle_path,
                common_file_status_name(file_status));
        return false;
    }
    uint8_t actual_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(file.data, file.size, actual_digest);
    if (memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
        fprintf(stderr, "Bundle SHA-256 mismatch for '%s': expected ",
                bundle_path);
        for (size_t index = 0U; index < sizeof(actual_digest); ++index) {
            fprintf(stderr, "%02x", expected_digest[index]);
        }
        fprintf(stderr, ", got ");
        for (size_t index = 0U; index < sizeof(actual_digest); ++index) {
            fprintf(stderr, "%02x", actual_digest[index]);
        }
        fprintf(stderr, "\n");
        common_file_bytes_dispose(&file);
        return false;
    }

    BundleArchive bundle;
    if (!bundle_open(&bundle, file.data, file.size)) {
        fprintf(stderr, "Pinned input '%s' is not a strict UnityFS bundle\n",
                bundle_path);
        common_file_bytes_dispose(&file);
        return false;
    }

    bool ok = true;
    size_t serialized_count = 0U;
    size_t resource_count = 0U;
    for (int index = 0; index < bundle.directory_count; ++index) {
        const BundleDirectoryInfo* member = &bundle.directories[index];
        BundleMemberKind kind = bundle_member_classify(member);
        const uint8_t* member_data = NULL;
        size_t member_size = 0U;
        if (!bundle_get_member_view(
                &bundle, (size_t)index, &member_data, &member_size)) {
            fprintf(stderr, "Could not read pinned bundle member '%s'\n",
                    member->name ? member->name : "<unnamed>");
            ok = false;
            break;
        }
        if (kind == BUNDLE_MEMBER_RESOURCE) {
            ++resource_count;
            continue;
        }
        if (kind == BUNDLE_MEMBER_DIRECTORY ||
            kind == BUNDLE_MEMBER_DELETED) {
            if (member_size != 0U) {
                fprintf(stderr,
                        "Directory/deleted member '%s' is nonempty\n",
                        member->name ? member->name : "<unnamed>");
                ok = false;
                break;
            }
            continue;
        }
        if (kind != BUNDLE_MEMBER_SERIALIZED_FILE ||
            !member_data || member_size == 0U) {
            fprintf(stderr, "Bundle member '%s' has no exact classification\n",
                    member->name ? member->name : "<unnamed>");
            ok = false;
            break;
        }

        SerializedFile serialized;
        if (!serialized_file_open_with_schema_registry_ex(
                &serialized, member_data, member_size, registry,
                TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT)) {
            fprintf(stderr,
                    "Pinned SerializedFile member '%s' failed strict parsing\n",
                    member->name ? member->name : "<unnamed>");
            ok = false;
            break;
        }
        ++serialized_count;
        serialized_file_close(&serialized);
    }
    if (ok && serialized_count == 0U) {
        fprintf(stderr, "Pinned bundle '%s' contains no SerializedFile\n",
                bundle_path);
        ok = false;
    }
    if (ok) {
        printf("pinned_bundle=%s sha256=", bundle_path);
        print_sha256(actual_digest);
        printf(" serialized_files=%zu resources=%zu schemas=%zu\n",
               serialized_count, resource_count,
               typetree_schema_registry_count(registry));
    }
    bundle_close(&bundle);
    common_file_bytes_dispose(&file);
    return ok;
}

static bool publish_registry_new(
    const TypeTreeSchemaRegistry* registry, const char* path) {
    uint8_t* encoded = NULL;
    size_t encoded_size = 0U;
    TypeTreeSchemaStatus status = typetree_schema_registry_serialize(
        registry, &encoded, &encoded_size);
    if (status != TYPETREE_SCHEMA_OK) {
        fprintf(stderr, "Could not serialize schema registry: %s\n",
                typetree_schema_status_name(status));
        return false;
    }
    CommonFileStatus write_status = common_file_write_new_atomic(
        path, encoded, encoded_size);
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(encoded, encoded_size, digest);
    mem_free(encoded, encoded_size);
    if (write_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not publish new registry '%s': %s\n", path,
                common_file_status_name(write_status));
        return false;
    }
    printf("registry=%s format=%u schemas=%zu bytes=%zu sha256=", path,
           TYPETREE_SCHEMA_REGISTRY_FORMAT_VERSION,
           typetree_schema_registry_count(registry), encoded_size);
    print_sha256(digest);
    printf("\n");
    return true;
}

static bool parse_class_id(const char* text, int32_t* out_class_id) {
    if (!text || !text[0] || !out_class_id) return false;
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0 ||
        (unsigned long)value > INT32_MAX) {
        return false;
    }
    *out_class_id = (int32_t)value;
    return true;
}

static bool bind_pinned_serialized_version(
    TypeTreeSchemaRegistry* registry, const char* evidence_path,
    const uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE],
    int32_t class_id) {
    CommonFileBytes evidence;
    CommonFileStatus file_status = common_file_read_regular(
        evidence_path, SIZE_MAX, &evidence);
    if (file_status != COMMON_FILE_OK) {
        fprintf(stderr, "Could not read evidence '%s': %s\n", evidence_path,
                common_file_status_name(file_status));
        return false;
    }
    uint8_t actual_digest[COMMON_SHA256_DIGEST_SIZE];
    common_sha256(evidence.data, evidence.size, actual_digest);
    if (memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0) {
        fprintf(stderr, "Evidence SHA-256 mismatch for '%s': expected ",
                evidence_path);
        fprint_sha256(stderr, expected_digest);
        fprintf(stderr, ", got ");
        fprint_sha256(stderr, actual_digest);
        fprintf(stderr, "\n");
        common_file_bytes_dispose(&evidence);
        return false;
    }

    SerializedFile file;
    if (!serialized_file_open_metadata(
            &file, evidence.data, evidence.size)) {
        fprintf(stderr, "Pinned evidence '%s' is not a strict SerializedFile\n",
                evidence_path);
        common_file_bytes_dispose(&evidence);
        return false;
    }
    bool ok = !file.type_tree_enabled;
    size_t bound = 0U;
    if (!ok) {
        fprintf(stderr,
                "bind-version evidence must be TypeTree-disabled; schemas "
                "come only from the input registry\n");
    }
    for (int index = 0; ok && index < file.type_count; ++index) {
        TypeTreeType* type = &file.types[index];
        if (type->type_id != class_id) continue;
        TypeTreeSchemaKey key;
        TypeTreeSchemaStatus status = typetree_schema_key_from_type(
            &key, file.version, file.unity_version, class_id, type);
        if (status == TYPETREE_SCHEMA_OK) {
            status = typetree_schema_registry_bind_exact_version(
                registry, &key, TYPETREE_SCHEMA_PROVENANCE_PINNED_INPUT);
        }
        if (status != TYPETREE_SCHEMA_OK) {
            fprintf(stderr,
                    "Could not bind exact schema for class %d in '%s': %s\n",
                    class_id, evidence_path,
                    typetree_schema_status_name(status));
            ok = false;
            break;
        }
        ++bound;
    }
    if (ok && bound == 0U) {
        fprintf(stderr, "Evidence '%s' has no class %d type record\n",
                evidence_path, class_id);
        ok = false;
    }
    if (ok) {
        printf("pinned_serialized=%s sha256=", evidence_path);
        print_sha256(actual_digest);
        printf(" unity_version=%s class_id=%d bindings=%zu schemas=%zu\n",
               file.unity_version, class_id, bound,
               typetree_schema_registry_count(registry));
    }
    serialized_file_close(&file);
    common_file_bytes_dispose(&evidence);
    return ok;
}

static int validate_or_inspect(const char* path, bool inspect) {
    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    uint8_t digest[COMMON_SHA256_DIGEST_SIZE];
    size_t size = 0U;
    bool ok = load_registry_canonical(path, &registry, digest, &size);
    if (ok) {
        printf("%s: format=%u schemas=%zu bytes=%zu sha256=",
               inspect ? "registry" : "valid",
               TYPETREE_SCHEMA_REGISTRY_FORMAT_VERSION,
               typetree_schema_registry_count(&registry), size);
        print_sha256(digest);
        printf("\n");
        for (size_t index = 0U;
             inspect && index < typetree_schema_registry_count(&registry);
             ++index) {
            TypeTreeSchemaKey key;
            const TypeTreeType* schema = NULL;
            TypeTreeSchemaStatus view_status =
                typetree_schema_registry_entry_view(
                    &registry, index, &key, &schema);
            uint8_t shape_digest[COMMON_SHA256_DIGEST_SIZE];
            if (view_status != TYPETREE_SCHEMA_OK || !schema ||
                !typetree_schema_shape_digest(schema, shape_digest)) {
                fprintf(stderr,
                        "Could not inspect canonical schema row %zu: %s\n",
                        index, typetree_schema_status_name(view_status));
                ok = false;
                break;
            }
            TypeTreeSchemaProfileResult profile =
                typetree_schema_validate_known_profile(
                    key.unity_version, key.unity_version_size,
                    key.class_id, key.type_hash, schema);
            printf("key[%zu]: serialized_file_version=%u "
                   "unity_version_size=%zu unity_version_hex=",
                   index, key.serialized_file_version,
                   key.unity_version_size);
            fprint_hex(stdout, (const uint8_t*)key.unity_version,
                       key.unity_version_size);
            printf(" target_platform=not-keyed class_id=%d "
                   "serialized_type_id=%d script_type_index=%u "
                   "has_script_id_hash=%s script_id_hash=",
                   key.class_id, key.serialized_type_id,
                   (unsigned)key.script_type_index,
                   key.has_script_id_hash ? "true" : "false");
            if (key.has_script_id_hash) {
                fprint_hex(stdout, key.script_id_hash,
                           sizeof(key.script_id_hash));
            } else {
                printf("absent");
            }
            printf(" type_hash=");
            fprint_hex(stdout, key.type_hash, sizeof(key.type_hash));
            printf(" stripped=%s reference_type=%s profile=%s "
                   "nodes=%d shape_sha256=",
                   key.is_stripped ? "true" : "false",
                   key.is_ref_type ? "true" : "false",
                   profile_name(profile), schema->node_count);
            print_sha256(shape_digest);
            printf("\n");
        }
    }
    typetree_schema_registry_dispose(&registry);
    return ok ? 0 : 1;
}

static int typetree_schema_main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "validate") == 0) {
        return validate_or_inspect(argv[2], false);
    }
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        return validate_or_inspect(argv[2], true);
    }

    bool bind_version = argc == 9 &&
        strcmp(argv[1], "bind-version") == 0 &&
        strcmp(argv[5], "--sha256") == 0 &&
        strcmp(argv[7], "--class-id") == 0;
    if (bind_version) {
        if (strcmp(argv[2], argv[3]) == 0) {
            fprintf(stderr,
                    "bind-version requires a distinct, new output path\n");
            return 2;
        }
        uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE];
        int32_t class_id = 0;
        if (!parse_sha256(argv[6], expected_digest) ||
            !parse_class_id(argv[8], &class_id)) {
            fprintf(stderr,
                    "bind-version requires a 64-digit SHA-256 and a "
                    "non-negative decimal class ID\n");
            return 2;
        }
        TypeTreeSchemaRegistry registry;
        typetree_schema_registry_init(&registry);
        uint8_t input_digest[COMMON_SHA256_DIGEST_SIZE];
        size_t input_size = 0U;
        bool ok = load_registry_canonical(
            argv[2], &registry, input_digest, &input_size);
        if (ok) {
            ok = bind_pinned_serialized_version(
                &registry, argv[4], expected_digest, class_id);
        }
        if (ok) ok = publish_registry_new(&registry, argv[3]);
        typetree_schema_registry_dispose(&registry);
        return ok ? 0 : 1;
    }

    bool create = argc == 6 && strcmp(argv[1], "create") == 0 &&
                  strcmp(argv[4], "--sha256") == 0;
    bool extend = argc == 7 && strcmp(argv[1], "extend") == 0 &&
                  strcmp(argv[5], "--sha256") == 0;
    if (!create && !extend) {
        print_usage(argv[0]);
        return 2;
    }

    const char* input_registry = extend ? argv[2] : NULL;
    const char* output_registry = extend ? argv[3] : argv[2];
    const char* bundle_path = extend ? argv[4] : argv[3];
    const char* digest_text = extend ? argv[6] : argv[5];
    if (extend && strcmp(input_registry, output_registry) == 0) {
        fprintf(stderr, "extend requires a distinct, new output path\n");
        return 2;
    }
    uint8_t expected_digest[COMMON_SHA256_DIGEST_SIZE];
    if (!parse_sha256(digest_text, expected_digest)) {
        fprintf(stderr, "--sha256 requires exactly 64 hexadecimal digits\n");
        return 2;
    }

    TypeTreeSchemaRegistry registry;
    typetree_schema_registry_init(&registry);
    bool ok = true;
    if (input_registry) {
        uint8_t input_digest[COMMON_SHA256_DIGEST_SIZE];
        size_t input_size = 0U;
        ok = load_registry_canonical(
            input_registry, &registry, input_digest, &input_size);
    }
    if (ok) {
        ok = learn_pinned_bundle(
            &registry, bundle_path, expected_digest);
    }
    if (ok) ok = publish_registry_new(&registry, output_registry);
    typetree_schema_registry_dispose(&registry);
    return ok ? 0 : 1;
}

COMMON_DEFINE_UTF8_MAIN(typetree_schema_main)
