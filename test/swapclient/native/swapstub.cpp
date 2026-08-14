//============================================================================
//  swapstub.dll —— 假客户端测试辅助库 (V65 hook 版冒烟测试用)
//
//  作用: 假客户端的 Java 线程 ("Client thread", 模拟游戏渲染线程) 每 10ms
//  调用 Java_TestSwapClient_swap, 本函数内调用真实的 gdi32!SwapBuffers ——
//  被注入的 MCCombatStatusJni.dll 钩住的就是这个函数, 从而触发完整的
//  "渲染帧内 GetEnv 复用 JNIEnv" 链路 (不新建线程、不 Attach)。
//
//  编译 (build.bat 已包含):
//    g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s \
//        -I..\..\..\include -I..\..\..\include\win32 \
//        -o swapstub.dll swapstub.cpp -lgdi32
//============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <jni.h>
#include <stdio.h>

// 崩溃记录器: 定位注入/APC 期间的未处理异常 (写入 user.dir 的 swapstub_crash.txt)
static LONG WINAPI CrashLog(PEXCEPTION_POINTERS ep)
{
    FILE* f = fopen("swapstub_crash.txt", "a");
    if (f) {
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQuery(ep->ExceptionRecord->ExceptionAddress, &mbi, sizeof(mbi));
        fprintf(f, "code=%08X at=%p page=%p prot=0x%lX rip=%p tid=%lu faultAddr=%p rax=%p r13=%p rbx=%p\n",
                ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                mbi.BaseAddress, mbi.Protect,
                (void*)ep->ContextRecord->Rip,
                GetCurrentThreadId(),
                (void*)ep->ExceptionRecord->ExceptionInformation[1],
                (void*)ep->ContextRecord->Rax,
                (void*)ep->ContextRecord->R13,
                (void*)ep->ContextRecord->Rbx);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 注意: 必须 extern "C" —— 否则 C++ 名字修饰导致 JVM 按符号名
// Java_TestSwapClient_swap 查找失败 (UnsatisfiedLinkError)。
extern "C" {
JNIEXPORT void JNICALL Java_TestSwapClient_swap(JNIEnv* env, jclass cls)
{
    (void)env;
    (void)cls;
    static bool first = true;
    static long calls = 0;
    if (first) { AddVectoredExceptionHandler(1, CrashLog); first = false; }
    calls++;
    // 调用计数日志 (冒烟测试诊断): 前 3 次 + 每 1000 次
    if (calls <= 3 || calls % 1000 == 0) {
        char path[MAX_PATH];
        if (GetTempPathA(MAX_PATH, path)) {
            strncat(path, "swapstub_tick.log", MAX_PATH - strlen(path) - 1);
            FILE* f = fopen(path, "ab");
            if (f) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                fprintf(f, "[%02d:%02d:%02d.%03d] swap() call #%ld tid=%lu\n",
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                        calls, GetCurrentThreadId());
                fclose(f);
            }
        }
    }
    // 控制台窗口 DC 不是双缓冲 GL 表面, SwapBuffers 会返回 FALSE ——
    // 这无关紧要: 钩子链在进入真实函数前就已触发, 我们只借这条调用路径。
    HWND w = GetConsoleWindow();
    HDC hdc = GetDC(w);
    SwapBuffers(hdc);
    if (hdc && w) ReleaseDC(w, hdc);
}
} // extern "C"
