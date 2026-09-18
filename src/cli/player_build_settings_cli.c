// SPDX-License-Identifier: GPL-3.0-only

#include "common/file_io.h"
#include "common/path_discovery.h"
#include "common/string_builder.h"
#include "io/unity_input.h"
#include "io/unity_player_build_settings.h"
#include "io/unity_player_shader_caps.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OUTPUT_TABLE = 0,
    OUTPUT_JSON,
} OutputFormat;

typedef struct {
    const char* input;
    const char* report;
    OutputFormat format;
} Options;

typedef struct {
    const char* source;
    UnityPlayerBuildSettings settings;
    UnityPlayerShaderCaps shader_caps;
} RecoveredPlayer;

static void print_usage(FILE* output, const char* program) {
    fprintf(output,
        "Usage: %s [--format table|json] [--report FILE] INPUT\n"
        "\n"
        "INPUT is one Unity player folder or its globalgamemanagers file.\n"
        "The exact Unity 2021.3.35f1 BuildSettings object reports the player\n"
        "target platform and serialized m_GraphicsAPIs values. The exact\n"
        "GraphicsSettings object reports the shipped 33-bit platform masks\n"
        "for every serialized shader platform and tier. validAPIs remains a\n"
        "live UnityShaderCompiler session field and is never guessed.\n",
        program);
}

static bool parse_options(int argc, char** argv, Options* options,
                          bool* help) {
    memset(options, 0, sizeof(*options));
    *help = false;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (strcmp(argument, "--help") == 0 ||
            strcmp(argument, "-h") == 0) {
            *help = true;
            continue;
        }
        if (strcmp(argument, "--format") == 0) {
            if (++index >= argc) return false;
            if (strcmp(argv[index], "table") == 0) {
                options->format = OUTPUT_TABLE;
            } else if (strcmp(argv[index], "json") == 0) {
                options->format = OUTPUT_JSON;
            } else {
                return false;
            }
            continue;
        }
        if (strcmp(argument, "--report") == 0) {
            if (++index >= argc || !argv[index][0] || options->report) {
                return false;
            }
            options->report = argv[index];
            continue;
        }
        if (argument[0] == '-' || options->input) return false;
        options->input = argument;
    }
    return *help || options->input != NULL;
}

static const char* path_basename(const char* path) {
    const char* basename = path;
    for (const char* cursor = path; cursor && *cursor; ++cursor) {
        if (*cursor == '/'
#ifdef _WIN32
            || *cursor == '\\'
#endif
        ) {
            basename = cursor + 1U;
        }
    }
    return basename;
}

static void append_json_string(StringBuilder* output, const char* value) {
    if (!value) {
        sb_append(output, "null");
        return;
    }
    sb_append_char(output, '"');
    for (const unsigned char* cursor = (const unsigned char*)value;
         *cursor;) {
        if (*cursor == '"') {
            sb_append(output, "\\\"");
            ++cursor;
        } else if (*cursor == '\\') {
            sb_append(output, "\\\\");
            ++cursor;
        } else if (*cursor == '\b') {
            sb_append(output, "\\b");
            ++cursor;
        } else if (*cursor == '\f') {
            sb_append(output, "\\f");
            ++cursor;
        } else if (*cursor == '\n') {
            sb_append(output, "\\n");
            ++cursor;
        } else if (*cursor == '\r') {
            sb_append(output, "\\r");
            ++cursor;
        } else if (*cursor == '\t') {
            sb_append(output, "\\t");
            ++cursor;
        } else if (*cursor < 0x20U) {
            sb_appendf(output, "\\u%04x", (unsigned)*cursor);
            ++cursor;
        } else if (*cursor < 0x80U) {
            sb_append_char(output, (char)*cursor++);
        } else {
            const unsigned char byte = *cursor;
            size_t sequence_size = 0U;
            if (byte >= 0xc2U && byte <= 0xdfU &&
                cursor[1] >= 0x80U && cursor[1] <= 0xbfU) {
                sequence_size = 2U;
            } else if (byte == 0xe0U && cursor[1] >= 0xa0U &&
                       cursor[1] <= 0xbfU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (((byte >= 0xe1U && byte <= 0xecU) ||
                        (byte >= 0xeeU && byte <= 0xefU)) &&
                       cursor[1] >= 0x80U && cursor[1] <= 0xbfU &&
                       cursor[2] >= 0x80U && cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (byte == 0xedU && cursor[1] >= 0x80U &&
                       cursor[1] <= 0x9fU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU) {
                sequence_size = 3U;
            } else if (byte == 0xf0U && cursor[1] >= 0x90U &&
                       cursor[1] <= 0xbfU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU && cursor[3] >= 0x80U &&
                       cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            } else if (byte >= 0xf1U && byte <= 0xf3U &&
                       cursor[1] >= 0x80U && cursor[1] <= 0xbfU &&
                       cursor[2] >= 0x80U && cursor[2] <= 0xbfU &&
                       cursor[3] >= 0x80U && cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            } else if (byte == 0xf4U && cursor[1] >= 0x80U &&
                       cursor[1] <= 0x8fU && cursor[2] >= 0x80U &&
                       cursor[2] <= 0xbfU && cursor[3] >= 0x80U &&
                       cursor[3] <= 0xbfU) {
                sequence_size = 4U;
            }
            if (sequence_size != 0U) {
                sb_append_len(output, (const char*)cursor, sequence_size);
                cursor += sequence_size;
            } else {
                sb_appendf(output, "\\u%04x", (unsigned)byte);
                ++cursor;
            }
        }
    }
    sb_append_char(output, '"');
}

static void append_shader_caps_json(
    StringBuilder* output, const UnityPlayerShaderCaps* caps) {
    sb_append(output, "\"shader_capabilities\":{\"authority\":");
    append_json_string(output, UNITY_PLAYER_SHADER_CAPS_LAYOUT_AUTHORITY);
    sb_appendf(output,
        ",\"status\":\"exact\",\"class_id\":%d,"
        "\"path_id\":\"%" PRId64 "\",\"byte_offset\":%" PRIu64
        ",\"byte_size\":%u,\"entries\":[",
        UNITY_PLAYER_SHADER_CAPS_CLASS_ID, caps->path_id,
        caps->byte_offset, caps->byte_size);
    bool first = true;
    for (uint32_t platform = 0U;
         platform < UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT; ++platform) {
        const UnityPlayerShaderCapsEntry* entry = &caps->entries[platform];
        if (!entry->present) continue;
        if (!first) sb_append_char(output, ',');
        sb_appendf(output,
            "{\"shader_platform\":%u,\"tier1\":%" PRIu64
            ",\"tier2\":%" PRIu64 ",\"tier3\":%" PRIu64 "}",
            entry->shader_platform, entry->tier_capabilities[0],
            entry->tier_capabilities[1], entry->tier_capabilities[2]);
        first = false;
    }
    sb_append(output, "]},\"compiler_request_fields\":{"
        "\"valid_apis\":null,\"valid_apis_status\":"
        "\"requires-unity-shader-compiler-session\",");
    uint64_t d3d11[UNITY_PLAYER_SHADER_CAPS_TIER_COUNT] = {0U, 0U, 0U};
    bool has_d3d11 = true;
    for (uint32_t tier = 1U;
         tier <= UNITY_PLAYER_SHADER_CAPS_TIER_COUNT; ++tier) {
        if (unity_player_shader_caps_get(
                caps, UNITY_PLAYER_SHADER_PLATFORM_D3D11, tier,
                &d3d11[tier - 1U]) != UNITY_PLAYER_SHADER_CAPS_OK) {
            has_d3d11 = false;
            break;
        }
    }
    sb_append(output, "\"d3d11_platform_caps\":");
    if (has_d3d11) {
        sb_appendf(output,
            "{\"status\":\"exact-serialized\",\"tier1\":%" PRIu64
            ",\"tier2\":%" PRIu64 ",\"tier3\":%" PRIu64 "}",
            d3d11[0], d3d11[1], d3d11[2]);
    } else {
        sb_append(output, "null");
    }
    sb_append(output, ",\"d3d11_platform_caps_status\":");
    append_json_string(output, has_d3d11
        ? "exact-serialized"
        : unity_player_shader_caps_status_name(
              UNITY_PLAYER_SHADER_CAPS_PLATFORM_MISSING));
    sb_append_char(output, '}');
}

static void append_player_json(StringBuilder* output,
                               const RecoveredPlayer* player) {
    const UnityPlayerBuildSettings* settings = &player->settings;
    sb_append(output, "{\"authorities\":{\"build_settings\":");
    append_json_string(output, UNITY_PLAYER_BUILD_SETTINGS_LAYOUT_AUTHORITY);
    sb_append(output, ",\"shader_capabilities\":");
    append_json_string(output, UNITY_PLAYER_SHADER_CAPS_LAYOUT_AUTHORITY);
    sb_append(output, "},\"source\":");
    append_json_string(output, player->source);
    sb_appendf(output,
        ",\"serialized_file_version\":%u,\"unity_version\":",
        settings->serialized_file_version);
    append_json_string(output, settings->unity_version);
    sb_appendf(output,
        ",\"target_platform\":%u,\"build_settings\":{"
        "\"class_id\":%d,\"path_id\":\"%" PRId64 "\","
        "\"byte_offset\":%" PRIu64 ",\"byte_size\":%u,"
        "\"scene_count\":%u,\"preloaded_plugin_count\":%u,"
        "\"enabled_vr_device_count\":%u,\"build_tag_count\":%u,"
        "\"build_guid\":\"",
        settings->target_platform,
        UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID, settings->path_id,
        settings->byte_offset, settings->byte_size,
        settings->scene_count, settings->preloaded_plugin_count,
        settings->enabled_vr_device_count, settings->build_tag_count);
    for (size_t index = 0U; index < sizeof(settings->build_guid); ++index) {
        sb_appendf(output, "%02x", (unsigned)settings->build_guid[index]);
    }
    sb_append(output, "\",\"graphics_apis\":[");
    for (size_t index = 0U; index < settings->graphics_api_count; ++index) {
        if (index != 0U) sb_append_char(output, ',');
        sb_appendf(output, "%" PRId32, settings->graphics_apis[index]);
    }
    sb_append(output, "]},");
    append_shader_caps_json(output, &player->shader_caps);
    sb_append_char(output, '}');
}

static bool render_json(StringBuilder* output,
                        const RecoveredPlayer* players,
                        size_t player_count) {
    sb_appendf(output,
        "{\"report_version\":2,\"status\":\"ok\","
        "\"player_count\":%zu,\"players\":[",
        player_count);
    for (size_t index = 0U; index < player_count; ++index) {
        if (index != 0U) sb_append_char(output, ',');
        append_player_json(output, &players[index]);
    }
    sb_append(output, "]}\n");
    return sb_ok(output);
}

static void append_player_table(StringBuilder* output,
                                const RecoveredPlayer* player) {
    const UnityPlayerBuildSettings* settings = &player->settings;
    const UnityPlayerShaderCaps* caps = &player->shader_caps;
    sb_appendf(output,
        "Source: %s\nBuildSettings authority: %s\n"
        "Shader-capability authority: %s\nSerializedFile: v%u\n"
        "Unity version: %s\nTarget platform: %u\n"
        "BuildSettings: ClassID %d, PathID %" PRId64
        ", offset %" PRIu64 ", size %u\n"
        "Graphics APIs (serialized):",
        player->source, UNITY_PLAYER_BUILD_SETTINGS_LAYOUT_AUTHORITY,
        UNITY_PLAYER_SHADER_CAPS_LAYOUT_AUTHORITY,
        settings->serialized_file_version, settings->unity_version,
        settings->target_platform, UNITY_PLAYER_BUILD_SETTINGS_CLASS_ID,
        settings->path_id, settings->byte_offset, settings->byte_size);
    if (settings->graphics_api_count == 0U) {
        sb_append(output, " <empty>");
    } else {
        for (size_t index = 0U; index < settings->graphics_api_count;
             ++index) {
            sb_appendf(output, "%s%" PRId32,
                       index == 0U ? " " : ", ",
                       settings->graphics_apis[index]);
        }
    }
    sb_appendf(output,
        "\nGraphicsSettings: ClassID %d, PathID %" PRId64
        ", offset %" PRIu64 ", size %u\n"
        "Platform capability masks (exact serialized):\n",
        UNITY_PLAYER_SHADER_CAPS_CLASS_ID, caps->path_id,
        caps->byte_offset, caps->byte_size);
    for (uint32_t platform = 0U;
         platform < UNITY_PLAYER_SHADER_CAPS_PLATFORM_COUNT; ++platform) {
        const UnityPlayerShaderCapsEntry* entry = &caps->entries[platform];
        if (!entry->present) continue;
        sb_appendf(output,
            "  platform %u: tier1=%" PRIu64 ", tier2=%" PRIu64
            ", tier3=%" PRIu64 "\n",
            entry->shader_platform, entry->tier_capabilities[0],
            entry->tier_capabilities[1], entry->tier_capabilities[2]);
    }
    uint64_t d3d11 = 0U;
    UnityPlayerShaderCapsStatus d3d11_status = unity_player_shader_caps_get(
        caps, UNITY_PLAYER_SHADER_PLATFORM_D3D11, 3U, &d3d11);
    if (d3d11_status == UNITY_PLAYER_SHADER_CAPS_OK) {
        sb_appendf(output,
            "D3D11 tier3 platform caps (exact serialized): %" PRIu64
            "\n", d3d11);
    } else {
        sb_appendf(output,
            "D3D11 platform caps: unavailable (%s)\n",
            unity_player_shader_caps_status_name(d3d11_status));
    }
    sb_append(output,
        "Compiler validAPIs: unavailable; this is a live "
        "UnityShaderCompiler session field and is not guessed.\n");
}

static bool render_table(StringBuilder* output,
                         const RecoveredPlayer* players,
                         size_t player_count) {
    for (size_t index = 0U; index < player_count; ++index) {
        if (index != 0U) sb_append_char(output, '\n');
        append_player_table(output, &players[index]);
    }
    return sb_ok(output);
}

static bool publish_report(const char* path,
                           const StringBuilder* report) {
    CommonFileStatus status = common_file_write_new_atomic(
        path, report->buf, report->len);
    if (status == COMMON_FILE_OK) return true;
    if (status != COMMON_FILE_ALREADY_EXISTS) return false;
    CommonFileBytes existing;
    status = common_file_read_regular(path, SIZE_MAX, &existing);
    const bool identical = status == COMMON_FILE_OK &&
        existing.size == report->len &&
        (existing.size == 0U ||
         memcmp(existing.data, report->buf, existing.size) == 0);
    common_file_bytes_dispose(&existing);
    return identical;
}

static void dispose_players(RecoveredPlayer* players, size_t count) {
    if (!players) return;
    for (size_t index = 0U; index < count; ++index) {
        unity_player_build_settings_dispose(&players[index].settings);
    }
    free(players);
}

static int player_build_settings_main(int argc, char** argv) {
    Options options;
    bool help = false;
    if (!parse_options(argc, argv, &options, &help)) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (help) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    CommonPathDiscoveryOptions discovery_options;
    common_path_discovery_options_default(&discovery_options);
    CommonPathDiscoveryResult discovered;
    common_path_discovery_result_init(&discovered);
    const char* inputs[1] = {options.input};
    CommonPathDiscoveryStatus discovery_status = common_path_discover(
        inputs, 1U, &discovery_options, &discovered);
    if (discovery_status != COMMON_PATH_DISCOVERY_OK) {
        fprintf(stderr, "Input discovery failed: %s\n",
                common_path_discovery_status_name(discovery_status));
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }

    size_t candidate_count = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        const CommonDiscoveredPath* path = &discovered.paths[index];
        if (path->explicit_file ||
            strcmp(path_basename(path->path), "globalgamemanagers") == 0) {
            ++candidate_count;
        }
    }
    if (candidate_count == 0U) {
        fprintf(stderr, "No globalgamemanagers file was found.\n");
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }
    if (dxbc_size_multiply_overflows(candidate_count,
                                     sizeof(RecoveredPlayer))) {
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }
    RecoveredPlayer* players = (RecoveredPlayer*)calloc(
        candidate_count, sizeof(*players));
    if (!players) {
        fprintf(stderr, "Could not allocate player metadata inventory.\n");
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }

    size_t player_index = 0U;
    for (size_t index = 0U; index < discovered.count; ++index) {
        const CommonDiscoveredPath* candidate = &discovered.paths[index];
        if (!candidate->explicit_file &&
            strcmp(path_basename(candidate->path),
                   "globalgamemanagers") != 0) {
            continue;
        }
        players[player_index].source = candidate->path;
        unity_player_build_settings_init(&players[player_index].settings);
        unity_player_shader_caps_init(&players[player_index].shader_caps);

        UnityInputProbe probe;
        UnityInputStatus input_status = unity_input_probe_path(
            candidate->path, &probe);
        if (input_status != UNITY_INPUT_OK ||
            probe.kind != UNITY_INPUT_KIND_SERIALIZED_FILE_V22) {
            fprintf(stderr, "%s is not a supported standalone "
                            "SerializedFile v22: %s\n",
                    candidate->path,
                    unity_input_status_name(input_status));
            dispose_players(players, candidate_count);
            common_path_discovery_result_dispose(&discovered);
            return 1;
        }

        CommonFileView view;
        CommonFileStatus file_status = common_file_view_open_regular(
            candidate->path, SIZE_MAX, &view);
        if (file_status != COMMON_FILE_OK) {
            fprintf(stderr, "Could not open %s: %s\n", candidate->path,
                    common_file_status_name(file_status));
            dispose_players(players, candidate_count);
            common_path_discovery_result_dispose(&discovered);
            return 1;
        }
        SerializedFile serialized_file;
        const bool metadata_opened = serialized_file_open_metadata(
            &serialized_file, view.data, view.size);
        UnityPlayerBuildSettingsStatus settings_status =
            UNITY_PLAYER_BUILD_SETTINGS_INVALID_SERIALIZED_FILE;
        UnityPlayerShaderCapsStatus caps_status =
            UNITY_PLAYER_SHADER_CAPS_INVALID_SERIALIZED_FILE;
        if (metadata_opened) {
            settings_status = unity_player_build_settings_decode(
                &players[player_index].settings, &serialized_file);
            caps_status = unity_player_shader_caps_decode(
                &players[player_index].shader_caps, &serialized_file);
            serialized_file_close(&serialized_file);
        }
        file_status = common_file_view_close(&view);
        if (settings_status != UNITY_PLAYER_BUILD_SETTINGS_OK ||
            caps_status != UNITY_PLAYER_SHADER_CAPS_OK ||
            file_status != COMMON_FILE_OK) {
            fprintf(stderr, "Player metadata recovery failed for %s: "
                            "BuildSettings=%s; ShaderCaps=%s%s%s\n",
                    candidate->path,
                    unity_player_build_settings_status_name(
                        settings_status),
                    unity_player_shader_caps_status_name(caps_status),
                    file_status == COMMON_FILE_OK
                        ? "" : "; file identity: ",
                    file_status == COMMON_FILE_OK
                        ? "" : common_file_status_name(file_status));
            dispose_players(players, candidate_count);
            common_path_discovery_result_dispose(&discovered);
            return 1;
        }
        ++player_index;
    }

    StringBuilder json;
    StringBuilder display;
    sb_init(&json);
    sb_init(&display);
    const bool rendered = render_json(&json, players, candidate_count) &&
        (options.format == OUTPUT_JSON
            ? render_json(&display, players, candidate_count)
            : render_table(&display, players, candidate_count));
    bool published = true;
    if (rendered && options.report) {
        published = publish_report(options.report, &json);
    }
    if (!rendered || !published) {
        fprintf(stderr, "%s\n", rendered
            ? "Could not publish report without overwriting different data."
            : "Could not allocate the metadata report.");
        sb_free(&display);
        sb_free(&json);
        dispose_players(players, candidate_count);
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }
    if (display.len != 0U &&
        fwrite(display.buf, 1U, display.len, stdout) != display.len) {
        fprintf(stderr, "Could not write output.\n");
        sb_free(&display);
        sb_free(&json);
        dispose_players(players, candidate_count);
        common_path_discovery_result_dispose(&discovered);
        return 1;
    }

    sb_free(&display);
    sb_free(&json);
    dispose_players(players, candidate_count);
    common_path_discovery_result_dispose(&discovered);
    return 0;
}

#ifdef _WIN32
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int wmain(int argc, wchar_t** wide_argv) {
    if (argc < 0 || !wide_argv ||
        (size_t)argc > SIZE_MAX / sizeof(char*) - 1U) {
        return 2;
    }
    char** argv = (char**)calloc((size_t)argc + 1U, sizeof(*argv));
    if (!argv) return 1;
    int converted = 0;
    for (; converted < argc; ++converted) {
        argv[converted] = common_windows_wide_to_utf8(wide_argv[converted]);
        if (!argv[converted]) break;
    }
    if (converted != argc) {
        DWORD conversion_error = GetLastError();
        for (int index = 0; index < converted; ++index) free(argv[index]);
        free(argv);
        return conversion_error == ERROR_NOT_ENOUGH_MEMORY ? 1 : 2;
    }
    int result = player_build_settings_main(argc, argv);
    for (int index = 0; index < argc; ++index) free(argv[index]);
    free(argv);
    return result;
}
#else
int main(int argc, char** argv) {
    return player_build_settings_main(argc, argv);
}
#endif
