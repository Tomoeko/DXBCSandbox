#include "d3dcompiler_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

bool g_enable_hook_logging = false;

typedef struct {
    BYTE original_bytes[14];
    LPVOID target_addr;
} Hook;

static Hook hook_AddElement;
static Hook hook_GetRegisterInfo;
static Hook hook_CompactRegisters;
static Hook hook_SplitRegisters;
static Hook hook_SwizzleRegisters;

// x64 absolute jump: jmp qword ptr [rip + 0] followed by 64-bit address
static void install_hook(Hook* h, LPVOID target, LPVOID dest) {
    h->target_addr = target;
    SIZE_T read_bytes;
    ReadProcessMemory(GetCurrentProcess(), target, h->original_bytes, 14, &read_bytes);
    
    BYTE patch[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    uintptr_t addr = (uintptr_t)dest;
    memcpy(patch + 6, &addr, 8);
    
    DWORD old_protect;
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old_protect);
    memcpy(target, patch, 14);
    VirtualProtect(target, 14, old_protect, &old_protect);
    
    FlushInstructionCache(GetCurrentProcess(), target, 14);
}

static void remove_hook(Hook* h) {
    DWORD old_protect;
    VirtualProtect(h->target_addr, 14, PAGE_EXECUTE_READWRITE, &old_protect);
    memcpy(h->target_addr, h->original_bytes, 14);
    VirtualProtect(h->target_addr, 14, old_protect, &old_protect);
    
    FlushInstructionCache(GetCurrentProcess(), h->target_addr, 14);
}

// Replicated RegisterHash
unsigned int register_hash(const char* name) {
    if (!name || !*name) return 0;
    unsigned int v1 = 0;
    const char* p = name;
    while (*p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A'; // toupper
        v1 = 19 * v1 + c;
        p++;
    }
    return v1 % 7;
}

typedef __int64 (__fastcall *pfn_AddElement)(void* self, const char* name, int register_index, int reg_count);

__int64 __fastcall hook_fn_AddElement(void* self, const char* name, int register_index, int reg_count) {
    remove_hook(&hook_AddElement);
    
    pfn_AddElement orig = (pfn_AddElement)hook_AddElement.target_addr;
    __int64 res = orig(self, name, register_index, reg_count);
    
    if (g_enable_hook_logging) {
        printf("    [HOOK AddElement] allocator=%p, name=%s, hash=%u, index=%d, count=%d\n", 
               self, name, register_hash(name), register_index, reg_count);
    }
    
    install_hook(&hook_AddElement, hook_AddElement.target_addr, hook_fn_AddElement);
    return res;
}

typedef int (__fastcall *pfn_GetRegisterInfo)(
    void* self,
    void* a2,
    void* a3,
    void* a4,
    const char* name,
    void* a6,
    unsigned int* pType,
    unsigned int* a8,
    unsigned int* pIndex
);

int __fastcall hook_fn_GetRegisterInfo(
    void* self,
    void* a2,
    void* a3,
    void* a4,
    const char* name,
    void* a6,
    unsigned int* pType,
    unsigned int* a8,
    unsigned int* pIndex
) {
    remove_hook(&hook_GetRegisterInfo);
    
    pfn_GetRegisterInfo orig = (pfn_GetRegisterInfo)hook_GetRegisterInfo.target_addr;
    int res = orig(self, a2, a3, a4, name, a6, pType, a8, pIndex);
    
    if (g_enable_hook_logging) {
        printf("    [HOOK GetRegisterInfo] self=%p, name=%s, type=%u, index=%u, res=%d\n", 
               self, name, pType ? *pType : 0, pIndex ? *pIndex : 0, res);
    }
    
    install_hook(&hook_GetRegisterInfo, hook_GetRegisterInfo.target_addr, hook_fn_GetRegisterInfo);
    return res;
}

// CProgram structure definitions and logging logic
typedef struct CPool_ {
    char padding1[8];
    const char *name; // offset 8
} CPool_;

typedef struct CArgument_ {
    char padding1[48];
    CPool_ *pool; // offset 48
    char padding2[208 - 56];
    CPool_ *pool2; // offset 208
    char padding3[240 - 216];
    unsigned int register_index; // offset 240
    unsigned int component_index; // offset 244
} CArgument_;

typedef struct CInstruction_ {
    struct CInstruction_ *next; // offset 0
    struct CInstruction_ *prev; // offset 8
    char padding1[32 - 16];
    unsigned int argument_count; // offset 32
    CArgument_ **arguments; // offset 40
    char padding2[136 - 48];
    unsigned int opcode; // offset 136
} CInstruction_;

typedef struct CProgram_ {
    char padding1[24];
    CInstruction_ *instructions; // offset 24
} CProgram_;

static void dump_program(void* self, const char* label) {
    if (!g_enable_hook_logging) return;
    CProgram_* prog = (CProgram_*)self;
    CInstruction_* inst = prog->instructions;
    printf("[HOOK %s] pProgram=%p\n", label, self);
    int idx = 0;
    while (inst) {
        printf("  Inst %d: opcode=%u, args=%u\n", idx++, inst->opcode, inst->argument_count);
        for (unsigned int a = 0; a < inst->argument_count; a++) {
            if (inst->arguments) {
                CArgument_* arg = inst->arguments[a];
                if (arg) {
                    CPool_* pool = arg->pool;
                    if (!pool && arg->pool2) pool = arg->pool2;
                    const char* pool_name = pool ? pool->name : "NULL";
                    printf("    Arg %u: pool=%s, reg=%u, comp=%u\n", a, pool_name, arg->register_index, arg->component_index);
                } else {
                    printf("    Arg %u: NULL\n", a);
                }
            } else {
                printf("    Arg %u: (No arguments array)\n", a);
            }
        }
        inst = inst->next;
    }
}

typedef __int64 (__fastcall *pfn_CompactRegisters)(void* self);
typedef __int64 (__fastcall *pfn_SplitRegisters)(void* self, bool a2);
typedef __int64 (__fastcall *pfn_SwizzleRegisters)(void* self);

__int64 __fastcall hook_fn_CompactRegisters(void* self) {
    remove_hook(&hook_CompactRegisters);
    dump_program(self, "CompactRegisters BEFORE");
    pfn_CompactRegisters orig = (pfn_CompactRegisters)hook_CompactRegisters.target_addr;
    __int64 res = orig(self);
    dump_program(self, "CompactRegisters AFTER");
    install_hook(&hook_CompactRegisters, hook_CompactRegisters.target_addr, hook_fn_CompactRegisters);
    return res;
}

__int64 __fastcall hook_fn_SplitRegisters(void* self, bool a2) {
    remove_hook(&hook_SplitRegisters);
    if (g_enable_hook_logging) {
        printf("[HOOK SplitRegisters BEFORE] self=%p, a2=%d\n", self, a2);
    }
    pfn_SplitRegisters orig = (pfn_SplitRegisters)hook_SplitRegisters.target_addr;
    __int64 res = orig(self, a2);
    if (g_enable_hook_logging) {
        printf("[HOOK SplitRegisters AFTER] self=%p, res=%lld\n", self, res);
    }
    install_hook(&hook_SplitRegisters, hook_SplitRegisters.target_addr, hook_fn_SplitRegisters);
    return res;
}

__int64 __fastcall hook_fn_SwizzleRegisters(void* self) {
    remove_hook(&hook_SwizzleRegisters);
    if (g_enable_hook_logging) {
        printf("[HOOK SwizzleRegisters BEFORE] self=%p\n", self);
    }
    pfn_SwizzleRegisters orig = (pfn_SwizzleRegisters)hook_SwizzleRegisters.target_addr;
    __int64 res = orig(self);
    if (g_enable_hook_logging) {
        printf("[HOOK SwizzleRegisters AFTER] self=%p, res=%lld\n", self, res);
    }
    install_hook(&hook_SwizzleRegisters, hook_SwizzleRegisters.target_addr, hook_fn_SwizzleRegisters);
    return res;
}

bool init_d3dcompiler_hooks(HMODULE mod) {
    uintptr_t base_addr = (uintptr_t)mod;
    // Offset for CFragmentRegisterAllocator::AddElement is 0x1240a0
    LPVOID target = (LPVOID)(base_addr + 0x1240a0);
    install_hook(&hook_AddElement, target, hook_fn_AddElement);
    
    // Offset for CFragmentInfo::GetRegisterInfo is 0x1245d0
    LPVOID target_gri = (LPVOID)(base_addr + 0x1245d0);
    install_hook(&hook_GetRegisterInfo, target_gri, hook_fn_GetRegisterInfo);
    
    // Offset for CProgram::CompactRegisters is 0xb8e90
    LPVOID target_cr = (LPVOID)(base_addr + 0xb8e90);
    install_hook(&hook_CompactRegisters, target_cr, hook_fn_CompactRegisters);

    // Offset for CProgram::SplitRegisters is 0xb7aa0
    LPVOID target_sr = (LPVOID)(base_addr + 0xb7aa0);
    install_hook(&hook_SplitRegisters, target_sr, hook_fn_SplitRegisters);

    // Offset for CProgram::SwizzleRegisters is 0xba3b0
    LPVOID target_szr = (LPVOID)(base_addr + 0xba3b0);
    install_hook(&hook_SwizzleRegisters, target_szr, hook_fn_SwizzleRegisters);

    printf("[HOOK] Successfully installed hooks in D3DCompiler_47.dll at %p, %p, %p, %p, %p\n", 
           target, target_gri, target_cr, target_sr, target_szr);
    return true;
}

void shutdown_d3dcompiler_hooks(void) {
    remove_hook(&hook_AddElement);
    remove_hook(&hook_GetRegisterInfo);
    remove_hook(&hook_CompactRegisters);
    remove_hook(&hook_SplitRegisters);
    remove_hook(&hook_SwizzleRegisters);
    printf("[HOOK] Successfully removed hooks.\n");
}
