// SPDX-License-Identifier: GPL-3.0-only

#include "app/release_shader_object_certificate.h"

#include "io/serialized_shader_profile.h"
#include "io/typetree_value_digest.h"

#include <limits.h>
#include <string.h>

typedef enum {
    VALUE_COMPARE_EQUAL = 0,
    VALUE_COMPARE_DIFFERENT,
    VALUE_COMPARE_UNAVAILABLE,
    VALUE_COMPARE_INVALID
} ValueCompare;

typedef struct {
    int subshader;
    int pass;
    int stage;
    int element;
    int platform;
    int archive_entry;
    size_t archive_byte;
} ProblemLocation;

static ProblemLocation no_location(void) {
    ProblemLocation location;
    location.subshader = -1;
    location.pass = -1;
    location.stage = -1;
    location.element = -1;
    location.platform = -1;
    location.archive_entry = -1;
    location.archive_byte = SIZE_MAX;
    return location;
}

void release_shader_object_certificate_options_init(
    ReleaseShaderObjectCertificateOptions* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
}

void release_shader_object_certificate_report_init(
    ReleaseShaderObjectCertificateReport* report) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->status = RELEASE_SHADER_OBJECT_CERTIFICATE_INVALID_ARGUMENT;
    report->first_problem_field = RELEASE_SHADER_FIELD_COUNT;
    report->subshader_index = -1;
    report->pass_index = -1;
    report->stage_index = -1;
    report->element_index = -1;
    report->platform = -1;
    report->archive_entry_index = -1;
    report->archive_byte_offset = SIZE_MAX;
    /* These claims require independent authorities and are never upgraded by
     * a serialized-object comparison. */
    report->source_identity_certified = false;
    report->visual_output_certified = false;
}

static void record_field(ReleaseShaderObjectCertificateReport* report,
                         ReleaseShaderObjectField field,
                         ReleaseShaderObjectFieldStatus status,
                         ProblemLocation location) {
    if (!report || field < 0 || field >= RELEASE_SHADER_FIELD_COUNT ||
        report->fields[field] != RELEASE_SHADER_FIELD_NOT_EVALUATED) {
        return;
    }
    report->fields[field] = status;
    ++report->evaluated_field_count;
    if (status == RELEASE_SHADER_FIELD_MATCH) {
        ++report->matched_field_count;
        return;
    }
    if (report->first_problem_field != RELEASE_SHADER_FIELD_COUNT) return;
    report->first_problem_field = field;
    report->subshader_index = location.subshader;
    report->pass_index = location.pass;
    report->stage_index = location.stage;
    report->element_index = location.element;
    report->platform = location.platform;
    report->archive_entry_index = location.archive_entry;
    report->archive_byte_offset = location.archive_byte;
}

static void fill_remaining_fields(
    ReleaseShaderObjectCertificateReport* report,
    ReleaseShaderObjectFieldStatus status) {
    if (!report) return;
    for (int field = 0; field < RELEASE_SHADER_FIELD_COUNT; ++field) {
        if (report->fields[field] ==
            RELEASE_SHADER_FIELD_NOT_EVALUATED) {
            record_field(report, (ReleaseShaderObjectField)field, status,
                         no_location());
        }
    }
}

static bool string_value_is_valid(const TypeTreeValue* value) {
    return value && value->type == VAL_TYPE_STRING && value->string_val &&
           value->string_length == strlen(value->string_val);
}

static bool value_header_is_valid(const TypeTreeValue* value) {
    return value && value->name && value->type_str &&
           value->type >= VAL_TYPE_INT && value->type <= VAL_TYPE_NONE;
}

static bool value_headers_match(const TypeTreeValue* expected,
                                const TypeTreeValue* actual) {
    return expected && actual && expected->name && actual->name &&
           expected->type_str && actual->type_str &&
           strcmp(expected->name, actual->name) == 0 &&
           strcmp(expected->type_str, actual->type_str) == 0 &&
           expected->type == actual->type;
}

static bool array_value_is_valid(const TypeTreeValue* value) {
    if (!value || value->type != VAL_TYPE_ARRAY ||
        value->array_val.count < 0) {
        return false;
    }
    if (value->array_val.storage == TYPETREE_ARRAY_VALUES) {
        return value->array_val.count == 0 || value->array_val.elements;
    }
    if (value->array_val.storage == TYPETREE_ARRAY_PACKED_BYTES) {
        return value->array_val.count == 0 || value->array_val.packed_bytes;
    }
    return false;
}

static ValueCompare canonical_value_compare(const TypeTreeValue* expected,
                                            const TypeTreeValue* actual);

static bool pair_value(const TypeTreeValue* value,
                       const TypeTreeValue** first,
                       const TypeTreeValue** second) {
    if (!value_header_is_valid(value) || value->type != VAL_TYPE_STRUCT ||
        value->struct_val.count != 2 || !value->struct_val.members ||
        !first || !second) {
        return false;
    }
    *first = typetree_find_child(value, "first");
    *second = typetree_find_child(value, "second");
    return *first && *second;
}

static ValueCompare canonical_map_compare(const TypeTreeValue* expected,
                                          const TypeTreeValue* actual) {
    if (!array_value_is_valid(expected) || !array_value_is_valid(actual) ||
        expected->array_val.storage != TYPETREE_ARRAY_VALUES ||
        actual->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return VALUE_COMPARE_INVALID;
    }
    if (expected->array_val.count != actual->array_val.count) {
        return VALUE_COMPARE_DIFFERENT;
    }

    for (int side = 0; side < 2; ++side) {
        const TypeTreeValue* map = side == 0 ? expected : actual;
        for (int index = 0; index < map->array_val.count; ++index) {
            const TypeTreeValue* key = NULL;
            const TypeTreeValue* value = NULL;
            if (!pair_value(&map->array_val.elements[index], &key, &value)) {
                return VALUE_COMPARE_INVALID;
            }
            (void)value;
            for (int previous = 0; previous < index; ++previous) {
                const TypeTreeValue* previous_key = NULL;
                const TypeTreeValue* previous_value = NULL;
                if (!pair_value(&map->array_val.elements[previous],
                                &previous_key, &previous_value)) {
                    return VALUE_COMPARE_INVALID;
                }
                (void)previous_value;
                ValueCompare duplicate =
                    canonical_value_compare(key, previous_key);
                if (duplicate == VALUE_COMPARE_INVALID) {
                    return VALUE_COMPARE_INVALID;
                }
                if (duplicate == VALUE_COMPARE_EQUAL) {
                    return VALUE_COMPARE_INVALID;
                }
            }
        }
    }

    for (int expected_index = 0;
         expected_index < expected->array_val.count; ++expected_index) {
        const TypeTreeValue* expected_key = NULL;
        const TypeTreeValue* expected_value = NULL;
        if (!pair_value(&expected->array_val.elements[expected_index],
                        &expected_key, &expected_value)) {
            return VALUE_COMPARE_INVALID;
        }
        bool found = false;
        for (int actual_index = 0;
             actual_index < actual->array_val.count; ++actual_index) {
            const TypeTreeValue* actual_key = NULL;
            const TypeTreeValue* actual_value = NULL;
            if (!pair_value(&actual->array_val.elements[actual_index],
                            &actual_key, &actual_value)) {
                return VALUE_COMPARE_INVALID;
            }
            ValueCompare key_result =
                canonical_value_compare(expected_key, actual_key);
            if (key_result == VALUE_COMPARE_INVALID) {
                return VALUE_COMPARE_INVALID;
            }
            if (key_result != VALUE_COMPARE_EQUAL) continue;
            ValueCompare value_result =
                canonical_value_compare(expected_value, actual_value);
            if (value_result != VALUE_COMPARE_EQUAL) return value_result;
            found = true;
            break;
        }
        if (!found) return VALUE_COMPARE_DIFFERENT;
    }
    return VALUE_COMPARE_EQUAL;
}

static ValueCompare canonical_array_compare(const TypeTreeValue* expected,
                                            const TypeTreeValue* actual) {
    if (!array_value_is_valid(expected) || !array_value_is_valid(actual)) {
        return VALUE_COMPARE_INVALID;
    }
    if (expected->array_val.count != actual->array_val.count) {
        return VALUE_COMPARE_DIFFERENT;
    }
    const bool expected_map =
        expected->type_str && strcmp(expected->type_str, "map") == 0;
    const bool actual_map =
        actual->type_str && strcmp(actual->type_str, "map") == 0;
    if (expected_map || actual_map) {
        if (!expected_map || !actual_map) return VALUE_COMPARE_DIFFERENT;
        return canonical_map_compare(expected, actual);
    }
    if (expected->array_val.storage != TYPETREE_ARRAY_VALUES ||
        actual->array_val.storage != TYPETREE_ARRAY_VALUES) {
        for (int index = 0; index < expected->array_val.count; ++index) {
            uint64_t expected_value = 0U;
            uint64_t actual_value = 0U;
            if (!typetree_array_get_uint(expected, index, &expected_value) ||
                !typetree_array_get_uint(actual, index, &actual_value)) {
                return VALUE_COMPARE_INVALID;
            }
            if (expected_value != actual_value) {
                return VALUE_COMPARE_DIFFERENT;
            }
        }
        return VALUE_COMPARE_EQUAL;
    }
    for (int index = 0; index < expected->array_val.count; ++index) {
        ValueCompare result = canonical_value_compare(
            &expected->array_val.elements[index],
            &actual->array_val.elements[index]);
        if (result != VALUE_COMPARE_EQUAL) return result;
    }
    return VALUE_COMPARE_EQUAL;
}

static ValueCompare canonical_value_compare(const TypeTreeValue* expected,
                                            const TypeTreeValue* actual) {
    if (!value_header_is_valid(expected) || !value_header_is_valid(actual)) {
        return VALUE_COMPARE_INVALID;
    }
    if (!value_headers_match(expected, actual)) {
        return VALUE_COMPARE_DIFFERENT;
    }
    switch (expected->type) {
        case VAL_TYPE_INT:
            if (expected->integer_is_unsigned != actual->integer_is_unsigned) {
                return VALUE_COMPARE_DIFFERENT;
            }
            if (expected->integer_is_unsigned) {
                return expected->uint_val == actual->uint_val
                    ? VALUE_COMPARE_EQUAL : VALUE_COMPARE_DIFFERENT;
            }
            return expected->int_val == actual->int_val
                ? VALUE_COMPARE_EQUAL : VALUE_COMPARE_DIFFERENT;
        case VAL_TYPE_FLOAT: {
            uint64_t expected_bits = 0U;
            uint64_t actual_bits = 0U;
            memcpy(&expected_bits, &expected->float_val,
                   sizeof(expected_bits));
            memcpy(&actual_bits, &actual->float_val, sizeof(actual_bits));
            return expected_bits == actual_bits
                ? VALUE_COMPARE_EQUAL : VALUE_COMPARE_DIFFERENT;
        }
        case VAL_TYPE_STRING:
            if (!string_value_is_valid(expected) ||
                !string_value_is_valid(actual)) {
                return VALUE_COMPARE_INVALID;
            }
            return expected->string_length == actual->string_length &&
                   memcmp(expected->string_val, actual->string_val,
                          expected->string_length) == 0
                ? VALUE_COMPARE_EQUAL : VALUE_COMPARE_DIFFERENT;
        case VAL_TYPE_ARRAY:
            return canonical_array_compare(expected, actual);
        case VAL_TYPE_STRUCT:
            if (expected->struct_val.count < 0 ||
                actual->struct_val.count < 0 ||
                (expected->struct_val.count > 0 &&
                 !expected->struct_val.members) ||
                (actual->struct_val.count > 0 &&
                 !actual->struct_val.members)) {
                return VALUE_COMPARE_INVALID;
            }
            if (expected->struct_val.count != actual->struct_val.count) {
                return VALUE_COMPARE_DIFFERENT;
            }
            for (int index = 0; index < expected->struct_val.count; ++index) {
                ValueCompare result = canonical_value_compare(
                    &expected->struct_val.members[index],
                    &actual->struct_val.members[index]);
                if (result != VALUE_COMPARE_EQUAL) return result;
            }
            return VALUE_COMPARE_EQUAL;
        case VAL_TYPE_NONE:
            return VALUE_COMPARE_EQUAL;
    }
    return VALUE_COMPARE_INVALID;
}

static ReleaseShaderObjectFieldStatus value_result_to_field(
    ValueCompare result) {
    switch (result) {
        case VALUE_COMPARE_EQUAL: return RELEASE_SHADER_FIELD_MATCH;
        case VALUE_COMPARE_DIFFERENT: return RELEASE_SHADER_FIELD_MISMATCH;
        case VALUE_COMPARE_UNAVAILABLE:
            return RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE;
        case VALUE_COMPARE_INVALID:
            return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
}

static const TypeTreeValue* required_child(const TypeTreeValue* parent,
                                           const char* name) {
    if (!parent || parent->type != VAL_TYPE_STRUCT || !name) return NULL;
    return typetree_find_child(parent, name);
}

static const TypeTreeValue* required_array(const TypeTreeValue* parent,
                                           const char* name) {
    return typetree_get_array(required_child(parent, name));
}

static ValueCompare compare_named(const TypeTreeValue* expected,
                                  const TypeTreeValue* actual,
                                  const char* name) {
    const TypeTreeValue* expected_value = required_child(expected, name);
    const TypeTreeValue* actual_value = required_child(actual, name);
    if (!expected_value || !actual_value) return VALUE_COMPARE_INVALID;
    return canonical_value_compare(expected_value, actual_value);
}

static ValueCompare combine_compare(ValueCompare aggregate,
                                    ValueCompare next) {
    if (aggregate == VALUE_COMPARE_INVALID ||
        next == VALUE_COMPARE_INVALID) {
        return VALUE_COMPARE_INVALID;
    }
    if (aggregate == VALUE_COMPARE_DIFFERENT ||
        next == VALUE_COMPARE_DIFFERENT) {
        return VALUE_COMPARE_DIFFERENT;
    }
    if (aggregate == VALUE_COMPARE_UNAVAILABLE ||
        next == VALUE_COMPARE_UNAVAILABLE) {
        return VALUE_COMPARE_UNAVAILABLE;
    }
    return VALUE_COMPARE_EQUAL;
}

static ValueCompare compare_named_list(const TypeTreeValue* expected,
                                       const TypeTreeValue* actual,
                                       const char* const* names,
                                       size_t count) {
    ValueCompare result = VALUE_COMPARE_EQUAL;
    for (size_t index = 0U; index < count; ++index) {
        result = combine_compare(
            result, compare_named(expected, actual, names[index]));
    }
    return result;
}

static ValueCompare compare_topology(const TypeTreeValue* expected_parsed,
                                     const TypeTreeValue* actual_parsed,
                                     ProblemLocation* location) {
    const TypeTreeValue* expected_subshaders =
        required_array(expected_parsed, "m_SubShaders");
    const TypeTreeValue* actual_subshaders =
        required_array(actual_parsed, "m_SubShaders");
    if (!array_value_is_valid(expected_subshaders) ||
        !array_value_is_valid(actual_subshaders) ||
        expected_subshaders->array_val.storage != TYPETREE_ARRAY_VALUES ||
        actual_subshaders->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return VALUE_COMPARE_INVALID;
    }
    if (expected_subshaders->array_val.count !=
        actual_subshaders->array_val.count) {
        return VALUE_COMPARE_DIFFERENT;
    }
    for (int subshader = 0;
         subshader < expected_subshaders->array_val.count; ++subshader) {
        const TypeTreeValue* expected_passes = required_array(
            &expected_subshaders->array_val.elements[subshader], "m_Passes");
        const TypeTreeValue* actual_passes = required_array(
            &actual_subshaders->array_val.elements[subshader], "m_Passes");
        if (!array_value_is_valid(expected_passes) ||
            !array_value_is_valid(actual_passes)) {
            if (location) location->subshader = subshader;
            return VALUE_COMPARE_INVALID;
        }
        if (expected_passes->array_val.count !=
            actual_passes->array_val.count) {
            if (location) location->subshader = subshader;
            return VALUE_COMPARE_DIFFERENT;
        }
    }
    return VALUE_COMPARE_EQUAL;
}

typedef enum {
    PASS_FIELD_SUBSHADER_METADATA = 0,
    PASS_FIELD_IDENTITY,
    PASS_FIELD_TAGS,
    PASS_FIELD_RENDER_STATE,
    PASS_FIELD_PROGRAM_VARIANTS,
    PASS_FIELD_PARAMETERS
} ParsedSequenceKind;

static ValueCompare compare_program_part(
    const TypeTreeValue* expected_pass, const TypeTreeValue* actual_pass,
    ParsedSequenceKind kind, ProblemLocation* location) {
    static const char* const program_names[] = {
        "progVertex", "progFragment", "progGeometry",
        "progHull", "progDomain", "progRayTracing",
    };
    for (int stage = 0; stage < 6; ++stage) {
        const TypeTreeValue* expected_program =
            required_child(expected_pass, program_names[stage]);
        const TypeTreeValue* actual_program =
            required_child(actual_pass, program_names[stage]);
        if (!expected_program || !actual_program) {
            if (location) location->stage = stage;
            return VALUE_COMPARE_INVALID;
        }
        ValueCompare result = VALUE_COMPARE_EQUAL;
        if (kind == PASS_FIELD_PROGRAM_VARIANTS) {
            const TypeTreeValue* expected_editor =
                required_array(expected_program, "m_SubPrograms");
            const TypeTreeValue* actual_editor =
                required_array(actual_program, "m_SubPrograms");
            if (!array_value_is_valid(expected_editor) ||
                !array_value_is_valid(actual_editor)) {
                if (location) location->stage = stage;
                return VALUE_COMPARE_INVALID;
            }
            /* Serialized editor subprogram records are intentionally not
             * modeled by the pinned Release projection.  Their presence is
             * not upgraded merely because two opaque trees happen to match. */
            if (expected_editor->array_val.count != 0 ||
                actual_editor->array_val.count != 0) {
                if (location) location->stage = stage;
                return VALUE_COMPARE_UNAVAILABLE;
            }
            static const char* const variant_fields[] = {
                "m_PlayerSubPrograms", "m_ParameterBlobIndices",
            };
            result = compare_named_list(
                expected_program, actual_program, variant_fields,
                sizeof(variant_fields) / sizeof(variant_fields[0]));
        } else {
            result = compare_named(expected_program, actual_program,
                                   "m_CommonParameters");
        }
        if (result != VALUE_COMPARE_EQUAL) {
            if (location) location->stage = stage;
            return result;
        }
    }
    return VALUE_COMPARE_EQUAL;
}

static ValueCompare compare_parsed_sequences(
    const TypeTreeValue* expected_parsed,
    const TypeTreeValue* actual_parsed, ParsedSequenceKind kind,
    ProblemLocation* location) {
    const TypeTreeValue* expected_subshaders =
        required_array(expected_parsed, "m_SubShaders");
    const TypeTreeValue* actual_subshaders =
        required_array(actual_parsed, "m_SubShaders");
    if (!array_value_is_valid(expected_subshaders) ||
        !array_value_is_valid(actual_subshaders) ||
        expected_subshaders->array_val.storage != TYPETREE_ARRAY_VALUES ||
        actual_subshaders->array_val.storage != TYPETREE_ARRAY_VALUES) {
        return VALUE_COMPARE_INVALID;
    }
    if (expected_subshaders->array_val.count !=
        actual_subshaders->array_val.count) {
        return VALUE_COMPARE_DIFFERENT;
    }
    for (int subshader = 0;
         subshader < expected_subshaders->array_val.count; ++subshader) {
        const TypeTreeValue* expected_subshader =
            &expected_subshaders->array_val.elements[subshader];
        const TypeTreeValue* actual_subshader =
            &actual_subshaders->array_val.elements[subshader];
        if (kind == PASS_FIELD_SUBSHADER_METADATA) {
            static const char* const fields[] = {"m_Tags", "m_LOD"};
            ValueCompare result = compare_named_list(
                expected_subshader, actual_subshader, fields,
                sizeof(fields) / sizeof(fields[0]));
            if (result != VALUE_COMPARE_EQUAL) {
                if (location) location->subshader = subshader;
                return result;
            }
            continue;
        }

        const TypeTreeValue* expected_passes =
            required_array(expected_subshader, "m_Passes");
        const TypeTreeValue* actual_passes =
            required_array(actual_subshader, "m_Passes");
        if (!array_value_is_valid(expected_passes) ||
            !array_value_is_valid(actual_passes) ||
            expected_passes->array_val.storage != TYPETREE_ARRAY_VALUES ||
            actual_passes->array_val.storage != TYPETREE_ARRAY_VALUES) {
            if (location) location->subshader = subshader;
            return VALUE_COMPARE_INVALID;
        }
        if (expected_passes->array_val.count !=
            actual_passes->array_val.count) {
            if (location) location->subshader = subshader;
            return VALUE_COMPARE_DIFFERENT;
        }
        for (int pass = 0; pass < expected_passes->array_val.count; ++pass) {
            const TypeTreeValue* expected_pass =
                &expected_passes->array_val.elements[pass];
            const TypeTreeValue* actual_pass =
                &actual_passes->array_val.elements[pass];
            ValueCompare result = VALUE_COMPARE_EQUAL;
            switch (kind) {
                case PASS_FIELD_IDENTITY: {
                    static const char* const fields[] = {
                        "m_EditorDataHash", "m_Platforms", "m_NameIndices",
                        "m_Type", "m_ProgramMask", "m_HasInstancingVariant",
                        "m_HasProceduralInstancingVariant", "m_UseName",
                        "m_Name", "m_TextureName",
                        "m_SerializedKeywordStateMask",
                    };
                    result = compare_named_list(
                        expected_pass, actual_pass, fields,
                        sizeof(fields) / sizeof(fields[0]));
                    break;
                }
                case PASS_FIELD_TAGS:
                    result = compare_named(expected_pass, actual_pass,
                                           "m_Tags");
                    break;
                case PASS_FIELD_RENDER_STATE:
                    result = compare_named(expected_pass, actual_pass,
                                           "m_State");
                    break;
                case PASS_FIELD_PROGRAM_VARIANTS:
                case PASS_FIELD_PARAMETERS:
                    result = compare_program_part(
                        expected_pass, actual_pass, kind, location);
                    break;
                case PASS_FIELD_SUBSHADER_METADATA:
                    return VALUE_COMPARE_INVALID;
            }
            if (result != VALUE_COMPARE_EQUAL) {
                if (location) {
                    location->subshader = subshader;
                    location->pass = pass;
                }
                return result;
            }
        }
    }
    return VALUE_COMPARE_EQUAL;
}

typedef struct {
    int32_t file_id;
    int64_t path_id;
} ParsedPPtr;

static bool parse_pptr(const TypeTreeValue* value, ParsedPPtr* result) {
    if (!value || !result || value->type != VAL_TYPE_STRUCT) return false;
    const TypeTreeValue* file = required_child(value, "m_FileID");
    const TypeTreeValue* path = required_child(value, "m_PathID");
    int64_t file_id = 0;
    int64_t path_id = 0;
    if (!file || !path || !typetree_value_get_int(file, &file_id) ||
        !typetree_value_get_int(path, &path_id) || file_id < INT32_MIN ||
        file_id > INT32_MAX) {
        return false;
    }
    result->file_id = (int32_t)file_id;
    result->path_id = path_id;
    return true;
}

static ReleaseShaderObjectFieldStatus compare_pptr(
    const TypeTreeValue* expected, const TypeTreeValue* actual,
    const ReleaseShaderObjectCertificateOptions* options) {
    ParsedPPtr expected_ptr;
    ParsedPPtr actual_ptr;
    if (!parse_pptr(expected, &expected_ptr) ||
        !parse_pptr(actual, &actual_ptr)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    const bool expected_null =
        expected_ptr.file_id == 0 && expected_ptr.path_id == 0;
    const bool actual_null =
        actual_ptr.file_id == 0 && actual_ptr.path_id == 0;
    if (expected_null || actual_null) {
        return expected_null == actual_null
            ? RELEASE_SHADER_FIELD_MATCH : RELEASE_SHADER_FIELD_MISMATCH;
    }
    if (!options || !options->resolve_reference) {
        return RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE;
    }
    uint8_t expected_id[RELEASE_SHADER_REFERENCE_ID_SIZE];
    uint8_t actual_id[RELEASE_SHADER_REFERENCE_ID_SIZE];
    if (!options->resolve_reference(
            options->reference_user_data, RELEASE_SHADER_SIDE_EXPECTED,
            expected_ptr.file_id, expected_ptr.path_id, expected_id) ||
        !options->resolve_reference(
            options->reference_user_data, RELEASE_SHADER_SIDE_ACTUAL,
            actual_ptr.file_id, actual_ptr.path_id, actual_id)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE;
    }
    return memcmp(expected_id, actual_id, sizeof(expected_id)) == 0
        ? RELEASE_SHADER_FIELD_MATCH : RELEASE_SHADER_FIELD_MISMATCH;
}

static bool values_array(const TypeTreeValue* value) {
    return array_value_is_valid(value) &&
           value->array_val.storage == TYPETREE_ARRAY_VALUES;
}

static ReleaseShaderObjectFieldStatus compare_dependency_ptrs(
    const ShaderObject* expected, const ShaderObject* actual,
    const ReleaseShaderObjectCertificateOptions* options,
    ProblemLocation* location) {
    const TypeTreeValue* expected_dependencies =
        required_array(&expected->root, "m_Dependencies");
    const TypeTreeValue* actual_dependencies =
        required_array(&actual->root, "m_Dependencies");
    if (!values_array(expected_dependencies) ||
        !values_array(actual_dependencies)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    if (expected_dependencies->array_val.count !=
        actual_dependencies->array_val.count) {
        return RELEASE_SHADER_FIELD_MISMATCH;
    }
    for (int index = 0; index < expected_dependencies->array_val.count;
         ++index) {
        ReleaseShaderObjectFieldStatus status = compare_pptr(
            &expected_dependencies->array_val.elements[index],
            &actual_dependencies->array_val.elements[index], options);
        if (status != RELEASE_SHADER_FIELD_MATCH) {
            if (location) location->element = index;
            return status;
        }
    }
    return RELEASE_SHADER_FIELD_MATCH;
}

static bool nonmodifiable_pair(const TypeTreeValue* value,
                               const TypeTreeValue** key,
                               const TypeTreeValue** pointer) {
    return pair_value(value, key, pointer) && string_value_is_valid(*key);
}

static bool nonmodifiable_keys_are_unique(const TypeTreeValue* map) {
    if (!values_array(map)) return false;
    for (int index = 0; index < map->array_val.count; ++index) {
        const TypeTreeValue* key = NULL;
        const TypeTreeValue* pointer = NULL;
        if (!nonmodifiable_pair(&map->array_val.elements[index], &key,
                                &pointer) || !parse_pptr(pointer,
                                                        &(ParsedPPtr){0})) {
            return false;
        }
        for (int previous = 0; previous < index; ++previous) {
            const TypeTreeValue* previous_key = NULL;
            const TypeTreeValue* previous_pointer = NULL;
            if (!nonmodifiable_pair(&map->array_val.elements[previous],
                                    &previous_key, &previous_pointer)) {
                return false;
            }
            (void)previous_pointer;
            if (key->string_length == previous_key->string_length &&
                memcmp(key->string_val, previous_key->string_val,
                       key->string_length) == 0) {
                return false;
            }
        }
    }
    return true;
}

static ReleaseShaderObjectFieldStatus compare_nonmodifiable_textures(
    const ShaderObject* expected, const ShaderObject* actual,
    const ReleaseShaderObjectCertificateOptions* options,
    ProblemLocation* location) {
    const TypeTreeValue* expected_map =
        required_array(&expected->root, "m_NonModifiableTextures");
    const TypeTreeValue* actual_map =
        required_array(&actual->root, "m_NonModifiableTextures");
    if (!nonmodifiable_keys_are_unique(expected_map) ||
        !nonmodifiable_keys_are_unique(actual_map)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    if (expected_map->array_val.count != actual_map->array_val.count) {
        return RELEASE_SHADER_FIELD_MISMATCH;
    }
    for (int expected_index = 0;
         expected_index < expected_map->array_val.count; ++expected_index) {
        const TypeTreeValue* expected_key = NULL;
        const TypeTreeValue* expected_pointer = NULL;
        if (!nonmodifiable_pair(
                &expected_map->array_val.elements[expected_index],
                &expected_key, &expected_pointer)) {
            return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
        }
        bool found = false;
        for (int actual_index = 0;
             actual_index < actual_map->array_val.count; ++actual_index) {
            const TypeTreeValue* actual_key = NULL;
            const TypeTreeValue* actual_pointer = NULL;
            if (!nonmodifiable_pair(
                    &actual_map->array_val.elements[actual_index],
                    &actual_key, &actual_pointer)) {
                return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
            }
            if (expected_key->string_length != actual_key->string_length ||
                memcmp(expected_key->string_val, actual_key->string_val,
                       expected_key->string_length) != 0) {
                continue;
            }
            ReleaseShaderObjectFieldStatus status = compare_pptr(
                expected_pointer, actual_pointer, options);
            if (status != RELEASE_SHADER_FIELD_MATCH) {
                if (location) location->element = expected_index;
                return status;
            }
            found = true;
            break;
        }
        if (!found) {
            if (location) location->element = expected_index;
            return RELEASE_SHADER_FIELD_MISMATCH;
        }
    }
    return RELEASE_SHADER_FIELD_MATCH;
}

static ReleaseShaderObjectFieldStatus compare_baked_flag(
    const ShaderObject* expected, const ShaderObject* actual) {
    const TypeTreeValue* expected_value =
        required_child(&expected->root, "m_ShaderIsBaked");
    const TypeTreeValue* actual_value =
        required_child(&actual->root, "m_ShaderIsBaked");
    int64_t expected_flag = 0;
    int64_t actual_flag = 0;
    if (!expected_value || !actual_value ||
        !typetree_value_get_int(expected_value, &expected_flag) ||
        !typetree_value_get_int(actual_value, &actual_flag) ||
        (expected_flag != 0 && expected_flag != 1) ||
        (actual_flag != 0 && actual_flag != 1)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    return expected_flag == actual_flag
        ? RELEASE_SHADER_FIELD_MATCH : RELEASE_SHADER_FIELD_MISMATCH;
}

static bool archive_shape_is_valid(const ShaderBlobArchive* archive) {
    if (!archive || archive->entry_count < 0 || archive->segment_count <= 0 ||
        !archive->segments || !archive->segment_lengths ||
        (archive->entry_count > 0 && !archive->entries)) {
        return false;
    }
    for (int segment = 0; segment < archive->segment_count; ++segment) {
        if (!archive->segments[segment] ||
            archive->segment_lengths[segment] <= 0) {
            return false;
        }
    }
    for (int entry = 0; entry < archive->entry_count; ++entry) {
        const uint8_t* payload = NULL;
        size_t payload_size = 0U;
        if (!shader_blob_archive_get(archive, entry, &payload,
                                     &payload_size) || !payload) {
            return false;
        }
    }
    return true;
}

ReleaseShaderArchiveCompareStatus
release_shader_archive_compare_canonical(
    const ShaderBlobArchive* expected, const ShaderBlobArchive* actual,
    ReleaseShaderArchiveCompareReport* report) {
    if (report) {
        memset(report, 0, sizeof(*report));
        report->status = RELEASE_SHADER_ARCHIVE_INVALID_ARGUMENT;
        report->first_differing_entry = -1;
        report->first_differing_byte = SIZE_MAX;
    }
    if (!expected || !actual || !report) {
        return RELEASE_SHADER_ARCHIVE_INVALID_ARGUMENT;
    }
    if (!archive_shape_is_valid(expected)) {
        report->status = RELEASE_SHADER_ARCHIVE_EXPECTED_INVALID;
        return report->status;
    }
    if (!archive_shape_is_valid(actual)) {
        report->status = RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID;
        return report->status;
    }
    report->expected_entry_count = (size_t)expected->entry_count;
    report->actual_entry_count = (size_t)actual->entry_count;
    if (expected->stage_count != actual->stage_count) {
        report->status = RELEASE_SHADER_ARCHIVE_STAGE_COUNT_MISMATCH;
        return report->status;
    }
    if (expected->entry_count != actual->entry_count) {
        report->status = RELEASE_SHADER_ARCHIVE_ENTRY_COUNT_MISMATCH;
        return report->status;
    }
    for (int entry = 0; entry < expected->entry_count; ++entry) {
        const uint8_t* expected_payload = NULL;
        const uint8_t* actual_payload = NULL;
        size_t expected_size = 0U;
        size_t actual_size = 0U;
        if (!shader_blob_archive_get(expected, entry, &expected_payload,
                                     &expected_size)) {
            report->status = RELEASE_SHADER_ARCHIVE_EXPECTED_INVALID;
            report->first_differing_entry = entry;
            return report->status;
        }
        if (!shader_blob_archive_get(actual, entry, &actual_payload,
                                     &actual_size)) {
            report->status = RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID;
            report->first_differing_entry = entry;
            return report->status;
        }
        if (expected_size != actual_size) {
            report->status = RELEASE_SHADER_ARCHIVE_ENTRY_SIZE_MISMATCH;
            report->first_differing_entry = entry;
            return report->status;
        }
        size_t byte = 0U;
        while (byte < expected_size &&
               expected_payload[byte] == actual_payload[byte]) {
            ++byte;
        }
        if (byte != expected_size) {
            report->status = RELEASE_SHADER_ARCHIVE_ENTRY_PAYLOAD_MISMATCH;
            report->first_differing_entry = entry;
            report->first_differing_byte = byte;
            return report->status;
        }
        ++report->matched_entry_count;
    }
    report->status = RELEASE_SHADER_ARCHIVE_EQUAL;
    return report->status;
}

static bool platform_array_is_valid(const TypeTreeValue* platforms) {
    if (!values_array(platforms)) return false;
    for (int index = 0; index < platforms->array_val.count; ++index) {
        int64_t value = 0;
        if (!typetree_array_get_int(platforms, index, &value) ||
            value < INT_MIN || value > INT_MAX) {
            return false;
        }
        for (int previous = 0; previous < index; ++previous) {
            int64_t previous_value = 0;
            if (!typetree_array_get_int(platforms, previous,
                                        &previous_value) ||
                previous_value == value) {
                return false;
            }
        }
    }
    return true;
}

static bool platform_array_contains(const TypeTreeValue* platforms,
                                    int platform) {
    if (!platform_array_is_valid(platforms)) return false;
    for (int index = 0; index < platforms->array_val.count; ++index) {
        int64_t value = 0;
        if (!typetree_array_get_int(platforms, index, &value)) return false;
        if (value == platform) return true;
    }
    return false;
}

static ReleaseShaderObjectFieldStatus compare_platform_sets(
    const ShaderObject* expected, const ShaderObject* actual,
    ProblemLocation* location) {
    const TypeTreeValue* expected_platforms =
        required_array(&expected->root, "platforms");
    const TypeTreeValue* actual_platforms =
        required_array(&actual->root, "platforms");
    if (!platform_array_is_valid(expected_platforms) ||
        !platform_array_is_valid(actual_platforms)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    if (expected_platforms->array_val.count !=
        actual_platforms->array_val.count) {
        return RELEASE_SHADER_FIELD_MISMATCH;
    }
    for (int index = 0; index < expected_platforms->array_val.count;
         ++index) {
        int64_t value = 0;
        if (!typetree_array_get_int(expected_platforms, index, &value)) {
            return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
        }
        if (!platform_array_contains(actual_platforms, (int)value)) {
            if (location) location->platform = (int)value;
            return RELEASE_SHADER_FIELD_MISMATCH;
        }
    }
    return RELEASE_SHADER_FIELD_MATCH;
}

static ReleaseShaderObjectFieldStatus compare_compiled_artifacts(
    const ShaderObject* expected, const ShaderObject* actual,
    ReleaseShaderObjectCertificateReport* report,
    ProblemLocation* location) {
    const TypeTreeValue* expected_platforms =
        required_array(&expected->root, "platforms");
    const TypeTreeValue* actual_platforms =
        required_array(&actual->root, "platforms");
    if (!platform_array_is_valid(expected_platforms) ||
        !platform_array_is_valid(actual_platforms)) {
        return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
    }
    if (expected_platforms->array_val.count !=
        actual_platforms->array_val.count) {
        return RELEASE_SHADER_FIELD_MISMATCH;
    }
    for (int index = 0; index < expected_platforms->array_val.count;
         ++index) {
        int64_t raw_platform = 0;
        if (!typetree_array_get_int(expected_platforms, index,
                                    &raw_platform) ||
            !platform_array_contains(actual_platforms, (int)raw_platform)) {
            return RELEASE_SHADER_FIELD_MISMATCH;
        }
        const int platform = (int)raw_platform;
        ShaderBlobArchive expected_archive;
        ShaderBlobArchive actual_archive;
        memset(&expected_archive, 0, sizeof(expected_archive));
        memset(&actual_archive, 0, sizeof(actual_archive));
        const bool expected_open = shader_blob_archive_open(
            &expected->root, platform, &expected_archive);
        const bool actual_open = shader_blob_archive_open(
            &actual->root, platform, &actual_archive);
        if (!expected_open || !actual_open) {
            shader_blob_archive_close(&expected_archive);
            shader_blob_archive_close(&actual_archive);
            if (location) location->platform = platform;
            return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
        }

        ReleaseShaderArchiveCompareReport archive_report;
        ReleaseShaderArchiveCompareStatus archive_status =
            release_shader_archive_compare_canonical(
                &expected_archive, &actual_archive, &archive_report);
        if (SIZE_MAX - report->expected_artifact_count <
                archive_report.expected_entry_count ||
            SIZE_MAX - report->actual_artifact_count <
                archive_report.actual_entry_count ||
            SIZE_MAX - report->matched_artifact_count <
                archive_report.matched_entry_count) {
            shader_blob_archive_close(&expected_archive);
            shader_blob_archive_close(&actual_archive);
            if (location) location->platform = platform;
            return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
        }
        report->expected_artifact_count +=
            archive_report.expected_entry_count;
        report->actual_artifact_count += archive_report.actual_entry_count;
        report->matched_artifact_count += archive_report.matched_entry_count;
        shader_blob_archive_close(&expected_archive);
        shader_blob_archive_close(&actual_archive);

        if (archive_status != RELEASE_SHADER_ARCHIVE_EQUAL) {
            if (location) {
                location->platform = platform;
                location->archive_entry =
                    archive_report.first_differing_entry;
                location->archive_byte =
                    archive_report.first_differing_byte;
            }
            if (archive_status == RELEASE_SHADER_ARCHIVE_EXPECTED_INVALID ||
                archive_status == RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID ||
                archive_status == RELEASE_SHADER_ARCHIVE_INVALID_ARGUMENT) {
                return RELEASE_SHADER_FIELD_AUTHORITY_INVALID;
            }
            return RELEASE_SHADER_FIELD_MISMATCH;
        }
    }
    return RELEASE_SHADER_FIELD_MATCH;
}

static void state_digest_count(CommonSha256Context *hash, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(value >> (i * 8U));
    common_sha256_update(hash, bytes, sizeof(bytes));
}

bool release_shader_render_state_digest(const ShaderObject *object,
                                        uint8_t digest[COMMON_SHA256_DIGEST_SIZE]) {
    if (!object || !digest || !object->decoded ||
        !serialized_shader_profile_validate_value(&object->root, object->profile)) return false;
    const TypeTreeValue *parsed = required_child(&object->root, "m_ParsedForm");
    const TypeTreeValue *subshaders = required_array(parsed, "m_SubShaders");
    if (!array_value_is_valid(subshaders) ||
        subshaders->array_val.storage != TYPETREE_ARRAY_VALUES) return false;
    CommonSha256Context hash;
    common_sha256_init(&hash);
    static const char domain[] = "DXBCSandbox.OrderedReleaseRenderState.v1";
    common_sha256_update(&hash, domain, sizeof(domain));
    state_digest_count(&hash, (uint32_t)subshaders->array_val.count);
    for (int subshader = 0; subshader < subshaders->array_val.count; ++subshader) {
        const TypeTreeValue *passes = required_array(
            &subshaders->array_val.elements[subshader], "m_Passes");
        if (!array_value_is_valid(passes) ||
            passes->array_val.storage != TYPETREE_ARRAY_VALUES) return false;
        state_digest_count(&hash, (uint32_t)passes->array_val.count);
        for (int pass = 0; pass < passes->array_val.count; ++pass) {
            const TypeTreeValue *state = required_child(&passes->array_val.elements[pass], "m_State");
            uint8_t state_digest[COMMON_SHA256_DIGEST_SIZE];
            if (!typetree_value_digest(state, state_digest)) return false;
            common_sha256_update(&hash, state_digest, sizeof(state_digest));
        }
    }
    common_sha256_final(&hash, digest);
    return true;
}

static void record_value_field(
    ReleaseShaderObjectCertificateReport* report,
    ReleaseShaderObjectField field, ValueCompare result,
    ProblemLocation location) {
    record_field(report, field, value_result_to_field(result), location);
}

ReleaseShaderObjectCertificateStatus release_shader_object_certify_equal(
    const ShaderObject* expected, const ShaderObject* actual,
    const ReleaseShaderObjectCertificateOptions* options,
    ReleaseShaderObjectCertificateReport* report) {
    if (!report) return RELEASE_SHADER_OBJECT_CERTIFICATE_INVALID_ARGUMENT;
    release_shader_object_certificate_report_init(report);
    if (!expected || !actual) return report->status;
    if (!expected->decoded || !actual->decoded) {
        report->status = RELEASE_SHADER_OBJECT_CERTIFICATE_INPUT_NOT_DECODED;
        fill_remaining_fields(report,
                              RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE);
        return report->status;
    }

    const bool expected_schema_valid =
        serialized_shader_profile_validate_value(&expected->root,
                                                  expected->profile);
    const bool actual_schema_valid =
        serialized_shader_profile_validate_value(&actual->root,
                                                  actual->profile);
    if (!expected_schema_valid || !actual_schema_valid) {
        record_field(report, RELEASE_SHADER_FIELD_SCHEMA_PROFILE,
                     RELEASE_SHADER_FIELD_AUTHORITY_INVALID, no_location());
        fill_remaining_fields(report,
                              RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE);
        report->status =
            RELEASE_SHADER_OBJECT_CERTIFICATE_SCHEMA_AUTHORITY_INVALID;
        return report->status;
    }
    if (expected->profile != actual->profile ||
        expected->serialized_file_version !=
            actual->serialized_file_version ||
        expected->target_platform != actual->target_platform) {
        record_field(report, RELEASE_SHADER_FIELD_SCHEMA_PROFILE,
                     RELEASE_SHADER_FIELD_MISMATCH, no_location());
        fill_remaining_fields(report,
                              RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE);
        report->status = RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER;
        return report->status;
    }
    record_field(report, RELEASE_SHADER_FIELD_SCHEMA_PROFILE,
                 RELEASE_SHADER_FIELD_MATCH, no_location());

    const TypeTreeValue* expected_parsed =
        required_child(&expected->root, "m_ParsedForm");
    const TypeTreeValue* actual_parsed =
        required_child(&actual->root, "m_ParsedForm");
    if (!expected_parsed || !actual_parsed) {
        fill_remaining_fields(report,
                              RELEASE_SHADER_FIELD_AUTHORITY_INVALID);
        report->status =
            RELEASE_SHADER_OBJECT_CERTIFICATE_SCHEMA_AUTHORITY_INVALID;
        return report->status;
    }

    static const char* const identity_fields[] = {
        "m_Name", "m_CustomEditorName", "m_FallbackName",
        "m_DisableNoSubshadersMessage",
    };
    ValueCompare identity = compare_named(&expected->root, &actual->root,
                                          "m_Name");
    identity = combine_compare(identity, compare_named_list(
        expected_parsed, actual_parsed, identity_fields,
        sizeof(identity_fields) / sizeof(identity_fields[0])));
    record_value_field(report, RELEASE_SHADER_FIELD_SHADER_IDENTITY,
                       identity, no_location());

    record_value_field(
        report, RELEASE_SHADER_FIELD_PROPERTIES,
        compare_named(expected_parsed, actual_parsed, "m_PropInfo"),
        no_location());
    static const char* const keyword_fields[] = {
        "m_KeywordNames", "m_KeywordFlags",
    };
    record_value_field(
        report, RELEASE_SHADER_FIELD_KEYWORDS,
        compare_named_list(expected_parsed, actual_parsed, keyword_fields,
                           sizeof(keyword_fields) /
                               sizeof(keyword_fields[0])),
        no_location());
    record_value_field(
        report, RELEASE_SHADER_FIELD_PARSED_DEPENDENCIES,
        compare_named(expected_parsed, actual_parsed, "m_Dependencies"),
        no_location());
    record_value_field(
        report, RELEASE_SHADER_FIELD_CUSTOM_EDITORS,
        compare_named(expected_parsed, actual_parsed,
                      "m_CustomEditorForRenderPipelines"),
        no_location());

    ProblemLocation location = no_location();
    ValueCompare sequence_result =
        compare_topology(expected_parsed, actual_parsed, &location);
    record_value_field(report, RELEASE_SHADER_FIELD_SUBSHADER_TOPOLOGY,
                       sequence_result, location);
    location = no_location();
    sequence_result = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_SUBSHADER_METADATA,
        &location);
    record_value_field(report, RELEASE_SHADER_FIELD_SUBSHADER_METADATA,
                       sequence_result, location);
    location = no_location();
    sequence_result = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_IDENTITY, &location);
    record_value_field(report, RELEASE_SHADER_FIELD_PASS_IDENTITY,
                       sequence_result, location);
    location = no_location();
    sequence_result = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_TAGS, &location);
    record_value_field(report, RELEASE_SHADER_FIELD_PASS_TAGS,
                       sequence_result, location);
    location = no_location();
    sequence_result = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_RENDER_STATE, &location);
    record_value_field(report, RELEASE_SHADER_FIELD_RENDER_STATE,
                       sequence_result, location);
    location = no_location();
    ValueCompare variants = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_PROGRAM_VARIANTS,
        &location);
    record_value_field(report, RELEASE_SHADER_FIELD_PROGRAM_VARIANTS,
                       variants, location);
    location = no_location();
    sequence_result = compare_parsed_sequences(
        expected_parsed, actual_parsed, PASS_FIELD_PARAMETERS, &location);
    record_value_field(report, RELEASE_SHADER_FIELD_PARAMETER_BINDINGS,
                       sequence_result, location);
    record_value_field(
        report, RELEASE_SHADER_FIELD_PARSED_FORM_RESIDUAL,
        canonical_value_compare(expected_parsed, actual_parsed),
        no_location());

    location = no_location();
    ReleaseShaderObjectFieldStatus field_status = compare_dependency_ptrs(
        expected, actual, options, &location);
    record_field(report, RELEASE_SHADER_FIELD_OBJECT_DEPENDENCY_PTRS,
                 field_status, location);
    location = no_location();
    field_status = compare_nonmodifiable_textures(
        expected, actual, options, &location);
    record_field(report, RELEASE_SHADER_FIELD_NONMODIFIABLE_TEXTURES,
                 field_status, location);
    record_field(report, RELEASE_SHADER_FIELD_SHADER_IS_BAKED,
                 compare_baked_flag(expected, actual), no_location());
    location = no_location();
    field_status = compare_platform_sets(expected, actual, &location);
    record_field(report, RELEASE_SHADER_FIELD_PLATFORM_ARCHIVES,
                 field_status, location);
    location = no_location();
    field_status = compare_compiled_artifacts(
        expected, actual, report, &location);
    record_field(report, RELEASE_SHADER_FIELD_COMPILED_ARTIFACTS,
                 field_status, location);

    bool has_invalid = false;
    bool has_unavailable = false;
    bool has_mismatch = false;
    for (int field = 0; field < RELEASE_SHADER_FIELD_COUNT; ++field) {
        switch (report->fields[field]) {
            case RELEASE_SHADER_FIELD_AUTHORITY_INVALID:
                has_invalid = true;
                break;
            case RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE:
                has_unavailable = true;
                break;
            case RELEASE_SHADER_FIELD_MISMATCH:
                has_mismatch = true;
                break;
            case RELEASE_SHADER_FIELD_MATCH:
            case RELEASE_SHADER_FIELD_NOT_EVALUATED:
                break;
        }
    }
    if (has_invalid) {
        report->status =
            RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_INVALID;
    } else if (has_unavailable) {
        report->status =
            RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE;
    } else if (has_mismatch) {
        report->status = RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER;
    } else {
        report->status = RELEASE_SHADER_OBJECT_CERTIFICATE_OK;
    }
    report->compiled_artifacts_exact =
        report->fields[RELEASE_SHADER_FIELD_COMPILED_ARTIFACTS] ==
        RELEASE_SHADER_FIELD_MATCH;
    report->canonical_release_identity_certified =
        report->status == RELEASE_SHADER_OBJECT_CERTIFICATE_OK &&
        report->evaluated_field_count == RELEASE_SHADER_FIELD_COUNT &&
        report->matched_field_count == RELEASE_SHADER_FIELD_COUNT;
    return report->status;
}

const char* release_shader_object_field_name(ReleaseShaderObjectField field) {
    switch (field) {
        case RELEASE_SHADER_FIELD_SCHEMA_PROFILE: return "schema-profile";
        case RELEASE_SHADER_FIELD_SHADER_IDENTITY: return "shader-identity";
        case RELEASE_SHADER_FIELD_PROPERTIES: return "properties";
        case RELEASE_SHADER_FIELD_KEYWORDS: return "keywords";
        case RELEASE_SHADER_FIELD_PARSED_DEPENDENCIES:
            return "parsed-dependencies";
        case RELEASE_SHADER_FIELD_CUSTOM_EDITORS: return "custom-editors";
        case RELEASE_SHADER_FIELD_SUBSHADER_TOPOLOGY:
            return "subshader-topology";
        case RELEASE_SHADER_FIELD_SUBSHADER_METADATA:
            return "subshader-metadata";
        case RELEASE_SHADER_FIELD_PASS_IDENTITY: return "pass-identity";
        case RELEASE_SHADER_FIELD_PASS_TAGS: return "pass-tags";
        case RELEASE_SHADER_FIELD_RENDER_STATE: return "render-state";
        case RELEASE_SHADER_FIELD_PROGRAM_VARIANTS:
            return "program-variants";
        case RELEASE_SHADER_FIELD_PARAMETER_BINDINGS:
            return "parameter-bindings";
        case RELEASE_SHADER_FIELD_PARSED_FORM_RESIDUAL:
            return "parsed-form-residual";
        case RELEASE_SHADER_FIELD_OBJECT_DEPENDENCY_PTRS:
            return "object-dependency-pptrs";
        case RELEASE_SHADER_FIELD_NONMODIFIABLE_TEXTURES:
            return "nonmodifiable-textures";
        case RELEASE_SHADER_FIELD_SHADER_IS_BAKED:
            return "shader-is-baked";
        case RELEASE_SHADER_FIELD_PLATFORM_ARCHIVES:
            return "platform-archives";
        case RELEASE_SHADER_FIELD_COMPILED_ARTIFACTS:
            return "compiled-artifacts";
        case RELEASE_SHADER_FIELD_COUNT: return "none";
    }
    return "unknown";
}

const char* release_shader_object_field_status_name(
    ReleaseShaderObjectFieldStatus status) {
    switch (status) {
        case RELEASE_SHADER_FIELD_NOT_EVALUATED: return "not-evaluated";
        case RELEASE_SHADER_FIELD_MATCH: return "match";
        case RELEASE_SHADER_FIELD_MISMATCH: return "mismatch";
        case RELEASE_SHADER_FIELD_AUTHORITY_UNAVAILABLE:
            return "authority-unavailable";
        case RELEASE_SHADER_FIELD_AUTHORITY_INVALID:
            return "authority-invalid";
    }
    return "unknown";
}

const char* release_shader_archive_compare_status_name(
    ReleaseShaderArchiveCompareStatus status) {
    switch (status) {
        case RELEASE_SHADER_ARCHIVE_EQUAL: return "equal";
        case RELEASE_SHADER_ARCHIVE_INVALID_ARGUMENT:
            return "invalid-argument";
        case RELEASE_SHADER_ARCHIVE_EXPECTED_INVALID:
            return "expected-invalid";
        case RELEASE_SHADER_ARCHIVE_ACTUAL_INVALID: return "actual-invalid";
        case RELEASE_SHADER_ARCHIVE_STAGE_COUNT_MISMATCH:
            return "stage-count-mismatch";
        case RELEASE_SHADER_ARCHIVE_ENTRY_COUNT_MISMATCH:
            return "entry-count-mismatch";
        case RELEASE_SHADER_ARCHIVE_ENTRY_SIZE_MISMATCH:
            return "entry-size-mismatch";
        case RELEASE_SHADER_ARCHIVE_ENTRY_PAYLOAD_MISMATCH:
            return "entry-payload-mismatch";
    }
    return "unknown";
}

const char* release_shader_object_certificate_status_name(
    ReleaseShaderObjectCertificateStatus status) {
    switch (status) {
        case RELEASE_SHADER_OBJECT_CERTIFICATE_OK: return "ok";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_INVALID_ARGUMENT:
            return "invalid-argument";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_INPUT_NOT_DECODED:
            return "input-not-decoded";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_SCHEMA_AUTHORITY_INVALID:
            return "schema-authority-invalid";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_INVALID:
            return "authority-invalid";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_AUTHORITY_UNAVAILABLE:
            return "authority-unavailable";
        case RELEASE_SHADER_OBJECT_CERTIFICATE_OBJECTS_DIFFER:
            return "objects-differ";
    }
    return "unknown";
}
