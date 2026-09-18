#ifndef TEST_HLSL_COMPILE_VERIFY_H
#define TEST_HLSL_COMPILE_VERIFY_H

#include <windows.h>
#include <stdbool.h>
#include "io/subprogram_metadata.h"

// ID3DBlob COM definition
typedef struct ID3DBlob ID3DBlob;
typedef struct ID3DBlobVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3DBlob* This, void* riid, void** ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3DBlob* This);
    ULONG (STDMETHODCALLTYPE *Release)(ID3DBlob* This);
    LPVOID (STDMETHODCALLTYPE *GetBufferPointer)(ID3DBlob* This);
    SIZE_T (STDMETHODCALLTYPE *GetBufferSize)(ID3DBlob* This);
} ID3DBlobVtbl;

struct ID3DBlob {
    const ID3DBlobVtbl* lpVtbl;
};

typedef HRESULT (WINAPI *pfn_D3DCompile)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const void* pDefines,
    void* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs
);

typedef HRESULT (WINAPI *pfn_D3DDisassemble)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    UINT Flags,
    LPCSTR szComments,
    ID3DBlob** ppDisassembly
);

extern pfn_D3DCompile g_D3DCompile;
extern pfn_D3DDisassemble g_D3DDisassemble;

extern int g_shaders_total;
extern int g_shaders_passed;
extern int g_shaders_matched;

bool compare_disassembly(const char* orig_asm, const char* gen_asm, long long path_id, int stage, int sub_idx);
bool verify_subprogram_compilation(
    const PlayerSubProgramMetadata* sub_meta,
    const SerializedProgramParameters* params,
    long long path_id,
    int stage,
    int sub_idx
);

#endif // TEST_HLSL_COMPILE_VERIFY_H
