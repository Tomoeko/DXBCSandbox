#ifndef D3DCOMPILER_HOOK_H
#define D3DCOMPILER_HOOK_H

#include <windows.h>
#include <stdbool.h>

bool init_d3dcompiler_hooks(HMODULE mod);
void shutdown_d3dcompiler_hooks(void);

extern bool g_enable_hook_logging;

#endif // D3DCOMPILER_HOOK_H
