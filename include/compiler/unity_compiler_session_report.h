// SPDX-License-Identifier: GPL-3.0-only

#ifndef UNITY_COMPILER_SESSION_REPORT_H
#define UNITY_COMPILER_SESSION_REPORT_H

#include "compiler/unity_compiler_client.h"

/*
 * One immutable, caller-owned view of a live initializeCompiler capture.
 * Path pointers are borrowed for the duration of formatting.  The two
 * fingerprints and the 25 platform records are copied values.
 */
typedef struct {
    const char* project_root;
    const char* includes_dir;
    UnityCompilerToolchainProvenance toolchain;
    UnityCompilerSessionCapabilities session;
} UnityCompilerSessionReport;

bool unity_compiler_session_report_validate(
    const UnityCompilerSessionReport* report);

/* Caller-owned, deterministic renderings.  JSON always ends in one LF and
 * uses canonical field order, lowercase SHA-256, and \u00xx escapes for
 * control bytes. */
char* unity_compiler_session_report_format_json(
    const UnityCompilerSessionReport* report);
char* unity_compiler_session_report_format_table(
    const UnityCompilerSessionReport* report);

#endif /* UNITY_COMPILER_SESSION_REPORT_H */
