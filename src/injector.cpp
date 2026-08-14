//============================================================================
//  injector.exe
//  把 MCInGame.dll 注入到 Minecraft (java/javaw) 进程。
//
//  DLL 注入后一切都在游戏内进行: 按 Insert 呼出菜单, 菜单里开关连点、
//  调 CPS、开关闸门; 设置自动保存, 无需任何外部程序。
//
//  用法:
//    injector.exe                      自动查找 Minecraft 窗口并注入
//    injector.exe -pid <PID>           注入到指定 PID
//    injector.exe -title <子串>        按窗口标题子串查找 (默认 "Minecraft")
//    injector.exe -dll <路径>          指定 DLL 文件
//
//  注意: injector.exe 与 DLL 位数必须和游戏 Java 一致 (默认 64 位)。
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* kDllName   = "MCInGame.dll";

static const char* g_titleSub = "Minecraft";
static char        g_dllPath[MAX_PATH] = ""; // 非空时使用指定 DLL 路径

//--------------------------------------------------------------------------
// 进程名判断 (不区分大小写)
//--------------------------------------------------------------------------
static bool NameIs(const char* name, const char* target)
{
    return _stricmp(name, target) == 0;
}

static bool IsJavaProcess(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    bool isJava = false;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid &&
                (NameIs(pe.szExeFile, "java.exe") || NameIs(pe.szExeFile, "javaw.exe"))) {
                isJava = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return isJava;
}

//--------------------------------------------------------------------------
// 按窗口标题查找 Minecraft 进程
//--------------------------------------------------------------------------
struct FindCtx { const char* sub; DWORD pid; };

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp)
{
    FindCtx* ctx = (FindCtx*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    if (GetWindowTextA(hwnd, title, sizeof(title)) == 0) return TRUE;
    if (!strstr(title, ctx->sub)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && IsJavaProcess(pid)) {
        ctx->pid = pid;
        return FALSE; // 找到
    }
    return TRUE;
}

static DWORD FindMinecraftPid(void)
{
    FindCtx ctx = { g_titleSub, 0 };
    EnumWindows(EnumWinProc, (LPARAM)&ctx);
    return ctx.pid;
}

//--------------------------------------------------------------------------
// 注入 DLL (CreateRemoteThread + LoadLibraryA)
//--------------------------------------------------------------------------
static bool InjectDll(DWORD pid, const char* dllPath)
{
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("[!] OpenProcess(%lu) 失败, 错误码 %lu (试试以管理员身份运行)\n",
               pid, GetLastError());
        return false;
    }

    // 位数检查
    BOOL isWow64 = FALSE;
    if (sizeof(void*) == 8 && IsWow64Process(proc, &isWow64) && isWow64) {
        printf("[!] 目标进程是 32 位, 而本工具是 64 位, 无法注入。\n");
        printf("    请使用 32 位版本 (gcc -m32 重新编译) 或改用 64 位 Java。\n");
        CloseHandle(proc);
        return false;
    }

    size_t len = strlen(dllPath) + 1;
    void* mem = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        printf("[!] VirtualAllocEx 失败, 错误码 %lu\n", GetLastError());
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, mem, dllPath, len, NULL)) {
        printf("[!] WriteProcessMemory 失败, 错误码 %lu\n", GetLastError());
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(proc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)loadLib, mem, 0, NULL);
    if (!thread) {
        printf("[!] CreateRemoteThread 失败, 错误码 %lu\n", GetLastError());
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return true;
}

//--------------------------------------------------------------------------
// 主流程
//--------------------------------------------------------------------------
int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8); // 中文输出

    DWORD pid = 0;
    bool g_openMenu = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-pid") == 0 && i + 1 < argc) {
            pid = (DWORD)strtoul(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "-title") == 0 && i + 1 < argc) {
            g_titleSub = argv[++i];
        }
        else if (strcmp(argv[i], "-dll") == 0 && i + 1 < argc) {
            snprintf(g_dllPath, sizeof(g_dllPath), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "-menu") == 0) {
            g_openMenu = true;   // 注入后自动打开游戏内菜单 (调试/测试)
        }
        else {
            printf("用法: injector.exe [-pid <PID>] [-title <窗口标题子串>] [-dll <DLL路径>] [-menu]\n");
            return 1;
        }
    }

    // 定位游戏进程
    if (pid == 0) {
        printf("[*] 正在查找窗口标题包含 \"%s\" 的 Java 进程...\n", g_titleSub);
        pid = FindMinecraftPid();
        if (pid == 0) {
            printf("[!] 未找到 Minecraft 进程。\n");
            printf("    请先启动游戏, 或手动指定: injector.exe -pid <PID>\n");
            printf("    可用命令查看: tasklist | findstr /i \"java\"\n");
            return 1;
        }
        printf("[*] 找到 Minecraft 进程, PID = %lu\n", pid);
    } else {
        if (!IsJavaProcess(pid)) {
            printf("[!] 警告: PID %lu 不是 java/javaw 进程, 仍将尝试注入。\n", pid);
        }
    }

    // 计算 DLL 路径 (与 injector.exe 同目录; 或 -dll 指定)
    char dllPath[MAX_PATH];
    if (g_dllPath[0]) {
        snprintf(dllPath, sizeof(dllPath), "%s", g_dllPath);
    } else {
        char exePath[MAX_PATH];
        DWORD got = GetModuleFileNameA(NULL, exePath, MAX_PATH);
        if (got == 0 || got >= MAX_PATH) {
            printf("[!] 无法获取自身路径\n");
            return 1;
        }
        char* slash = strrchr(exePath, '\\');
        if (slash) *(slash + 1) = 0;
        snprintf(dllPath, sizeof(dllPath), "%s%s", exePath, kDllName);
    }

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[!] 找不到 %s (应与 injector.exe 在同一目录)\n", dllPath);
        return 1;
    }

    // 注入
    // -menu: 若 DLL 已注入过 (事件已存在), 只发打开菜单命令, 绝不重复注入
    //        (重复 LoadLibrary 会加载第二份副本, 且副本留在模块列表里)
    if (g_openMenu) {
        HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\MCInGameMenuEvent");
        if (ev) {
            SetEvent(ev);
            CloseHandle(ev);
            printf("[+] 已发送打开菜单命令 (进程此前已注入)。\n");
            return 0;
        }
    }

    printf("[*] 注入 %s ...\n", dllPath);
    if (!InjectDll(pid, dllPath)) return 1;
    printf("[+] 注入完成。\n");
    if (g_openMenu) {
        HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\MCInGameMenuEvent");
        if (ev) {
            SetEvent(ev);
            CloseHandle(ev);
            printf("[+] 已发送打开菜单命令。\n");
        } else {
            printf("[!] 打开菜单命令发送失败。\n");
        }
    }
    printf("    游戏中按 Insert 呼出菜单 (↑↓ 选择 / ←→ 回车 调整 / Esc 关闭)\n");
    printf("    设置: %%APPDATA%%\\MCInGameClicker\\settings.ini\n");
    printf("    日志: %%TEMP%%\\MCInGameClicker.log\n");
    return 0;
}
