// probe.cpp —— 验证: 加载器解析 GDI32!SwapBuffers 导入时, IAT 指向
// 导出存根 (FF 25) 还是真实函数体 (绕过存根)。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void* GetIatEntry(HMODULE mod, const char* dllName, const char* funcName)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress) return NULL;
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress);
    for (; d->Name; ++d) {
        const char* name = (const char*)(base + d->Name);
        if (_stricmp(name, dllName) != 0) continue;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (int i = 0; oft[i].u1.AddressOfData; ++i) {
            if (oft[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft[i].u1.AddressOfData);
            if (strcmp((char*)ibn->Name, funcName) == 0)
                return (void*)ft[i].u1.Function;
        }
        return NULL;
    }
    return NULL;
}

int main()
{
    HMODULE stubDll = LoadLibraryA("test\\swapclient\\native\\swapstub.dll");
    printf("swapstub=%p loaded=%d err=%lu\n", (void*)stubDll, stubDll != NULL, GetLastError());
    if (stubDll) {
        void* iat = GetIatEntry(stubDll, "GDI32.dll", "SwapBuffers");
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        BYTE* stub = (BYTE*)(void*)GetProcAddress(gdi, "SwapBuffers");
        INT32 rel = 0;
        memcpy(&rel, stub + 2, 4);
        void* slot = *(void**)(stub + 6 + rel);
        printf("stub=%p bytes=%02X%02X slot=%p\n", (void*)stub, stub[0], stub[1], slot);
        printf("IAT=%p  ==stub?%d ==slot?%d\n", iat, iat == stub, iat == slot);
    }
    return 0;
}
