// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILER_CLIENT_H
#define UNITY_COMPILER_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct UscCacheToolchainLease;

/* Unity 2021.3 initializeCompiler returns one availability mask followed by
 * exactly 25 ordered platform records.  The mask is a signed protocol Int in
 * Unity, but retaining its raw bits avoids losing the seven reserved high
 * bits (which this pinned implementation leaves set). */
#define UNITY_COMPILER_PLATFORM_COUNT 25U
#define UNITY_COMPILER_PLATFORM_MASK UINT32_C(0x01ffffff)

typedef struct {
    uint64_t supported_features;
    int32_t version;
} UnityCompilerPlatformCapability;

typedef struct {
    uint32_t raw_available_platform_mask;
    UnityCompilerPlatformCapability
        platforms[UNITY_COMPILER_PLATFORM_COUNT];
} UnityCompilerSessionCapabilities;

/* Exact caller-supplied initializeCompiler authority.  PENDING deliberately
 * permits process-free persistent-cache use: it means no live compiler has
 * been initialized yet, not that the expected value was accepted.  MATCHED
 * is the only state which permits a live work command when an expectation is
 * configured. */
typedef enum {
    UNITY_COMPILER_VALID_APIS_AUTHORITY_NOT_CONFIGURED = 0,
    UNITY_COMPILER_VALID_APIS_AUTHORITY_PENDING = 1,
    UNITY_COMPILER_VALID_APIS_AUTHORITY_MATCHED = 2,
    UNITY_COMPILER_VALID_APIS_AUTHORITY_MISMATCH = 3,
    UNITY_COMPILER_VALID_APIS_AUTHORITY_SESSION_FAILED = 4,
} UnityCompilerValidApisAuthorityStatus;

typedef struct {
    UnityCompilerValidApisAuthorityStatus status;
    uint32_t expected_valid_apis;
    uint32_t observed_valid_apis;
} UnityCompilerValidApisAuthority;

typedef struct {
    int socket_fd;
    pid_t process_id;
    char* project_root;
    char* includes_dir;
    char* sandbox_includes_dir;
    char* unity_contents_path;
    char* compiler_path;
    char* builtin_includes_dir;
    char* playback_engines_dir;
    char* frameworks_dir;
    char* tools_dir;
    char* glslang_path;
    char* dxcompiler_path;
    char* dynamic_library_path;
    bool has_sandbox_includes;
    bool configured;
    bool cache_compiler_ready;
    bool cache_compiler_failed;
    uint8_t cache_compiler_fingerprint[32];
    bool cache_environment_ready;
    bool cache_environment_failed;
    uint8_t cache_environment_fingerprint[32];
    UnityCompilerSessionCapabilities session_capabilities;
    bool session_capabilities_ready;
    bool session_capabilities_failed;
    uint32_t expected_valid_apis;
    bool expected_valid_apis_ready;
    struct UscCacheToolchainLease* cache_toolchain_lease;
} UnityCompilerChannel;

#define UNITY_COMPILER_FINGERPRINT_SIZE 32

/* Borrowed, fixed-size content authority supplied by a validated offline
 * source such as OraclePack v4.  No local toolchain state is inferred. */
typedef struct {
    const uint8_t* compiler_fingerprint;
    const uint8_t* environment_fingerprint;
} UnityCompilerOfflineAuthority;

/*
 * Exact terminal records emitted by UnityShaderCompiler 2021.3.  The
 * preprocess command has three flags; its first flag is the primary command
 * result.  compileSnippet (including preprocess-only invocations) and
 * disassembleShader each have one flag and distinct record prefixes.
 *
 * These parsers consume the complete record.  Extra whitespace, missing
 * fields, non-binary digits, and trailing tokens are rejected so diagnostics
 * cannot accidentally be interpreted as success.
 */
typedef struct {
    bool primary_success;
    bool secondary_status;
    bool tertiary_status;
} UnityCompilerPreprocessStatus;

/*
 * One lossless ShaderCompilerErrorReportSend callback.  Unity uses the same
 * `err:` record shape for informational notes, warnings, and errors.  Binary
 * analysis of Unity 2021.3.35f1 proves that fields[0] is CgBatchErrorType:
 * zero is a non-actionable information record (for example the preprocess
 * timing note), while positive values are actionable diagnostics.  A live
 * pinned-compiler probe confirms that D3DCompiler warning 3206 (implicit
 * vector truncation) is retained as type one, even though compilation itself
 * succeeds. Unknown negative values fail closed as actionable. The remaining
 * signed fields stay opaque. `record`, `file`, and `message` are owned
 * strings.
 */
typedef struct {
    int32_t fields[3];
    char* record;
    char* file;
    char* message;
} UnityCompilerDiagnostic;

/* A cache-only miss is a complete local lookup result, not a compiler
 * rejection and not a transport failure.  Keeping it in the typed response
 * prevents offline verification from accidentally classifying absence as
 * evidence about UnityShaderCompiler. */
typedef enum {
    UNITY_COMPILER_RESPONSE_AVAILABLE = 0,
    UNITY_COMPILER_RESPONSE_CACHE_ONLY_MISS = 1,
} UnityCompilerResponseAvailability;

/* Terminal compiler status plus every ordered diagnostic that preceded it.
 * A successful terminal status is clean only when it has no actionable
 * diagnostics; type-zero informational records are still retained.
 * Structured callers may inspect the payload and diagnostics, while the
 * compatibility/verifier APIs below fail closed on every actionable record. */
typedef struct {
    UnityCompilerResponseAvailability availability;
    bool compiler_success;
    bool from_cache;
    /* Local authority disposition for this response.  This field is derived
     * from the current channel and is intentionally not persisted inside a
     * compiler-response cache entry. */
    UnityCompilerValidApisAuthority valid_apis_authority;
    UnityCompilerDiagnostic* diagnostics;
    size_t diagnostic_count;
} UnityCompilerResponseStatus;

void unity_compiler_response_status_init(
    UnityCompilerResponseStatus* status);
void unity_compiler_response_status_free(
    UnityCompilerResponseStatus* status);
bool unity_compiler_response_status_copy(
    UnityCompilerResponseStatus* destination,
    const UnityCompilerResponseStatus* source);
bool unity_compiler_diagnostic_is_actionable(
    const UnityCompilerDiagnostic* diagnostic);
size_t unity_compiler_response_status_actionable_diagnostic_count(
    const UnityCompilerResponseStatus* status);
bool unity_compiler_response_status_is_clean_success(
    const UnityCompilerResponseStatus* status);

/* Caller-owned rendering of every diagnostic, or of fallback when there is
 * no diagnostic.  This is the text used by fail-closed compatibility calls. */
char* unity_compiler_response_status_format(
    const UnityCompilerResponseStatus* status, const char* fallback);

bool unity_compiler_parse_preprocess_status_record(
    const char* record, UnityCompilerPreprocessStatus* out_status);
bool unity_compiler_parse_compile_status_record(
    const char* record, bool* out_success);
bool unity_compiler_parse_disassemble_status_record(
    const char* record, bool* out_success);

/* Borrowed paths remain valid until the channel is shut down. */
typedef struct {
    const char* unity_contents_path;
    const char* compiler_path;
    const char* builtin_includes_dir;
    const char* playback_engines_dir;
    const char* glslang_path;
    const char* dxcompiler_path;
    uint8_t compiler_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE];
    uint8_t environment_fingerprint[UNITY_COMPILER_FINGERPRINT_SIZE];
} UnityCompilerToolchainProvenance;

typedef struct {
    char* keyword;
    uint64_t requirements;
} ConditionalShaderRequirement;

#define UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT 4

typedef enum {
    UNITY_KEYWORD_VARIANTS_USER_GLOBAL = 0,
    UNITY_KEYWORD_VARIANTS_USER_LOCAL = 1,
    UNITY_KEYWORD_VARIANTS_BUILTIN = 2,
} UnityKeywordVariantFamily;

/* One ordered `keywordsUserGlobal/UserLocal/Builtin` response record.  Each
 * string is one ordered variant combination exactly as Unity emitted it. */
typedef struct {
    bool present;
    char** combinations;
    int combination_count;
} SnippetKeywordVariantSet;

/* The response sends one family trio per ShaderCompilerProgram.  Program
 * values use Unity's wire enum (vertex=0, fragment=1, hull=2, domain=3,
 * geometry=4, ray tracing=6).  Records retain their response order. */
typedef struct {
    int32_t compiler_program;
    SnippetKeywordVariantSet user_global;
    SnippetKeywordVariantSet user_local;
    SnippetKeywordVariantSet builtin;
} SnippetProgramKeywordVariants;

/*
 * Lossless metadata emitted by Unity 2021.3's preprocess protocol for one
 * `snip:` / `keywordsEnd:` pair.  The `snip:` header contains exactly 13
 * signed decimal 32-bit tokens in this order: the first six scalar fields,
 * four source-hash words, then the final three scalar fields below.
 *
 * The keyword arrays retain the order sent by Unity.  All pointers are owned
 * by the contract and are managed by the init/copy/free helpers.
 */
typedef struct SnippetCompileContract {
    int32_t snippet_id;
    int32_t platforms;
    int32_t quality_variants;
    uint32_t program_types_mask;
    uint32_t compilation_flags;
    int32_t language;
    uint32_t source_hash[UNITY_SNIPPET_SOURCE_HASH_WORD_COUNT];
    int32_t start_line;
    int32_t use_dxc_apis;
    int32_t never_use_dxc_apis;

    SnippetProgramKeywordVariants* program_keyword_variants;
    int program_keyword_variant_count;

    /* Exact keyword-universe metadata returned after `keywordsEnd:`.  These
     * are not the pKW/dKW arrays later sent to compileSnippet. */
    char** non_stripped_user_keywords;
    int non_stripped_user_keyword_count;
    char** builtin_keywords;
    int builtin_keyword_count;
    uint64_t requirements;
    ConditionalShaderRequirement* conditional_requirements;
    int conditional_requirement_count;
} SnippetCompileContract;

void unity_compiler_snippet_contract_init(SnippetCompileContract* contract);
void unity_compiler_snippet_contract_free(SnippetCompileContract* contract);
bool unity_compiler_snippet_contract_copy(
    SnippetCompileContract* destination,
    const SnippetCompileContract* source);
bool unity_compiler_snippet_contract_validate(
    const SnippetCompileContract* contract);

/* Parse the complete `snip:` header.  `contract` must be initialized first. */
bool unity_compiler_snippet_contract_parse_header(
    const char* header,
    SnippetCompileContract* contract);

/* Parse the two space-joined keyword lines sent after `keywordsEnd:`. */
bool unity_compiler_snippet_contract_set_keyword_lines(
    SnippetCompileContract* contract,
    const char* non_stripped_user_keywords,
    const char* builtin_keywords);

/* Copies a complete ordered keyword-family response record. */
bool unity_compiler_snippet_contract_set_variant_combinations(
    SnippetCompileContract* contract,
    int32_t compiler_program,
    UnityKeywordVariantFamily family,
    const char* const* combinations,
    int combination_count);

/* True only when all three family records required to reproduce dKW exist. */
bool unity_compiler_snippet_contract_has_variant_families(
    const SnippetCompileContract* contract,
    int32_t compiler_program);

/* Borrowed stage record, or NULL when that program was not emitted. */
const SnippetProgramKeywordVariants*
unity_compiler_snippet_contract_find_program_variants(
    const SnippetCompileContract* contract,
    int32_t compiler_program);

typedef struct {
    char* source;
    uint64_t reqs;
    int language;
    int gpu_program_id;
    ConditionalShaderRequirement* conditional_requirements;
    int conditional_requirement_count;

    /*
     * New code should use this lossless contract.  The fields above are
     * borrowed compatibility mirrors so existing verifier call sites keep
     * working while they migrate; when has_contract is true, the contract is
     * the sole owner of conditional_requirements.
     */
    SnippetCompileContract contract;
    bool has_contract;
} PreprocessedSnippet;

typedef struct PreprocessResult {
    PreprocessedSnippet* snippets;
    int snippet_count;
    uint8_t* blob;
    size_t blob_len;
} PreprocessResult;

typedef struct UnityCompilerPreprocessResponse {
    UnityCompilerResponseStatus status;
    PreprocessResult result;
} UnityCompilerPreprocessResponse;

void unity_compiler_preprocess_response_init(
    UnityCompilerPreprocessResponse* response);
void unity_compiler_preprocess_response_free(
    UnityCompilerPreprocessResponse* response);

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t corrupt_entries;
    uint64_t write_errors;
} UnityCompilerCacheStats;

/*
 * Configures a compiler channel.  With DXBC_USC_CACHE_DIR set, the subprocess
 * is started lazily on the first cache miss; without a cache this preserves
 * the historical eager-start behavior.  Setting DXBC_USC_CACHE_ONLY=1 makes
 * every cache miss return a typed CACHE_ONLY_MISS without starting a process.
 * Live requests have one absolute
 * monotonic I/O deadline (120 seconds by default, configurable from 1 through
 * 3,600,000 ms with DXBC_USC_IO_TIMEOUT_MS).  An I/O or protocol failure
 * invalidates and reaps the persistent process so the next request starts a
 * clean channel.  Returns true on success.
 */
bool unity_compiler_start(UnityCompilerChannel* channel, const char* project_root, const char* includes_dir);

/* Configures all authority paths and fingerprints without eagerly starting
 * UnityShaderCompiler.  A later live cache miss starts the process lazily. */
bool unity_compiler_start_lazy(
    UnityCompilerChannel* channel, const char* project_root,
    const char* includes_dir);

/* Strict helpers for the typed initializeCompiler response.  `valid_apis` is
 * precisely the low 25 bits of the returned mask; high bits are protocol
 * reserved and are never exposed as APIs.  The subset predicate is useful for
 * diagnostics only. Exact verification must use the equality predicate: a
 * profile that silently omits an available compiler platform is not the
 * session authority Unity supplied to preprocess.
 */
bool unity_compiler_session_capabilities_validate(
    const UnityCompilerSessionCapabilities* capabilities);
bool unity_compiler_session_capabilities_equal(
    const UnityCompilerSessionCapabilities* left,
    const UnityCompilerSessionCapabilities* right);
bool unity_compiler_session_capabilities_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t* out_valid_apis);
bool unity_compiler_session_capabilities_support_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t valid_apis);
bool unity_compiler_session_capabilities_match_valid_apis(
    const UnityCompilerSessionCapabilities* capabilities,
    uint32_t valid_apis);

/* Installs or removes exact validApis authority without starting
 * UnityShaderCompiler.  `set` rejects bits outside the pinned 25-platform
 * mask, but otherwise records the expectation even when an already captured
 * session disagrees; callers can inspect that deterministic disagreement
 * through the query or response status.  The expectation survives compiler
 * process recycling and is cleared only explicitly or by channel shutdown. */
bool unity_compiler_set_expected_valid_apis(
    UnityCompilerChannel* channel, uint32_t expected_valid_apis);
bool unity_compiler_clear_expected_valid_apis(
    UnityCompilerChannel* channel);
bool unity_compiler_expected_valid_apis_authority(
    const UnityCompilerChannel* channel,
    UnityCompilerValidApisAuthority* out_authority);

/* Returns a caller-owned deterministic JSON object containing the raw mask,
 * derived valid_apis, and all 25 ordered feature/version records.  This is a
 * report fragment, not a substitute for retaining the typed record. */
char* unity_compiler_session_capabilities_format_json(
    const UnityCompilerSessionCapabilities* capabilities);

/* Copies a previously captured record without starting a process.  This is
 * the fail-closed offline/cache-only accessor. */
bool unity_compiler_session_capabilities_snapshot(
    const UnityCompilerChannel* channel,
    UnityCompilerSessionCapabilities* out_capabilities);

/* Captures the record from the pinned live compiler when the channel has not
 * yet initialized one.  Repeated calls reuse the same channel record and do
 * not start another process.  DXBC_USC_CACHE_ONLY=1 makes an uncaptured call
 * fail without starting UnityShaderCompiler. */
bool unity_compiler_capture_session_capabilities(
    UnityCompilerChannel* channel,
    UnityCompilerSessionCapabilities* out_capabilities);

/* Orderly stops only the live UnityShaderCompiler child and protocol socket.
 * The validated toolchain paths, fingerprints, and cache authority remain
 * configured, so the next cache miss starts an equivalent process lazily. */
bool unity_compiler_recycle_process(UnityCompilerChannel* channel);

/* Complete preprocess wire authority.  Exact verification must provide these
 * values explicitly rather than deriving build target/API masks from the
 * graphics API selected later for compileSnippet. */
typedef struct {
    const char* source;
    const char* file_path;
    const char* shader_name;
    bool surface_only;
    bool caching_preprocessor;
    uint32_t build_platform;
    uint32_t valid_apis;
    char** keywords;
    int keyword_count;
    char** defines;
    int define_count;
} UnityCompilerShaderPreprocessRequest;

bool unity_compiler_preprocess_contract(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    PreprocessResult* out_result);

/* Returns true when a complete, structurally valid compiler response was
 * received (live or cached), including a compiler-declared failure. */
bool unity_compiler_preprocess_contract_response(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    UnityCompilerPreprocessResponse* out_response);

/* Canonical authority transcript for the complete preprocess request,
 * including resolved include paths and compiler/environment fingerprints.
 * This never starts UnityShaderCompiler. */
bool unity_compiler_serialize_preprocess_request(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

/* Canonical offline form of the request above.  The channel contributes only
 * its configured include-path spellings.  This function does not inspect or
 * hash those paths, acquire a toolchain lease, or start UnityShaderCompiler.
 * Cross-machine equality therefore requires the same configured spellings. */
bool unity_compiler_serialize_preprocess_request_with_authority(
    UnityCompilerChannel* channel,
    const UnityCompilerShaderPreprocessRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

/* Stable typed payload used by preprocess cache and OraclePack replay. */
bool unity_compiler_serialize_preprocess_result(
    const PreprocessResult* result, uint8_t** out_data, size_t* out_size);
bool unity_compiler_deserialize_preprocess_result(
    const uint8_t* data, size_t size, PreprocessResult* out_result);

/* Compatibility wrapper using the historical sandbox verification profile. */
bool unity_compiler_preprocess(UnityCompilerChannel* channel,
                               const char* source,
                               const char* shader_name,
                               PreprocessResult* out_result);

uint64_t unity_compiler_variant_requirements(
    const PreprocessedSnippet* snippet, char** keywords, int keyword_count);

/*
 * Explicit compileSnippet request tied to the exact preprocessing contract.
 * The three source strings match Unity's protocol semantics: directory,
 * basename, and pass name.  The selected keyword arrays describe this compile
 * invocation; the contract separately retains the ordered keyword universe
 * reported by preprocessing.  Array order is significant.
 */
typedef struct {
    const char* snippet_source;
    const char* source_directory;
    const char* source_basename;
    const char* pass_name;
    bool caching_preprocessor;
    bool preprocess_only;
    bool strip_line_directives;
    uint32_t build_platform;
    int32_t render_state_length;
    char** variant_keywords;
    int variant_keyword_count;
    char** user_keywords;
    int user_keyword_count;
    char** disabled_keywords;
    int disabled_keyword_count;
    uint32_t compiler_flags;
    int32_t shader_type;
    int32_t platform;
    uint64_t requirements;
    int32_t program_mask;
    int32_t program_start;
    const SnippetCompileContract* contract;
} UnityCompilerSnippetCompileRequest;

typedef struct UnityCompilerBinaryResponse {
    UnityCompilerResponseStatus status;
    /* Ordered, lossless ShaderCompiler reflection callbacks.  These records
     * are runtime-binding evidence independent of stripped DXBC bytes. */
    struct UnityCompilerReflectionRecord* reflection_records;
    size_t reflection_record_count;
    uint8_t* data;
    size_t size;
} UnityCompilerBinaryResponse;

typedef enum {
    UNITY_COMPILER_REFLECTION_INPUT = 0,
    UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER,
    UNITY_COMPILER_REFLECTION_CONSTANT,
    UNITY_COMPILER_REFLECTION_CONSTANT_BUFFER_BINDING,
    UNITY_COMPILER_REFLECTION_TEXTURE_BINDING,
    UNITY_COMPILER_REFLECTION_SAMPLER,
    UNITY_COMPILER_REFLECTION_BUFFER_BINDING,
    UNITY_COMPILER_REFLECTION_UAV_BINDING,
    UNITY_COMPILER_REFLECTION_STATS,
} UnityCompilerReflectionKind;

#define UNITY_COMPILER_REFLECTION_MAX_VALUES 6U

typedef struct UnityCompilerReflectionRecord {
    UnityCompilerReflectionKind kind;
    char* record;
    char* name;
    int32_t values[UNITY_COMPILER_REFLECTION_MAX_VALUES];
    size_t value_count;
} UnityCompilerReflectionRecord;

void unity_compiler_reflection_record_free(
    UnityCompilerReflectionRecord* record);

/* Parses one complete canonical callback line. The output owns its strings
 * and must be freed even after a successful parse. */
bool unity_compiler_reflection_record_parse(
    const char* text, UnityCompilerReflectionRecord* out_record);

bool unity_compiler_reflection_records_equal(
    const UnityCompilerReflectionRecord* left, size_t left_count,
    const UnityCompilerReflectionRecord* right, size_t right_count);

void unity_compiler_binary_response_init(
    UnityCompilerBinaryResponse* response);
void unity_compiler_binary_response_free(
    UnityCompilerBinaryResponse* response);

typedef struct {
    UnityCompilerResponseStatus status;
    char* text;
    size_t size;
} UnityCompilerTextResponse;

void unity_compiler_text_response_init(
    UnityCompilerTextResponse* response);
void unity_compiler_text_response_free(
    UnityCompilerTextResponse* response);

uint8_t* unity_compiler_compile_contract(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    size_t* out_size,
    char** out_error);

/* Diagnostic-preserving compile surface.  Returns true for any complete,
 * valid terminal response.  `status.compiler_success` distinguishes compiler
 * rejection; diagnostics remain available even when Unity returned bytes. */
bool unity_compiler_compile_contract_response(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    UnityCompilerBinaryResponse* out_response);

/*
 * Returns the resolved toolchain and content fingerprints without starting
 * UnityShaderCompiler.  Resolution is controlled by, in descending priority:
 * DXBC_UNITY_CONTENTS_PATH, DXBC_UNITY_APP, UNITY_EDITOR_PATH, then the
 * historical /Applications/Unity/Unity.app location.  Individual artifact
 * paths may be overridden with DXBC_UNITY_COMPILER_PATH,
 * DXBC_UNITY_BUILTIN_INCLUDES_PATH, DXBC_UNITY_PLAYBACK_ENGINES_PATH,
 * DXBC_UNITY_GLSLANG_PATH, and DXBC_UNITY_DXCOMPILER_PATH.
 */
bool unity_compiler_get_toolchain_provenance(
    UnityCompilerChannel* channel,
    UnityCompilerToolchainProvenance* out_provenance);

/*
 * Serializes the exact, length-delimited compileSnippet authority input used
 * by the persistent cache.  The returned transcript is owned by the caller.
 * Its SHA-256 is returned separately for VariantKey/oracle-pack identity.
 */
bool unity_compiler_serialize_compile_request(
    UnityCompilerChannel* channel,
    const UnityCompilerSnippetCompileRequest* request,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

/* Canonical offline compile identity.  Only the supplied fingerprints and
 * typed request are consumed; no Unity path is opened, hashed, or launched. */
bool unity_compiler_serialize_compile_request_with_authority(
    const UnityCompilerSnippetCompileRequest* request,
    const UnityCompilerOfflineAuthority* authority,
    uint8_t** out_transcript,
    size_t* out_transcript_size,
    uint8_t out_request_digest[UNITY_COMPILER_FINGERPRINT_SIZE]);

// Compiles a single snippet to binary bytecode. Returns allocated buffer on success.
uint8_t* unity_compiler_compile(
    UnityCompilerChannel* channel,
    const char* snippet_src,
    const char* shader_name,
    int shader_type, // 0 = vertex, 1 = fragment
    int platform,    // 15 = GLCore
    uint64_t reqs,
    char** keywords,
    int keyword_count,
    char** defines,
    int define_count,
    size_t* out_size,
    char** out_error
);

/*
 * Set DXBC_USC_CACHE_DIR to opt into the persistent compileSnippet cache.
 * Keys cover the complete ordered request, the UnityShaderCompiler executable,
 * and all resolved include/plugin/toolchain inputs that can affect output.
 * Failures are never cached.
 */
void unity_compiler_cache_get_stats(UnityCompilerCacheStats* out_stats);
void unity_compiler_cache_reset_stats(void);

// Preprocesses a snippet and returns the full expanded source (with all includes expanded)
char* unity_compiler_preprocess_expanded(
    UnityCompilerChannel* channel,
    const char* snippet_src,
    const char* shader_name,
    int shader_type,
    int platform,
    uint64_t reqs,
    char** keywords,
    int keyword_count,
    char** defines,
    int define_count
);

// Disassembles a compiled binary bytecode to text. Returns allocated string on success.
char* unity_compiler_disassemble(
    UnityCompilerChannel* channel,
    const char* shader_name,
    int platform,    // 4 = d3d11, 15 = glcore
    int stage,       // 0 = vertex, 1 = fragment
    const uint8_t* bytecode,
    size_t size
);

/* Diagnostic-preserving disassembly surface.  The returned text owns
 * `size` payload bytes plus a convenience NUL terminator.  Returns true for
 * any complete terminal response, including compiler rejection or a success
 * accompanied by diagnostics.  Disassembly responses are never cached. */
bool unity_compiler_disassemble_response(
    UnityCompilerChannel* channel,
    const char* shader_name,
    int platform,
    int stage,
    const uint8_t* bytecode,
    size_t size,
    UnityCompilerTextResponse* out_response);

// Frees a preprocess result.
void unity_compiler_free_preprocess(PreprocessResult* result);

// Shuts down compiler process and closes connection.
void unity_compiler_shutdown(UnityCompilerChannel* channel);

#endif // UNITY_COMPILER_CLIENT_H
