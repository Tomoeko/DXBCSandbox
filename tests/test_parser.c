#include "common/common.h"
#include "common/file_io.h"
#include "io/bundle_archive.h"
#include "io/serialized_file.h"

static void dump_serialized_file(SerializedFile* file) {
    printf("--- Serialized File Info ---\n");
    printf("Version: %u\n", file->version);
    printf("Declared File Size: %llu\n",
           (unsigned long long)file->file_size);
    printf("Metadata Size: %llu\n",
           (unsigned long long)file->metadata_size);
    printf("Data Offset: %llu\n",
           (unsigned long long)file->data_offset);
    printf("Unity Version: %s\n", file->unity_version ? file->unity_version : "N/A");
    printf("Target Platform: %u\n", file->target_platform);
    printf("Type Tree Enabled: %s\n", file->type_tree_enabled ? "Yes" : "No");
    printf("Type Count: %d\n", file->type_count);
    printf("Object Count: %d\n", file->object_count);
    printf("Script Count: %d\n", file->script_count);
    printf("External Count: %d\n", file->external_count);
    printf("Ref Type Count: %d\n", file->ref_type_count);

    printf("\n--- Object Table ---\n");
    for (int i = 0; i < file->object_count; i++) {
        const AssetObjectInfo* object = &file->objects[i];
        printf("Object [%d]: PathID=%lld, ClassID=%d, TypeIndex=%d, "
               "Offset=%llu, Size=%u\n",
               i, (long long)object->path_id, object->type_id,
               object->type_id_or_index,
               (unsigned long long)object->byte_offset,
               object->byte_size);
    }
    
    printf("\n--- Type Tree Dumps (First 5 types) ---\n");
    int type_limit = file->type_count < 5 ? file->type_count : 5;
    for (int i = 0; i < type_limit; i++) {
        TypeTreeType* t = &file->types[i];
        printf("Type [%d]: ClassID=%d, Strip=%d, Nodes=%d\n", i, t->type_id, t->is_stripped, t->node_count);
        if (t->node_count > 0) {
            printf("  Root Node: Type=\"%s\", Name=\"%s\"\n", t->nodes[0].type_str, t->nodes[0].name_str);
            // Dump detailed layout of the root and a few children
            int node_limit = t->node_count < 10 ? t->node_count : 10;
            for (int n = 0; n < node_limit; n++) {
                TypeTreeNode* node = &t->nodes[n];
                printf("    Node[%d]: Level=%d, Type=\"%s\", Name=\"%s\", Size=%d, MetaFlags=0x%X\n",
                       n, node->level, node->type_str, node->name_str, node->byte_size, node->meta_flags);
            }
            if (t->node_count > node_limit) {
                printf("    ... and %d more nodes\n", t->node_count - node_limit);
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <path_to_bundle_or_assets_file>\n", argv[0]);
        return 1;
    }
    
    const char* path = argv[1];
    printf("Reading file: %s\n", path);
    
    CommonFileBytes input;
    CommonFileStatus file_status =
        common_file_read_regular(path, SIZE_MAX, &input);
    if (file_status != COMMON_FILE_OK) {
        LOG_ERROR("Failed to read file %s: %s", path,
                  common_file_status_name(file_status));
        return 1;
    }
    const uint8_t* file_data = input.data;
    const size_t file_size = input.size;
    
    printf("File size: %zu bytes\n", file_size);
    
    // Check magic signature at the start to determine if it is a bundle
    bool is_bundle =
        (file_size >= 7U && memcmp(file_data, "UnityFS", 7U) == 0) ||
        (file_size >= 12U &&
         memcmp(file_data, "UnityArchive", 12U) == 0) ||
        (file_size >= 8U && memcmp(file_data, "UnityWeb", 8U) == 0) ||
        (file_size >= 8U && memcmp(file_data, "UnityRaw", 8U) == 0);
    
    if (is_bundle) {
        printf("Detected AssetBundle container. Parsing...\n");
        BundleArchive archive;
        if (!bundle_open(&archive, file_data, file_size)) {
            LOG_ERROR("Failed to open AssetBundle");
            common_file_bytes_dispose(&input);
            return 1;
        }
        
        printf("\n--- Bundle Archive Info ---\n");
        printf("Signature: %s\n", archive.signature);
        printf("Version: %u\n", archive.version);
        printf("Generation: %s\n", archive.generation_version);
        printf("Engine Version: %s\n", archive.engine_version);
        printf("Decompressed Payload Size: %zu bytes\n", archive.payload_size);
        printf("Block Count: %d\n", archive.block_count);
        printf("Directory File Count: %d\n", archive.directory_count);
        
        for (int i = 0; i < archive.directory_count; i++) {
            printf("  File [%d]: Name=\"%s\", Offset=%llu, Size=%llu\n",
                   i, archive.directories[i].name,
                   (unsigned long long)archive.directories[i].offset,
                   (unsigned long long)archive.directories[i].decompressed_size);
        }
        
        // Classify every member before attempting a SerializedFile parse.
        for (int i = 0; i < archive.directory_count; i++) {
            const BundleDirectoryInfo* member = &archive.directories[i];
            if (bundle_member_classify(member) !=
                BUNDLE_MEMBER_SERIALIZED_FILE) {
                continue;
            }
            size_t sub_file_size = 0;
            const uint8_t* sub_file_data = NULL;
            if (!bundle_get_member_view(&archive, (size_t)i,
                                        &sub_file_data, &sub_file_size) ||
                !sub_file_data || sub_file_size == 0U) {
                LOG_ERROR("Failed to read serialized member: %s",
                          member->name ? member->name : "<unnamed>");
                bundle_close(&archive);
                common_file_bytes_dispose(&input);
                return 1;
            }
            SerializedFile file;
            printf("\nParsing serialized member \"%s\"...\n", member->name);
            if (serialized_file_open(&file, sub_file_data, sub_file_size)) {
                dump_serialized_file(&file);
                serialized_file_close(&file);
            } else if (serialized_file_open_metadata(
                           &file, sub_file_data, sub_file_size)) {
                printf("TypeTree schemas are unresolved; showing validated "
                       "metadata and object boundaries only.\n");
                dump_serialized_file(&file);
                serialized_file_close(&file);
            } else {
                printf("Serialized member \"%s\" is invalid or unsupported.\n",
                       member->name);
            }
        }
        
        bundle_close(&archive);
    } else {
        printf("Detected raw assets file. Parsing...\n");
        SerializedFile file;
        if (serialized_file_open(&file, file_data, file_size)) {
            dump_serialized_file(&file);
            serialized_file_close(&file);
        } else if (serialized_file_open_metadata(
                       &file, file_data, file_size)) {
            printf("TypeTree schemas are unresolved; showing validated "
                   "metadata and object boundaries only.\n");
            dump_serialized_file(&file);
            serialized_file_close(&file);
        } else {
            LOG_ERROR("Failed to open SerializedFile");
            common_file_bytes_dispose(&input);
            return 1;
        }
    }
    
    common_file_bytes_dispose(&input);
    
    // Memory leak tracking report
    printf("\nMemory allocations remaining: %zu blocks (%zu bytes)\n", g_allocations_count, g_allocated_bytes);
    if (g_allocations_count > 0 || g_allocated_bytes > 0) {
        printf("[WARN] Memory leaks detected!\n");
    } else {
        printf("[SUCCESS] No memory leaks detected!\n");
    }
    
    return 0;
}
