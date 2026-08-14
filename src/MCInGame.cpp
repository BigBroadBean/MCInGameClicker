//============================================================================
//  MCInGame.dll  (v1.0)
//  注入到 Minecraft (java/javaw) 进程中的游戏内连点器。
//
//  与旧项目 (MCCombatStatusJni) 的区别 (按需求对齐参考实现):
//    * 不再通过共享内存/UDP 对外发布状态 —— 交互全部发生在游戏内:
//      按 Insert 呼出游戏内菜单 (GDI 叠加层), 菜单里直接开关连点、
//      调 CPS、开关闸门, 设置自动保存到 %APPDATA%\MCInGameClicker\settings.ini。
//    * 连点逻辑也移入游戏进程: 渲染帧内按时间累加器精确发 click
//      (PostMessage WM_LBUTTONDOWN/UP, 与外部版 AutoClicker v2.9 同协议),
//      不再需要任何外部程序。
//    * 删除滚轮点 (scroll-to-click) 与多倍点 (multi-click)。
//    * 保留: 左/右键独立开关与 CPS、保持连点(无需按住)、三个闸门
//      (能攻击才点 / 能放置才点 / 视角内才点-光标隐藏)。
//
//  架构 (与参考实现一致, V65.1 对齐版):
//    * 不创建任何线程、不 AttachCurrentThread、不创建任何 socket。
//    * 内联钩住 gdi32!SwapBuffers: 覆盖 12 字节导出存根为绝对跳转
//      (mov rax,imm64; jmp rax), 真实函数从存根槽位读出。
//    * 钩子内 GetEnv() 复用渲染线程已有的 JNIEnv 解析 MC 状态
//      (能攻击/手持放置物/是否在游戏内), 全部工作在渲染帧内分帧完成。
//    * DllMain 内把本 DLL 从 PEB 模块三链表摘除 (模块枚举不可见)。
//
//  版本适配: mc_maps_generated.h (171 张映射表, 1.8.9~1.21.x,
//  vanilla/forge/mojang/intermediary), 自动探测运行环境并逐表尝试。
//============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <jni.h>
#include <jvmti.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

//--------------------------------------------------------------------------
// 内部状态结构 (不再跨进程发布; 仅本进程内使用, 菜单/连点逻辑读取)
//--------------------------------------------------------------------------
#pragma pack(push, 1)
struct Status {
    DWORD        magic;            // 0x4D434354 = 'MCST'
    DWORD        version;          // 7
    volatile LONG ready;           // JNI 解析是否成功
    volatile LONG inGame;          // 是否已进入游戏 (mc && player != null)
    volatile LONG canAttack;       // 当前是否能攻击
    volatile LONG canPlace;        // 手持物品是否为放置物 (ItemBlock/BlockItem)
    volatile LONG placeReady;      // 放置物判定链是否解析成功 (0=未启用, canPlace 恒 0)
    volatile LONG hitType;         // 0=未命中 1=命中方块 2=命中实体
    volatile LONG targetLiving;    // 目标是否为 LivingEntity
    volatile LONG targetAlive;     // 目标是否存活
    volatile LONG targetIsPlayer;  // 目标是否是玩家自己
    volatile LONG heldItemNull;    // 1 = 手持为空 (null), 0 = 手持有物品
    char          targetName[128]; // 目标的类名 (如 EntityZombie / pr / bfj)
    char          heldItemName[64];// 手持物品的 Item 类名 (如 ItemBlock / yo / cds)
    char          mappingName[32]; // 命中的命名体系 (如 forge1201)
    char          envName[48];     // 环境探测结果 (forge/optifine/fabric/launchwrapper)
    char          loaderName[48];  // 使用的游戏类加载器类名
    char          errMsg[96];      // 最近一次错误详情 (类名/异常信息)
    char          failLog[160];    // 最近一轮 10 套映射的失败原因汇总
    volatile LONG mcNull;          // 1 = getMinecraft() 返回 null (双份类副本问题)
    volatile LONG tick;            // 更新计数
    volatile LONG lastError;       // 最近一次错误码, 0=无错误
};
#pragma pack(pop)

static const DWORD kMagic   = 0x4D435354; // 'MCST'
static const DWORD kVersion = 7;

static Status  g_st;
#define g_status (&g_st)

static char g_gameVer[32];   // 从 classpath 提取的 MC 版本号 (菜单显示)

static void CopyName(char* dst, size_t cap, const char* src); // 前向声明
static jclass FindLoadedGameClass(JNIEnv* env, jobject loader, const char* name,
                                  jclass clsCls, jmethodID forName,
                                  const char* getterName, const char* getterSig); // 前向声明

//--------------------------------------------------------------------------
// 日志: %TEMP%\MCInGameClicker.log (仅关键事件; 点击日志需 debugLogClicks=1)
//--------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return;
    size_t n = strlen(path);
    if (n + 24 >= MAX_PATH) return;
    strcpy(path + n, "MCInGameClicker.log");
    FILE* f = fopen(path, "ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

//--------------------------------------------------------------------------
// 设置 (INI) —— %APPDATA%\MCInGameClicker\settings.ini
//--------------------------------------------------------------------------
static const int kCpsMin = 5;
static const int kCpsMax = 100;

struct Settings {
    int master;      // 总开关 (连点)
    int left;        // 左键连点
    int right;       // 右键连点
    int cpsLeft;     // 左键 CPS (5~100, 连续)
    int cpsRight;    // 右键 CPS (5~100, 连续)
    int keep;        // 保持连点 (无需按住鼠标)
    int gatk;        // 能攻击才点
    int gplace;      // 能放置才点
    int gcursor;     // 视角内才点 (光标隐藏时)
    int hotMaster;   // 总开关热键 (VK 码, 0=无)
    int hotLeft;     // 左键热键
    int hotRight;    // 右键热键
    int hotKeep;     // 保持热键
    int hotGatk;     // 能攻击闸门热键
    int hotGplace;   // 能放置闸门热键
    int hotGcursor;  // 视角闸门热键
    int dbgClicks;   // 调试: 记录每次 click 与状态变化到日志
};

// 默认热键: F8 总开关 / F6 左键 / F7 右键 / F9 保持 (F6=117 F7=118 F8=119 F9=120)
static Settings g_s = { 0, 1, 1, 20, 20, 0, 0, 0, 0,
                        119, 117, 118, 120, 0, 0, 0, 0 };

static int clampCps(int v)
{
    if (v < kCpsMin) v = kCpsMin;
    if (v > kCpsMax) v = kCpsMax;
    return v;
}

static void IniPath(char* out, size_t cap)
{
    const char* appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        snprintf(out, cap, "%s\\MCInGameClicker\\settings.ini", appdata);
    } else {
        snprintf(out, cap, "MCInGameClicker.ini");
    }
}

static int ReadInt(const char* t) { return atoi(t ? t : "0"); }

static void ClampCps(void)
{
    g_s.cpsLeft  = clampCps(g_s.cpsLeft);
    g_s.cpsRight = clampCps(g_s.cpsRight);
}

static void LoadSettings(void)
{
    char path[MAX_PATH];
    IniPath(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* e = strchr(line, '\r'); if (e) *e = 0;
        e = strchr(line, '\n');       if (e) *e = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line, *v = eq + 1;
        if      (!strcmp(k, "master"))    g_s.master    = ReadInt(v);
        else if (!strcmp(k, "left"))      g_s.left      = ReadInt(v);
        else if (!strcmp(k, "right"))     g_s.right     = ReadInt(v);
        else if (!strcmp(k, "cpsLeft"))   g_s.cpsLeft   = ReadInt(v);
        else if (!strcmp(k, "cpsRight"))  g_s.cpsRight  = ReadInt(v);
        else if (!strcmp(k, "keep"))      g_s.keep      = ReadInt(v);
        else if (!strcmp(k, "gatk"))      g_s.gatk      = ReadInt(v);
        else if (!strcmp(k, "gplace"))    g_s.gplace    = ReadInt(v);
        else if (!strcmp(k, "gcursor"))   g_s.gcursor   = ReadInt(v);
        else if (!strcmp(k, "hotMaster")) g_s.hotMaster = ReadInt(v);
        else if (!strcmp(k, "hotLeft"))   g_s.hotLeft   = ReadInt(v);
        else if (!strcmp(k, "hotRight"))  g_s.hotRight  = ReadInt(v);
        else if (!strcmp(k, "hotKeep"))   g_s.hotKeep   = ReadInt(v);
        else if (!strcmp(k, "hotGatk"))   g_s.hotGatk   = ReadInt(v);
        else if (!strcmp(k, "hotGplace")) g_s.hotGplace = ReadInt(v);
        else if (!strcmp(k, "hotGcursor"))g_s.hotGcursor = ReadInt(v);
        else if (!strcmp(k, "dbgClicks")) g_s.dbgClicks = ReadInt(v);
    }
    fclose(f);
    ClampCps();
}

static void SaveSettings(void)
{
    char path[MAX_PATH];
    IniPath(path, sizeof(path));
    char dir[MAX_PATH];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; CreateDirectoryA(dir, NULL); }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f,
        "master=%d\nleft=%d\nright=%d\ncpsLeft=%d\ncpsRight=%d\n"
        "keep=%d\ngatk=%d\ngplace=%d\ngcursor=%d\n"
        "hotMaster=%d\nhotLeft=%d\nhotRight=%d\nhotKeep=%d\n"
        "hotGatk=%d\nhotGplace=%d\nhotGcursor=%d\ndbgClicks=%d\n",
        g_s.master, g_s.left, g_s.right, g_s.cpsLeft, g_s.cpsRight,
        g_s.keep, g_s.gatk, g_s.gplace, g_s.gcursor,
        g_s.hotMaster, g_s.hotLeft, g_s.hotRight, g_s.hotKeep,
        g_s.hotGatk, g_s.hotGplace, g_s.hotGcursor, g_s.dbgClicks);
    fclose(f);
}

//--------------------------------------------------------------------------
// 游戏内菜单 (Insert 呼出)
// 渲染: 独立分层悬浮窗 (WS_EX_LAYERED) + 内存 DC 双缓存位图合成,
//       每帧一次 UpdateLayeredWindow 交给 DWM —— 不再往游戏前缓冲上
//       画 GDI, 彻底消除闪烁。
//--------------------------------------------------------------------------
static const int MENU_W = 360;
static const int TITLE_H = 26, INFO_H = 17, ROW_H = 22;
static const int ITEM_COUNT = 16;                       // 9 功能项 + 7 热键项
static const int ITEM_TOP = TITLE_H + INFO_H * 2 + 6;
static const int MENU_H = TITLE_H + INFO_H * 2 + 6 + ITEM_COUNT * ROW_H + 6 + 36 + 4;
static const int TOOL_W = 230;                          // 悬浮提示区
static const int OVL_W = MENU_W + TOOL_W;
static const int OVL_H = MENU_H;

enum ItemId { IT_MASTER = 0, IT_LEFT, IT_RIGHT, IT_CPSL, IT_CPSR,
              IT_KEEP, IT_GATK, IT_GPLACE, IT_CURSOR,
              IT_HOTMASTER, IT_HOTLEFT, IT_HOTRIGHT, IT_HOTKEEP,
              IT_HOTGATK, IT_HOTGPLACE, IT_HOTCURSOR };

static const wchar_t* const kItemNames[ITEM_COUNT] = {
    L"总开关 (连点)",
    L"左键连点",
    L"右键连点",
    L"左键 CPS",
    L"右键 CPS",
    L"保持连点 (无需按住)",
    L"能攻击才点",
    L"能放置才点",
    L"视角内才点 (光标隐藏)",
    L"总开关热键",
    L"左键热键",
    L"右键热键",
    L"保持热键",
    L"能攻击闸门热键",
    L"能放置闸门热键",
    L"视角闸门热键",
};

static const wchar_t* const kItemTips[ITEM_COUNT] = {
    L"连点总开关；关闭后左右键都不连点",
    L"开=按住左键连点；保持模式开启时无需按住",
    L"开=按住右键连点；保持模式开启时无需按住",
    L"左键每秒点击次数 (5~100)；按住左右拖动 / 滚轮 / ←→ 调整",
    L"右键每秒点击次数 (5~100)；按住左右拖动 / 滚轮 / ←→ 调整",
    L"开=无需按住鼠标持续连点；关=按住左/右键才点",
    L"开=仅准星瞄准可攻击生物时才点左键",
    L"开=仅手持放置物 (方块类) 时才点右键",
    L"开=光标隐藏 (游戏视角) 时连点，光标可见 (背包/聊天) 时暂停",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
    L"回车后按下新按键绑定，Esc 取消；0=无",
};

static volatile LONG g_menuOpen = 0;
static volatile LONG g_sel      = 0;
static volatile LONG g_hover    = -1;    // 鼠标悬停项 (悬浮提示)
static volatile LONG g_capturing = -1;   // 正在绑定热键的项
static int   g_dragItem = -1;            // 正在滑动的 CPS 项
static int   g_dragX = 0, g_dragVal = 0;
static bool  g_prevInsert = false;
static HWND  g_hwnd       = NULL;
static WNDPROC g_origWndProc = NULL;

// 分层悬浮窗 + 双缓存位图
static HWND  g_ovl = NULL;
static HDC   g_memDC = NULL;
static HBITMAP g_dib = NULL;
static void* g_bits = NULL;
static LONG  g_bitsW = 0, g_bitsH = 0;
static const char kOvlClass[] = "MCInGameOverlayW";
static HINSTANCE g_hInst = NULL;

// 色板 (CLR_BG/CLR_TIP 在 alpha 后处理中会被映射为半透明)
static const COLORREF CLR_BG   = RGB(10, 11, 16);
static const COLORREF CLR_TIP  = RGB(30, 32, 44);
static const COLORREF CLR_SEL  = RGB(52, 64, 102);
static const COLORREF CLR_SEP  = RGB(60, 70, 92);
static const COLORREF CLR_TXT  = RGB(224, 228, 238);
static const COLORREF CLR_DIM  = RGB(150, 160, 180);
static const COLORREF CLR_ON   = RGB(96, 208, 140);
static const COLORREF CLR_OFF  = RGB(240, 96, 110);
static const COLORREF CLR_WHT  = RGB(255, 255, 255);
static const COLORREF CLR_BRD  = RGB(110, 122, 150);

// 点击状态 (渲染线程单线程访问)
static bool  g_downL = false, g_downR = false;
static long  g_accL = 0, g_accR = 0;
static DWORD g_lastFrame = 0;

static HFONT  g_fTitle = NULL, g_fRow = NULL, g_fDim = NULL;
static HBRUSH g_brBg = NULL, g_brSel = NULL, g_brBorder = NULL, g_brSep = NULL;
static HBRUSH g_brTip = NULL;

static void ReleaseClick(bool left);   // 前向声明

static void OpenMenu(void)
{
    g_menuOpen = 1;
    g_sel = 0;
    g_hover = -1;
    g_capturing = -1;
    g_dragItem = -1;
    ReleaseClick(true);
    ReleaseClick(false);
    g_accL = g_accR = 0;
    Log("menu open");
}

static void CloseMenu(void)
{
    g_menuOpen = 0;
    g_capturing = -1;
    g_dragItem = -1;
    SaveSettings();
    Log("menu close (saved)");
}

static void ItemChanged(void) { SaveSettings(); }

static void ToggleItem(void)
{
    switch ((ItemId)(int)g_sel) {
    case IT_MASTER: g_s.master = !g_s.master; break;
    case IT_LEFT:   g_s.left   = !g_s.left;   break;
    case IT_RIGHT:  g_s.right  = !g_s.right;  break;
    case IT_CPSL:   g_s.cpsLeft  = clampCps(g_s.cpsLeft + 1); break;
    case IT_CPSR:   g_s.cpsRight = clampCps(g_s.cpsRight + 1); break;
    case IT_KEEP:   g_s.keep   = !g_s.keep;   break;
    case IT_GATK:   g_s.gatk   = !g_s.gatk;   break;
    case IT_GPLACE: g_s.gplace = !g_s.gplace; break;
    case IT_CURSOR: g_s.gcursor = !g_s.gcursor; break;
    default: break;   // 热键项: 由回车进入绑定, 不在这里切换
    }
    if ((ItemId)(int)g_sel == IT_MASTER && !g_s.master) {
        ReleaseClick(true);
        ReleaseClick(false);
        g_accL = g_accR = 0;
    }
    ItemChanged();
}

static void AdjustItem(int dir)
{
    if (dir == 0) return;
    switch ((ItemId)(int)g_sel) {
    case IT_CPSL:   g_s.cpsLeft  = clampCps(g_s.cpsLeft + dir); break;
    case IT_CPSR:   g_s.cpsRight = clampCps(g_s.cpsRight + dir); break;
    case IT_HOTMASTER: case IT_HOTLEFT: case IT_HOTRIGHT: case IT_HOTKEEP:
    case IT_HOTGATK: case IT_HOTGPLACE: case IT_HOTCURSOR:
        g_capturing = g_sel;   // ←→ 也进入绑定
        return;
    default: ToggleItem(); break;
    }
    ItemChanged();
}

//--------------------------------------------------------------------------
// 热键: 指针 / 匹配 / 绑定 / 触发
//--------------------------------------------------------------------------
static int* HotkeyPtr(int item)
{
    switch (item) {
    case IT_HOTMASTER:  return &g_s.hotMaster;
    case IT_HOTLEFT:    return &g_s.hotLeft;
    case IT_HOTRIGHT:   return &g_s.hotRight;
    case IT_HOTKEEP:    return &g_s.hotKeep;
    case IT_HOTGATK:    return &g_s.hotGatk;
    case IT_HOTGPLACE:  return &g_s.hotGplace;
    case IT_HOTCURSOR:  return &g_s.hotGcursor;
    default: return NULL;
    }
}

static int HotkeyOf(int item)
{
    int* p = HotkeyPtr(item);
    return p ? *p : 0;
}

static void BindHotkey(int item, int vk)
{
    int* p = HotkeyPtr(item);
    if (!p) return;
    if (vk == VK_ESCAPE || vk == VK_INSERT) return;   // 菜单键不允许绑定
    for (int i = IT_HOTMASTER; i <= IT_HOTCURSOR; ++i) {
        int* q = HotkeyPtr(i);
        if (q && q != p && *q == vk) *q = 0;          // 冲突解绑旧的
    }
    *p = vk;
    ItemChanged();
    Log("hotkey bound: item=%d vk=%d", item, vk);
}

static bool HotkeyVkMatch(int vk)
{
    if (!vk) return false;
    for (int i = IT_HOTMASTER; i <= IT_HOTCURSOR; ++i)
        if (HotkeyOf(i) == vk) return true;
    return false;
}

static void DoHotkey(int vk)
{
    for (int i = IT_HOTMASTER; i <= IT_HOTCURSOR; ++i) {
        if (HotkeyOf(i) != vk) continue;
        switch (i) {
        case IT_HOTMASTER: g_s.master = !g_s.master; break;
        case IT_HOTLEFT:   g_s.left   = !g_s.left;   break;
        case IT_HOTRIGHT:  g_s.right  = !g_s.right;  break;
        case IT_HOTKEEP:   g_s.keep   = !g_s.keep;   break;
        case IT_HOTGATK:   g_s.gatk   = !g_s.gatk;   break;
        case IT_HOTGPLACE: g_s.gplace = !g_s.gplace; break;
        case IT_HOTCURSOR: g_s.gcursor = !g_s.gcursor; break;
        default: return;
        }
        if (i == IT_HOTMASTER && !g_s.master) {
            ReleaseClick(true);
            ReleaseClick(false);
            g_accL = g_accR = 0;
        }
        ItemChanged();
        Log("hotkey vk=%d -> item=%d", vk, i);
        return;
    }
}

// 游戏窗口子类化: 菜单打开时处理菜单按键/热键绑定/点击空白快关;
// 菜单关闭时拦截已绑定热键并吃掉该键。
static LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // ---- 菜单关闭: 热键拦截 ----
    if (!g_menuOpen) {
        if (msg == WM_KEYDOWN && !((lp >> 30) & 1) && (UINT)wp != VK_INSERT) {
            if (HotkeyVkMatch((UINT)wp)) { DoHotkey((UINT)wp); return 0; }
        }
        if (msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN) {
            int vk = (msg == WM_MBUTTONDOWN) ? VK_MBUTTON
                   : ((HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2);
            if (HotkeyVkMatch(vk)) { DoHotkey(vk); return 0; }
        }
        return g_origWndProc ? CallWindowProcW(g_origWndProc, h, msg, wp, lp)
                             : DefWindowProcW(h, msg, wp, lp);
    }

    // ---- 菜单打开: 模态 ----
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        bool rep = ((lp >> 30) & 1) != 0;
        if (g_capturing >= 0) {                       // 热键绑定中
            if (vk == VK_ESCAPE) g_capturing = -1;    // 取消
            else { BindHotkey((int)g_capturing, vk); g_capturing = -1; }
            return 0;
        }
        if (rep) {
            // 按住自动重复: 仅 CPS 项连续滑动, 布尔项忽略防误触
            if (g_sel == IT_CPSL || g_sel == IT_CPSR)
                AdjustItem(vk == VK_LEFT ? -1 : (vk == VK_RIGHT ? 1 : 0));
            return 0;
        }
        switch (vk) {
        case VK_INSERT: g_prevInsert = true; CloseMenu(); break; // 抑制钩子轮询的同一击
        case VK_ESCAPE: CloseMenu(); break;
        case VK_UP:     g_sel = (g_sel + ITEM_COUNT - 1) % ITEM_COUNT; break;
        case VK_DOWN:   g_sel = (g_sel + 1) % ITEM_COUNT; break;
        case VK_LEFT:   AdjustItem(-1); break;
        case VK_RIGHT:  AdjustItem(+1); break;
        case VK_RETURN:
        case VK_SPACE:
            if (g_sel >= IT_HOTMASTER && g_sel <= IT_HOTCURSOR) g_capturing = g_sel;
            else ToggleItem();
            break;
        default: break;
        }
        return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR:
        return 0;
    case WM_LBUTTONDOWN:        // 点在菜单面板外 -> 快速关闭
        CloseMenu();
        return 0;
    case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEMOVE:
        return 0;
    default:
        break;
    }
    return g_origWndProc ? CallWindowProcW(g_origWndProc, h, msg, wp, lp)
                         : DefWindowProcW(h, msg, wp, lp);
}

static void PollInsertKey(void)
{
    bool cur = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (!g_menuOpen && cur && !g_prevInsert) OpenMenu();
    g_prevInsert = cur;
}

// 外部命令 (injector -menu) 通过命名事件触发打开菜单; 帧内轮询, 无线程
static HANDLE g_menuEvent = NULL;

static void PollMenuEvent(void)
{
    if (!g_menuEvent) return;
    if (WaitForSingleObject(g_menuEvent, 0) == WAIT_OBJECT_0) {
        ResetEvent(g_menuEvent);
        if (!g_menuOpen) OpenMenu();
    }
}

// 首帧从 DC 反查游戏窗口并安装 WndProc 子类
static void EnsureHwndAndSubclass(HDC hdc)
{
    if (g_hwnd) return;
    if (!hdc) return;
    HWND w = WindowFromDC(hdc);
    if (!w) return;
    g_hwnd = w;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)&MenuWndProc);
    if (prev && prev != &MenuWndProc) g_origWndProc = prev;
    Log("window subclass installed hwnd=%p", (void*)w);
}

static void EnsureUiObjects(void)
{
    if (!g_fTitle)
        g_fTitle = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    if (!g_fRow)
        g_fRow = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    if (!g_fDim)
        g_fDim = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    if (!g_brBg)     g_brBg     = CreateSolidBrush(CLR_BG);
    if (!g_brSel)    g_brSel    = CreateSolidBrush(CLR_SEL);
    if (!g_brBorder) g_brBorder = CreateSolidBrush(CLR_BRD);
    if (!g_brSep)    g_brSep    = CreateSolidBrush(CLR_SEP);
    if (!g_brTip)    g_brTip    = CreateSolidBrush(CLR_TIP);
}

static void AtoW(const char* s, wchar_t* out, int cap)
{
    if (cap <= 0) return;
    if (!s) { out[0] = 0; return; }
    MultiByteToWideChar(CP_ACP, 0, s, -1, out, cap - 1);
    out[cap - 1] = 0;
}

static void VkNameW(int vk, wchar_t* out, int cap)
{
    switch (vk) {
    case 0:            swprintf(out, cap, L"无"); return;
    case VK_LBUTTON:   swprintf(out, cap, L"鼠标左键"); return;
    case VK_RBUTTON:   swprintf(out, cap, L"鼠标右键"); return;
    case VK_MBUTTON:   swprintf(out, cap, L"鼠标中键"); return;
    case VK_XBUTTON1:  swprintf(out, cap, L"侧键1"); return;
    case VK_XBUTTON2:  swprintf(out, cap, L"侧键2"); return;
    default: break;
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_INSERT: case VK_HOME: case VK_PRIOR:
    case VK_DELETE: case VK_END: case VK_NEXT:
    case VK_NUMLOCK: case VK_SCROLL:
        scan |= 0x100;   // 扩展键位 (lParam bit24)
        break;
    default: break;
    }
    wchar_t buf[64];
    if (scan && GetKeyNameTextW(scan << 16, buf, 64) > 0) {
        wcsncpy(out, buf, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    swprintf(out, cap, L"键%d", vk);
}

static int ItemBool(int i)
{
    switch (i) {
    case IT_MASTER: return g_s.master;
    case IT_LEFT:   return g_s.left;
    case IT_RIGHT:  return g_s.right;
    case IT_KEEP:   return g_s.keep;
    case IT_GATK:   return g_s.gatk;
    case IT_GPLACE: return g_s.gplace;
    case IT_CURSOR: return g_s.gcursor;
    default: return 0;
    }
}

//--------------------------------------------------------------------------
// 分层悬浮窗 + 内存 DC 双缓存合成 (真正的双缓冲, 不闪)
//--------------------------------------------------------------------------
static int HitItem(int x, int y)
{
    if (x < 0 || x >= MENU_W) return -1;
    if (y < ITEM_TOP || y >= ITEM_TOP + ITEM_COUNT * ROW_H) return -1;
    return (y - ITEM_TOP) / ROW_H;
}

// 悬浮窗消息处理: 点击/拖动滑 CPS/滚轮/悬停
static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (!g_menuOpen) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int item = HitItem(pt.x, pt.y);
        if (item >= 0) {
            g_sel = item;
            if (item == IT_CPSL || item == IT_CPSR) {
                g_dragItem = item;
                g_dragX = pt.x;
                g_dragVal = (item == IT_CPSL) ? g_s.cpsLeft : g_s.cpsRight;
                SetCapture(h);
            } else if (item >= IT_HOTMASTER && item <= IT_HOTCURSOR) {
                g_capturing = item;
            } else {
                ToggleItem();
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_menuOpen) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        g_hover = HitItem(pt.x, pt.y);
        if (g_dragItem >= 0) {                    // 滑动调 CPS: 每 2px = 1 CPS
            int v = clampCps(g_dragVal + (pt.x - g_dragX) / 2);
            if (g_dragItem == IT_CPSL) {
                if (g_s.cpsLeft != v) { g_s.cpsLeft = v; ItemChanged(); }
            } else {
                if (g_s.cpsRight != v) { g_s.cpsRight = v; ItemChanged(); }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragItem >= 0) { g_dragItem = -1; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!g_menuOpen) break;
        int d = (short)HIWORD(wp);
        if (g_hover == IT_CPSL) {
            g_s.cpsLeft = clampCps(g_s.cpsLeft + (d > 0 ? 1 : -1));
            ItemChanged();
        } else if (g_hover == IT_CPSR) {
            g_s.cpsRight = clampCps(g_s.cpsRight + (d > 0 ? 1 : -1));
            ItemChanged();
        } else {
            g_sel = (g_sel + ITEM_COUNT + (d > 0 ? -1 : 1)) % ITEM_COUNT;
        }
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool EnsureOverlay(void)
{
    if (g_ovl) return true;
    HINSTANCE inst = g_hInst ? g_hInst : GetModuleHandleA(NULL);
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kOvlClass;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassExA(&wc);   // 重复注册返回失败, 忽略
    g_ovl = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOvlClass, "MCInGame", WS_POPUP,
        0, 0, OVL_W, OVL_H, NULL, NULL, inst, NULL);
    if (g_ovl) Log("overlay window created");
    return g_ovl != NULL;
}

static bool EnsureBackbuffer(void)
{
    if (g_memDC) return true;
    HDC screen = GetDC(NULL);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!g_memDC) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = OVL_W;
    bi.bmiHeader.biHeight = -OVL_H;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    if (!g_dib) {
        DeleteDC(g_memDC);
        g_memDC = NULL;
        return false;
    }
    SelectObject(g_memDC, g_dib);
    g_bitsW = OVL_W;
    g_bitsH = OVL_H;
    return true;
}

// GDI 写 DIB 时 alpha 通道固定为 0; 后处理: 背景/提示底色映射为半透明
// (预乘 alpha), 其余内容 (文字/选中条/边框) 置为不透明。
static void ApplyAlpha(void)
{
    BYTE* p = (BYTE*)g_bits;
    for (LONG y = 0; y < g_bitsH; ++y) {
        BYTE* row = p + (size_t)y * g_bitsW * 4;
        for (LONG x = 0; x < g_bitsW; ++x) {
            BYTE* px = row + (size_t)x * 4;
            if (px[3] != 0) continue;    // 已是透明区
            COLORREF c = RGB(px[2], px[1], px[0]);
            if (c == CLR_BG) {
                px[0] = px[0] * 210 / 255;
                px[1] = px[1] * 210 / 255;
                px[2] = px[2] * 210 / 255;
                px[3] = 210;
            } else if (c == CLR_TIP) {
                px[0] = px[0] * 205 / 255;
                px[1] = px[1] * 205 / 255;
                px[2] = px[2] * 205 / 255;
                px[3] = 205;
            } else {
                px[3] = 255;
            }
        }
    }
}

static void ComposeFrame(void)
{
    if (!EnsureBackbuffer()) return;
    memset(g_bits, 0, (size_t)g_bitsW * 4 * g_bitsH);   // 全透明
    HDC dc = g_memDC;
    EnsureUiObjects();

    RECT panel = { 0, 0, MENU_W, MENU_H };
    FillRect(dc, &panel, g_brBg);
    FrameRect(dc, &panel, g_brBorder);
    SetBkMode(dc, TRANSPARENT);

    // 标题
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, CLR_WHT);
    RECT rt = { 12, 0, MENU_W - 12, TITLE_H };
    DrawTextW(dc, L"MCInGame 连点器  v1.1", -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 状态行
    SelectObject(dc, g_fDim);
    SetTextColor(dc, CLR_DIM);
    wchar_t st[256];
    if (g_status->ready) {
        wchar_t map[40], ver[32];
        AtoW(g_status->mappingName, map, 40);
        AtoW(g_gameVer, ver, 32);
        swprintf(st, 256, L"状态: 就绪 · 映射 %ls · %ls", map,
                 g_gameVer[0] ? ver : L"?");
    } else {
        wchar_t err[96];
        AtoW(g_status->errMsg, err, 96);
        swprintf(st, 256, L"状态: 解析中… (%ls)", g_status->errMsg[0] ? err : L"-");
    }
    RECT r1 = { 12, TITLE_H, MENU_W - 12, TITLE_H + INFO_H };
    DrawTextW(dc, st, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 实时状态行
    SelectObject(dc, g_fRow);
    SetTextColor(dc, CLR_TXT);
    const wchar_t* atk  = g_status->canAttack ? L"是" : L"否";
    const wchar_t* held = !g_status->inGame ? L"—"
                        : (g_status->heldItemNull ? L"空"
                        : (g_status->canPlace ? L"方块" : L"物品"));
    wchar_t live[128];
    swprintf(live, 128, L"能攻击:%ls  手持:%ls  左:%ls 右:%ls",
             atk, held, g_downL ? L"●" : L"○", g_downR ? L"●" : L"○");
    RECT r2 = { 12, TITLE_H + INFO_H, MENU_W - 12, TITLE_H + INFO_H * 2 };
    DrawTextW(dc, live, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 分隔线
    RECT sep1 = { 6, TITLE_H + INFO_H * 2 + 2, MENU_W - 6, TITLE_H + INFO_H * 2 + 3 };
    FillRect(dc, &sep1, g_brSep);

    // 菜单项
    for (int i = 0; i < ITEM_COUNT; ++i) {
        RECT rr = { 6, ITEM_TOP + i * ROW_H, MENU_W - 6, ITEM_TOP + (i + 1) * ROW_H };
        if (i == g_sel) FillRect(dc, &rr, g_brSel);
        SelectObject(dc, g_fRow);
        SetTextColor(dc, CLR_TXT);
        RECT rn = { rr.left + 8, rr.top, rr.right - 84, rr.bottom };
        DrawTextW(dc, kItemNames[i], -1, &rn, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT rv = { rr.right - 80, rr.top, rr.right - 6, rr.bottom };
        wchar_t val[40];
        if (i == g_capturing) {
            wcscpy(val, L"按新键…");
            SetTextColor(dc, CLR_ON);
        } else if (i == IT_CPSL) {
            swprintf(val, 40, L"%d/s", g_s.cpsLeft);
            SetTextColor(dc, CLR_WHT);
        } else if (i == IT_CPSR) {
            swprintf(val, 40, L"%d/s", g_s.cpsRight);
            SetTextColor(dc, CLR_WHT);
        } else if (i >= IT_HOTMASTER && i <= IT_HOTCURSOR) {
            VkNameW(HotkeyOf(i), val, 40);
            SetTextColor(dc, CLR_WHT);
        } else if (ItemBool(i)) {
            wcscpy(val, L"开");
            SetTextColor(dc, CLR_ON);
        } else {
            wcscpy(val, L"关");
            SetTextColor(dc, CLR_OFF);
        }
        DrawTextW(dc, val, -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // 分隔线
    RECT sep2 = { 6, ITEM_TOP + ITEM_COUNT * ROW_H + 2, MENU_W - 6, ITEM_TOP + ITEM_COUNT * ROW_H + 3 };
    FillRect(dc, &sep2, g_brSep);

    // 底部提示
    SelectObject(dc, g_fDim);
    SetTextColor(dc, CLR_DIM);
    RECT rf1 = { 12, ITEM_TOP + ITEM_COUNT * ROW_H + 6, MENU_W - 12, ITEM_TOP + ITEM_COUNT * ROW_H + 22 };
    DrawTextW(dc, L"Insert/Esc 或点菜单外关闭 · ↑↓ 选择 · ←→/回车 调整", -1, &rf1,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rf2 = { 12, ITEM_TOP + ITEM_COUNT * ROW_H + 22, MENU_W - 12, ITEM_TOP + ITEM_COUNT * ROW_H + 38 };
    DrawTextW(dc, L"拖动/滚轮滑 CPS · 热键项回车绑定 · 设置自动保存", -1, &rf2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 悬浮提示 (鼠标悬停时, 显示在面板右侧)
    if (g_hover >= 0 && g_hover < ITEM_COUNT) {
        int ty = ITEM_TOP + g_hover * ROW_H;
        if (ty + 46 > MENU_H) ty = MENU_H - 48;
        RECT tr = { MENU_W + 8, ty, OVL_W - 8, ty + 46 };
        FillRect(dc, &tr, g_brTip);
        FrameRect(dc, &tr, g_brBorder);
        SetTextColor(dc, CLR_TXT);
        RECT tt = { tr.left + 6, tr.top + 3, tr.right - 6, tr.bottom - 3 };
        DrawTextW(dc, kItemTips[g_hover], -1, &tt,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    ApplyAlpha();
}

// 每帧: 菜单打开 -> 定位悬浮窗 + 合成 + 一次 UpdateLayeredWindow (原子呈现)
static void UpdateOverlay(void)
{
    if (!g_menuOpen || !g_hwnd || IsIconic(g_hwnd)) {
        if (g_ovl && IsWindowVisible(g_ovl)) ShowWindow(g_ovl, SW_HIDE);
        return;
    }
    if (!EnsureOverlay() || !EnsureBackbuffer()) return;
    POINT tl = { 10, 10 };
    ClientToScreen(g_hwnd, &tl);
    SetWindowPos(g_ovl, HWND_TOPMOST, tl.x, tl.y, OVL_W, OVL_H,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ComposeFrame();
    BLENDFUNCTION bf = { AC_SRC_ALPHA, 0, 255, 0 };
    POINT src = { 0, 0 };
    SIZE sz = { OVL_W, OVL_H };
    UpdateLayeredWindow(g_ovl, NULL, NULL, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

//--------------------------------------------------------------------------
// 连点逻辑 (渲染帧内, 时间累加器保证平均 CPS 精确)
//--------------------------------------------------------------------------
static bool IsCursorShowing(void)
{
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) != FALSE && (ci.flags & CURSOR_SHOWING) != 0;
}

static void PostClick(bool left, bool down)
{
    if (!g_hwnd) return;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g_hwnd, &pt);
    LPARAM lp = MAKELPARAM(pt.x, pt.y);
    if (left)
        PostMessageA(g_hwnd, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                     down ? MK_LBUTTON : 0, lp);
    else
        PostMessageA(g_hwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP,
                     down ? MK_RBUTTON : 0, lp);
}

static void ReleaseClick(bool left)
{
    bool& down = left ? g_downL : g_downR;
    if (down) { PostClick(left, false); down = false; }
}

static void EmitClicks(long& acc, int cps, bool left, DWORD dt)
{
    int interval = 1000 / cps;
    if (interval < 1) interval = 1;
    acc += (long)dt;
    if (acc > interval * 3) acc = interval * 3;   // 防长时间暂停后爆发
    int n = 0;
    while (acc >= interval && n < 2) {
        acc -= interval;
        bool& down = left ? g_downL : g_downR;
        down = !down;                              // DOWN/UP 交替, 与 v2.9 同节奏
        PostClick(left, down);
        if (g_s.dbgClicks)
            Log("click %s %s (canAtk=%d canPlace=%d inGame=%d)",
                left ? "L" : "R", down ? "down" : "up",
                (int)g_status->canAttack, (int)g_status->canPlace,
                (int)g_status->inGame);
        n++;
    }
}

static void ClickTick(HDC hdc)
{
    EnsureHwndAndSubclass(hdc);
    if (g_menuOpen) return;      // 菜单打开时暂停连点
    if (!g_hwnd) return;

    // 前台检查: 游戏窗口不在前台不点 (debugLogClicks=1 时放行, 测试用)
    bool fgOk = g_s.dbgClicks || GetForegroundWindow() == g_hwnd;
    DWORD now = GetTickCount();
    DWORD dt = now - g_lastFrame;
    g_lastFrame = now;
    if (dt > 500) dt = 500;

    bool inGame     = g_status->inGame == 1;      // 玩家存在才点 (主菜单不点)
    bool canAtkOk   = !g_s.gatk || g_status->canAttack == 1;
    bool canPlaceOk = !g_s.gplace || g_status->canPlace == 1;
    bool cursorOk   = !g_s.gcursor || !IsCursorShowing();

    bool lActive = fgOk && inGame && g_s.master && g_s.left &&
                   (g_s.keep || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) &&
                   canAtkOk && cursorOk;
    bool rActive = fgOk && inGame && g_s.master && g_s.right &&
                   (g_s.keep || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) &&
                   canPlaceOk && cursorOk;

    if (!lActive) { ReleaseClick(true);  g_accL = 0; }
    if (!rActive) { ReleaseClick(false); g_accR = 0; }
    if (lActive) EmitClicks(g_accL, g_s.cpsLeft,  true,  dt);
    if (rActive) EmitClicks(g_accR, g_s.cpsRight, false, dt);
}

//--------------------------------------------------------------------------
// 追加失败原因到 failLog (每轮解析开始前清空)
//--------------------------------------------------------------------------
static void AppendFail(const char* s)
{
    if (!s) return;
    size_t n = strlen(g_status->failLog);
    size_t m = strlen(s);
    if (n + m + 2 >= sizeof(g_status->failLog)) return;
    if (n > 0) g_status->failLog[n++] = ' ';
    memcpy(g_status->failLog + n, s, m + 1);
}

//--------------------------------------------------------------------------
// 一套命名体系下, 所有需要解析的东西叫什么。
//--------------------------------------------------------------------------
struct JniMap {
    const char* name;              // 标识
    const char* mcClass;
    const char* mcSig;             // getMinecraft 返回类型描述符
    const char* getMinecraft;
    const char* thePlayerField;
    const char* playerFieldSig;
    const char* mopField;
    const char* mopFieldSig;
    const char* mopClass;
    const char* typeOfHitField;
    const char* typeOfHitGetter;
    const char* typeOfHitSig;      // 字段描述符 或 getter 签名
    const char* entityHitClass;
    const char* entityHitField;
    const char* entityHitGetter;
    const char* entityHitSig;
    const char* typeClass;
    const char* entityConstField;
    const char* entityConstAlt;
    const char* entityConstSig;
    const char* entityClass;
    const char* canAttackWithItem; // NULL = 跳过 (现代版本已移除)
    const char* isAliveMethod;
    const char* isAttackable;      // NULL = 跳过
    const char* livingClass;
    // ---- 放置物判定 (全部可选: 解析失败仅 canPlace=0, 不拖垮 canAttack) ----
    const char* heldItemGetter;
    const char* heldItemSig;
    const char* itemStackClass;
    const char* itemGetItem;
    const char* itemGetItemSig;
    const char* itemBlockClass;
};

#include "mc_maps_generated.h"

//--------------------------------------------------------------------------
// 类加载辅助 (解决 Forge/launchwrapper 的"双份类副本"问题)
//--------------------------------------------------------------------------
static jclass FindLoadedClassByName(JNIEnv* env, jobject loader, const char* slashName)
{
    if (!loader) return NULL;
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (!loaderCls) { env->ExceptionClear(); return NULL; }
    jmethodID flc = env->GetMethodID(loaderCls, "findLoadedClass",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!flc) { env->ExceptionClear(); return NULL; }
    char dot[256];
    size_t n = strlen(slashName);
    if (n >= sizeof(dot)) n = sizeof(dot) - 1;
    for (size_t i = 0; i < n; ++i) dot[i] = (slashName[i] == '/') ? '.' : slashName[i];
    dot[n] = 0;
    jstring nm = env->NewStringUTF(dot);
    if (!nm) { env->ExceptionClear(); return NULL; }
    jclass c = (jclass)env->CallObjectMethod(loader, flc, nm);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    env->DeleteLocalRef(nm);
    return c;
}

static jclass LoadClass(JNIEnv* env, const char* slashName, jobject loader,
                        jclass clsCls, jmethodID forName)
{
    if (loader) {
        env->ExceptionClear();
        jclass loaded = FindLoadedClassByName(env, loader, slashName);
        if (loaded) return loaded;
        snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                 "flc(%s)=X loader=%p", slashName, (void*)loader);
        g_status->lastError = 205;
    }
    if (loader && clsCls && forName) {
        char dot[256];
        size_t n = strlen(slashName);
        if (n >= sizeof(dot)) n = sizeof(dot) - 1;
        for (size_t i = 0; i < n; ++i) dot[i] = (slashName[i] == '/') ? '.' : slashName[i];
        dot[n] = 0;
        jstring name = env->NewStringUTF(dot);
        if (!name) { env->ExceptionClear(); return NULL; }
        jclass c = (jclass)env->CallStaticObjectMethod(clsCls, forName, name, JNI_FALSE, loader);
        env->DeleteLocalRef(name);
        if (env->ExceptionCheck()) {
            jthrowable ex = env->ExceptionOccurred();
            env->ExceptionClear();
            jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
            jmethodID getName = exCls ? env->GetMethodID(clsCls, "getName", "()Ljava/lang/String;") : NULL;
            jstring nm = getName ? (jstring)env->CallObjectMethod(exCls, getName) : NULL;
            const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
            snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                     "forName(%s) -> %s", dot, utf ? utf : "?");
            g_status->lastError = 202;
            if (utf) env->ReleaseStringUTFChars(nm, utf);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return NULL;
        }
        if (!c) {
            snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                     "forName(%s) -> NULL(无异常)", dot);
            g_status->lastError = 204;
        }
        return c;
    }
    jclass c = env->FindClass(slashName);
    if (env->ExceptionCheck()) {
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
        jmethodID getName = exCls ? env->GetMethodID(env->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;") : NULL;
        jstring nm = getName ? (jstring)env->CallObjectMethod(exCls, getName) : NULL;
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                 "FindClass(%s) -> %s", slashName, utf ? utf : "?");
        g_status->lastError = 203;
        if (utf) env->ReleaseStringUTFChars(nm, utf);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return NULL;
    }
    if (!c) {
        snprintf(g_status->errMsg, sizeof(g_status->errMsg),
                 "FindClass(%s) -> NULL(无异常)", slashName);
        g_status->lastError = 204;
    }
    return c;
}

static jobject FindLaunchClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_LAUNCH_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L1:find-Launch");
    jclass launch = env->FindClass("net/minecraft/launchwrapper/Launch");
    if (!launch) { env->ExceptionClear(); return NULL; }
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L2:Launch-ok");
    jfieldID cl = env->GetStaticFieldID(launch, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;");
    if (!cl) { env->ExceptionClear(); return NULL; }
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L3:field-ok");
    jobject loader = env->GetStaticObjectField(launch, cl);
    if (env->ExceptionCheck()) env->ExceptionClear();
    CopyName(g_status->errMsg, sizeof(g_status->errMsg),
             loader ? "L4:loader-ok" : "L4:loader-null");
    return loader;
#endif
}

static jobject FindThreadClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_THREAD_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
    jclass threadCls = env->FindClass("java/lang/Thread");
    if (!threadCls) { env->ExceptionClear(); return NULL; }
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
    jmethodID getAll = env->GetStaticMethodID(threadCls, "getAllStackTraces",
                                              "()Ljava/util/Map;");
    if (!getAll) { env->ExceptionClear(); return NULL; }
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T2:getAll");
    jobject map = env->CallStaticObjectMethod(threadCls, getAll);
    if (env->ExceptionCheck() || !map) { env->ExceptionClear(); return NULL; }
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T3:map");

    jclass mapCls = env->FindClass("java/util/Map");
    jclass setCls = env->FindClass("java/util/Set");
    jclass itCls  = env->FindClass("java/util/Iterator");
    if (!mapCls || !setCls || !itCls) { env->ExceptionClear(); return NULL; }
    jmethodID keySet = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
    jmethodID iterator = env->GetMethodID(setCls, "iterator", "()Ljava/util/Iterator;");
    jmethodID hasNext = env->GetMethodID(itCls, "hasNext", "()Z");
    jmethodID next = env->GetMethodID(itCls, "next", "()Ljava/lang/Object;");
    jmethodID getCtx = env->GetMethodID(threadCls, "getContextClassLoader",
                                        "()Ljava/lang/ClassLoader;");
    if (!keySet || !iterator || !hasNext || !next || !getCtx) {
        env->ExceptionClear();
        return NULL;
    }

    jobject set = env->CallObjectMethod(map, keySet);
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:keySet");
    jobject it  = env->CallObjectMethod(set, iterator);
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T5:iterator");
    if (env->ExceptionCheck() || !it) { env->ExceptionClear(); return NULL; }

    DWORD tStart = GetTickCount();
    jobject found = NULL;
    jobject found2 = NULL;
    while (env->CallBooleanMethod(it, hasNext)) {
        if (GetTickCount() - tStart > 10000) break;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T6:hasNext");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject thread = env->CallObjectMethod(it, next);
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T7:next");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject loader = env->CallObjectMethod(thread, getCtx);
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T8:getCtx");
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, clsCls, forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            found = loader;
            break;
        }
        for (int i = 0; i < kGenMapCount && !found2; ++i) {
            if (strcmp(kGenMaps[i].mcClass, "net/minecraft/client/Minecraft") == 0)
                continue;
            jclass probe = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                found2 = loader;
                break;
            }
        }
    }
    if (!found) found = found2;
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T9:done");
    if (env->ExceptionCheck()) env->ExceptionClear();
    return found;
#endif
}

static jobject FindGameClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_GAME_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    jobject loader = FindLaunchClassLoader(env, clsCls, forName);
    if (loader) {
        bool canLoad = false;
        for (int i = 0; i < kGenMapCount && !canLoad; ++i) {
            jclass probe = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                canLoad = true;
            }
        }
        if (canLoad) return loader;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                 "E4:launch-loader-cant-load-game");
        env->DeleteLocalRef(loader);
    }
    loader = FindThreadClassLoader(env, clsCls, forName);
    if (loader) return loader;
    CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E5:system-loader");
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls) { env->ExceptionClear(); return NULL; }
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader",
                                              "()Ljava/lang/ClassLoader;");
    if (!getSys) { env->ExceptionClear(); return NULL; }
    jobject sys = env->CallStaticObjectMethod(clCls, getSys);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return sys;
#endif
}

//--------------------------------------------------------------------------
// 环境探测: forge / optifine / fabric / launchwrapper
//--------------------------------------------------------------------------
static void DetectEnv(JNIEnv* env, jobject loader, jclass clsCls, jmethodID forName,
                      char* out, size_t cap)
{
    struct Mark { const char* cls; const char* tag; };
    static const Mark marks[] = {
        { "net/minecraftforge/fml/common/FMLCommonHandler",  "forge" },
        { "net/minecraftforge/fml/loading/FMLLoader",        "forge" },
        { "net/neoforged/fml/loading/FMLLoader",             "neoforge" },
        { "optifine/OptiFineClassTransformer",               "optifine" },
        { "net/optifine/Config",                             "optifine" },
        { "net/fabricmc/loader/FabricLoader",                "fabric" },
        { "net/minecraft/launchwrapper/Launch",              "launchwrapper" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
        jclass c = LoadClass(env, marks[i].cls, loader, clsCls, forName);
        if (c) {
            env->DeleteLocalRef(c);
            if (out[0]) {
                size_t n = strlen(out);
                if (n + strlen(marks[i].tag) + 2 < cap) {
                    out[n++] = '+';
                    strcpy(out + n, marks[i].tag);
                }
            } else {
                CopyName(out, cap, marks[i].tag);
            }
        }
    }
}

//--------------------------------------------------------------------------
// 解析结果 (ID 解析一次, 永久使用)
//--------------------------------------------------------------------------
struct Resolved {
    bool       ok;
    const char* name;
    jclass     mcClass, mopClass, typeClass, entityClass, livingClass;
    jclass     entityHitCls;
    jclass     javaLangClass;
    jmethodID  getMinecraft, canAttackWithItem, isAliveMethod, isAttackable, classGetName;
    jmethodID  typeOfHitGetter, entityHitGetter;
    jfieldID   thePlayerField, mopField, typeOfHitField, entityHitField, entityConstField;
    bool       placeOk;
    jclass     itemStackCls;
    jclass     itemBlockCls;
    jmethodID  heldItemGetter;
    jmethodID  itemGetItem;
};

static void NoteErr(const char* mapName, const char* why)
{
    snprintf(g_status->errMsg, sizeof(g_status->errMsg), "%s: %s", mapName, why);
    g_status->lastError = 201;
}

static bool ResolveWith(JNIEnv* env, const JniMap& m, Resolved& r,
                        jobject loader, jclass clsCls, jmethodID forName)
{
    memset(&r, 0, sizeof(r));

    jclass mc  = LoadClass(env, m.mcClass, loader, clsCls, forName);
    jclass mop = LoadClass(env, m.mopClass, loader, clsCls, forName);
    jclass typ = LoadClass(env, m.typeClass, loader, clsCls, forName);
    jclass ent = LoadClass(env, m.entityClass, loader, clsCls, forName);
    jclass liv = LoadClass(env, m.livingClass, loader, clsCls, forName);
    jclass jlc = env->FindClass("java/lang/Class");
    if (!mc || !mop || !typ || !ent || !liv || !jlc) {
        AppendFail(m.name);
        AppendFail(":cls ");
        if (g_status->lastError == 202 || g_status->lastError == 203) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s: %s", m.name, g_status->errMsg);
            CopyName(g_status->errMsg, sizeof(g_status->errMsg), tmp);
        } else {
            NoteErr(m.name, "FindClass 失败");
        }
        env->ExceptionClear();
        return false;
    }

    jmethodID getMc = env->GetStaticMethodID(mc, m.getMinecraft, m.mcSig);
    jmethodID alive = env->GetMethodID(ent, m.isAliveMethod, "()Z");
    jmethodID name  = env->GetMethodID(jlc, "getName", "()Ljava/lang/String;");
    jfieldID  pl    = env->GetFieldID(mc, m.thePlayerField, m.playerFieldSig);
    jfieldID  mopF  = env->GetFieldID(mc, m.mopField, m.mopFieldSig);
    if (!getMc || !alive || !name || !pl || !mopF) {
        NoteErr(m.name, "成员 ID 解析失败");
        AppendFail(m.name);
        AppendFail(":member ");
        env->ExceptionClear();
        return false;
    }

    jmethodID hitG = NULL;
    jfieldID  hitF = NULL;
    if (m.typeOfHitGetter) {
        hitG = env->GetMethodID(mop, m.typeOfHitGetter, m.typeOfHitSig);
        if (!hitG) { env->ExceptionClear(); return false; }
    } else {
        hitF = env->GetFieldID(mop, m.typeOfHitField, m.typeOfHitSig);
        if (!hitF) { env->ExceptionClear(); return false; }
    }

    jclass    ehr = NULL;
    jmethodID entG = NULL;
    jfieldID  entF = NULL;
    if (m.entityHitGetter) {
        ehr = LoadClass(env, m.entityHitClass, loader, clsCls, forName);
        if (!ehr) { env->ExceptionClear(); return false; }
        entG = env->GetMethodID(ehr, m.entityHitGetter, m.entityHitSig);
        if (!entG) { env->ExceptionClear(); return false; }
    } else {
        entF = env->GetFieldID(mop, m.entityHitField, m.entityHitSig);
        if (!entF) { env->ExceptionClear(); return false; }
    }

    jmethodID atk = NULL;
    if (m.canAttackWithItem) {
        atk = env->GetMethodID(ent, m.canAttackWithItem, "()Z");
        if (!atk) { env->ExceptionClear(); return false; }
    }
    jmethodID atkb = NULL;
    if (m.isAttackable) {
        atkb = env->GetMethodID(ent, m.isAttackable, "()Z");
        if (!atkb) { env->ExceptionClear(); return false; }
    }

    jfieldID entC = env->GetStaticFieldID(typ, m.entityConstField, m.entityConstSig);
    if (!entC && m.entityConstAlt) {
        env->ExceptionClear();
        entC = env->GetStaticFieldID(typ, m.entityConstAlt, m.entityConstSig);
    }
    if (!entC) { env->ExceptionClear(); return false; }

    jclass    itemStackCls = NULL;
    jclass    itemBlockCls = NULL;
    jmethodID heldGetter   = NULL;
    jmethodID getItem      = NULL;
    if (m.heldItemGetter && m.itemStackClass && m.itemGetItem && m.itemBlockClass) {
        heldGetter = env->GetMethodID(liv, m.heldItemGetter, m.heldItemSig);
        if (env->ExceptionCheck()) { env->ExceptionClear(); heldGetter = NULL; }
        itemStackCls = LoadClass(env, m.itemStackClass, loader, clsCls, forName);
        itemBlockCls = LoadClass(env, m.itemBlockClass, loader, clsCls, forName);
        if (itemStackCls) {
            getItem = env->GetMethodID(itemStackCls, m.itemGetItem, m.itemGetItemSig);
            if (env->ExceptionCheck()) { env->ExceptionClear(); getItem = NULL; }
        }
        if (!heldGetter || !itemStackCls || !getItem || !itemBlockCls) {
            if (itemStackCls) env->DeleteLocalRef(itemStackCls);
            if (itemBlockCls) env->DeleteLocalRef(itemBlockCls);
            itemStackCls = NULL; itemBlockCls = NULL;
            heldGetter = NULL; getItem = NULL;
        }
    }

    r.ok                = true;
    r.name              = m.name;
    r.mcClass           = mc;
    r.mopClass          = mop;
    r.typeClass         = typ;
    r.entityClass       = ent;
    r.livingClass       = liv;
    r.entityHitCls      = ehr;
    r.javaLangClass     = jlc;
    r.getMinecraft      = getMc;
    r.canAttackWithItem = atk;
    r.isAliveMethod     = alive;
    r.isAttackable      = atkb;
    r.classGetName      = name;
    r.thePlayerField    = pl;
    r.mopField          = mopF;
    r.typeOfHitGetter   = hitG;
    r.typeOfHitField    = hitF;
    r.entityHitGetter   = entG;
    r.entityHitField    = entF;
    r.entityConstField  = entC;
    r.placeOk           = (heldGetter && itemStackCls && getItem && itemBlockCls);
    r.itemStackCls      = itemStackCls;
    r.itemBlockCls      = itemBlockCls;
    r.heldItemGetter    = heldGetter;
    r.itemGetItem       = getItem;
    return true;
}

//--------------------------------------------------------------------------
// 拷贝字符串 (保证以 \0 结尾)
//--------------------------------------------------------------------------
static void CopyName(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

//--------------------------------------------------------------------------
// 每帧更新: 计算 "是否能攻击" / "手持放置物" 并写入内部状态
//--------------------------------------------------------------------------
static void UpdateStatus(JNIEnv* env, const Resolved& r)
{
    if (env->PushLocalFrame(32) < 0) {
        g_status->lastError = 100;
        return;
    }
    g_status->lastError = 0;

    jobject mc = env->CallStaticObjectMethod(r.mcClass, r.getMinecraft);
    if (env->ExceptionCheck()) {
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exCls = ex ? env->GetObjectClass(ex) : NULL;
        jstring nm = exCls ? (jstring)env->CallObjectMethod(exCls, r.classGetName) : NULL;
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), utf);
        if (utf) env->ReleaseStringUTFChars(nm, utf);
        g_status->lastError = 101;
    }
    if (!mc) {
        g_status->inGame    = 0;
        g_status->canAttack = 0;
        g_status->canPlace  = 0;
        g_status->placeReady = 0;
        g_status->heldItemNull = 0;
        g_status->hitType   = 0;
        g_status->mcNull    = 1;
        g_status->tick++;
        env->PopLocalFrame(NULL);
        return;
    }
    g_status->mcNull = 0;

    jobject player = env->GetObjectField(mc, r.thePlayerField);
    jobject mop    = env->GetObjectField(mc, r.mopField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 102; }

    g_status->inGame = (player != NULL);

    if (!player) {
        g_status->canAttack      = 0;
        g_status->canPlace       = 0;
        g_status->placeReady     = 0;
        g_status->heldItemNull   = 0;
        g_status->hitType        = 0;
        g_status->targetLiving   = 0;
        g_status->targetAlive    = 0;
        g_status->targetIsPlayer = 0;
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), NULL);
        g_status->tick++;
        env->PopLocalFrame(NULL);
        return;
    }

    g_status->placeReady = r.placeOk ? 1 : 0;
    LONG canPlace    = 0;
    LONG heldNull    = 1;
    CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), NULL);
    if (r.heldItemGetter && r.itemStackCls && r.itemGetItem && r.itemBlockCls) {
        jobject stack = env->CallObjectMethod(player, r.heldItemGetter);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 110; }
        if (stack) {
            heldNull = 0;
            jobject item = env->CallObjectMethod(stack, r.itemGetItem);
            if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 111; }
            if (item) {
                canPlace = env->IsInstanceOf(item, r.itemBlockCls) ? 1 : 0;
                jobject cls = env->GetObjectClass(item);
                jstring nm  = (jstring)env->CallObjectMethod(cls, r.classGetName);
                if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 112; }
                const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
                if (utf) {
                    CopyName(g_status->heldItemName, sizeof(g_status->heldItemName), utf);
                    env->ReleaseStringUTFChars(nm, utf);
                }
                env->DeleteLocalRef(cls);
                if (nm) env->DeleteLocalRef(nm);
                env->DeleteLocalRef(item);
            }
            env->DeleteLocalRef(stack);
        }
    }

    if (!mop) {
        g_status->canAttack      = 0;
        g_status->hitType        = 0;
        g_status->targetLiving   = 0;
        g_status->targetAlive    = 0;
        g_status->targetIsPlayer = 0;
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        g_status->canPlace       = canPlace;
        g_status->heldItemNull   = heldNull;
        g_status->tick++;
        env->PopLocalFrame(NULL);
        return;
    }

    jobject typeObj = r.typeOfHitGetter
        ? env->CallObjectMethod(mop, r.typeOfHitGetter)
        : env->GetObjectField(mop, r.typeOfHitField);
    jobject entConst = env->GetStaticObjectField(r.typeClass, r.entityConstField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 103; }

    int hit = 0;
    if (typeObj) {
        if (entConst && env->IsSameObject(typeObj, entConst)) hit = 2;
        else hit = 1;
    }
    g_status->hitType = hit;

    jobject entity = NULL;
    if (hit == 2) {
        entity = r.entityHitGetter
            ? env->CallObjectMethod(mop, r.entityHitGetter)
            : env->GetObjectField(mop, r.entityHitField);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 104; }
    }

    LONG living = 0, alive = 0, isSelf = 0;
    if (entity) {
        living = env->IsInstanceOf(entity, r.livingClass) ? 1 : 0;
        alive  = env->CallBooleanMethod(entity, r.isAliveMethod) ? 1 : 0;
        isSelf = (player && env->IsSameObject(entity, player)) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 105; }

        jobject cls = env->GetObjectClass(entity);
        jstring nm  = (jstring)env->CallObjectMethod(cls, r.classGetName);
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 106; }
        const char* utf = nm ? env->GetStringUTFChars(nm, NULL) : NULL;
        if (utf) {
            CopyName(g_status->targetName, sizeof(g_status->targetName), utf);
            env->ReleaseStringUTFChars(nm, utf);
        } else {
            CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
        }
        env->DeleteLocalRef(cls);
        if (nm) env->DeleteLocalRef(nm);
    } else {
        CopyName(g_status->targetName, sizeof(g_status->targetName), NULL);
    }

    LONG canUseItem = 1;
    if (r.canAttackWithItem) {
        canUseItem = env->CallBooleanMethod(player, r.canAttackWithItem) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 107; }
    }

    LONG attackable = 1;
    if (r.isAttackable && entity) {
        attackable = env->CallBooleanMethod(entity, r.isAttackable) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 108; }
    }

    g_status->targetLiving    = living;
    g_status->targetAlive     = alive;
    g_status->targetIsPlayer  = isSelf;
    g_status->canAttack       = (hit == 2 && living && alive && !isSelf && canUseItem && attackable) ? 1 : 0;
    g_status->canPlace        = canPlace;
    g_status->heldItemNull    = heldNull;
    g_status->tick++;

    env->PopLocalFrame(NULL);
}

//--------------------------------------------------------------------------
// 终极方案: ClassLoader.findLoadedClass(name) 直接拿"游戏已加载的类"
//--------------------------------------------------------------------------
static jclass FindLoadedGameClass(JNIEnv* env, jobject loader, const char* name,
                                  jclass clsCls, jmethodID forName,
                                  const char* getterName, const char* getterSig)
{
    if (!loader) return NULL;
    jclass loaderCls = env->FindClass("java/lang/ClassLoader");
    if (!loaderCls) { env->ExceptionClear(); return NULL; }
    jmethodID flc = env->GetMethodID(loaderCls, "findLoadedClass",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!flc) { env->ExceptionClear(); return NULL; }
    jstring nm = env->NewStringUTF(name);
    if (!nm) { env->ExceptionClear(); return NULL; }
    jclass c = (jclass)env->CallObjectMethod(loader, flc, nm);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(nm);
    if (!c) return NULL;
    if (getterName) {
        jmethodID m = env->GetStaticMethodID(c, getterName, getterSig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jobject inst = m ? env->CallStaticObjectMethod(c, m) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!inst) return NULL;
        env->DeleteLocalRef(inst);
    }
    return c;
}

//--------------------------------------------------------------------------
// 版本指纹探测: 用每版 vanilla 表的混淆 mcClass 快速定位版本 (原版环境)
//--------------------------------------------------------------------------
static int DetectVersionHint(JNIEnv* env, jobject loader, jclass clsCls, jmethodID forName,
                             DWORD deadline)
{
    for (int i = 0; i < kGenMapCount; ++i) {
        if (GetTickCount() > deadline) break;
        if (strncmp(kGenMaps[i].name, "vanilla", 7) != 0) continue;
        jclass c = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
        if (!c) { env->ExceptionClear(); continue; }
        jmethodID gm = env->GetStaticMethodID(c, kGenMaps[i].getMinecraft, kGenMaps[i].mcSig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jobject inst = gm ? env->CallStaticObjectMethod(c, gm) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (inst) {
            env->DeleteLocalRef(inst);
            env->DeleteLocalRef(c);
            return i;
        }
        env->DeleteLocalRef(c);
    }
    return 0;
}

//--------------------------------------------------------------------------
// 版本检测 (JVM classpath): 从 System.getProperty 提取 Minecraft 版本号
//--------------------------------------------------------------------------
static int ExtractMcVersion(const char* text, char* out, size_t cap)
{
    out[0] = 0;
    if (!text) return 0;
    const char* start = NULL;
    const char* p = text;
    while ((p = strstr(p, "versions")) != NULL) {
        const char* s = p + 8;
        while (*s == '\\' || *s == '/' || *s == ' ' || *s == '"') s++;
        if (s[0] == '1' && s[1] == '.') { start = s; break; }
        p += 8;
    }
    if (!start) {
        p = text;
        while ((p = strstr(p, "1.")) != NULL) {
            const char* q = p + 2;
            if (*q >= '0' && *q <= '9' &&
                (p == text || p[-1] == ';' || p[-1] == '\\' || p[-1] == '/' ||
                 p[-1] == ' ' || p[-1] == '=' || p[-1] == ':')) {
                start = p;
                break;
            }
            p = q;
        }
    }
    if (!start) return 0;
    size_t n = 0;
    const char* q = start;
    while (*q && ((*q >= '0' && *q <= '9') || *q == '.') && n < cap - 1) {
        out[n++] = *q++;
    }
    while (n > 0 && out[n - 1] == '.') out[--n] = 0;
    out[n] = 0;
    return n > 0;
}

static void GetGameVersion(JNIEnv* env, char* out, size_t cap)
{
    out[0] = 0;
    if (!env) return;
    jclass sysCls = env->FindClass("java/lang/System");
    if (!sysCls) { env->ExceptionClear(); return; }
    jmethodID getProp = env->GetStaticMethodID(sysCls, "getProperty",
                                               "(Ljava/lang/String;)Ljava/lang/String;");
    if (!getProp) { env->ExceptionClear(); return; }
    const char* props[] = { "java.class.path", "java.library.path" };
    for (int i = 0; i < 2 && out[0] == 0; ++i) {
        jstring key = env->NewStringUTF(props[i]);
        if (!key) { env->ExceptionClear(); continue; }
        jstring val = (jstring)env->CallStaticObjectMethod(sysCls, getProp, key);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(key);
        if (!val) continue;
        const char* utf = env->GetStringUTFChars(val, NULL);
        if (utf) {
            ExtractMcVersion(utf, out, cap);
            env->ReleaseStringUTFChars(val, utf);
        }
        env->DeleteLocalRef(val);
    }
}

static int FindVersionMapIndex(const char* version)
{
    char tag[32];
    size_t n = 0;
    for (const char* p = version; *p && n < sizeof(tag) - 1; ++p) {
        if (*p >= '0' && *p <= '9') tag[n++] = *p;
    }
    tag[n] = 0;
    if (n == 0) return -1;
    for (int i = 0; i < kGenMapCount; ++i) {
        const char* name = kGenMaps[i].name;
        size_t nameLen = strlen(name);
        size_t digits = 0;
        while (digits < nameLen && name[nameLen - 1 - digits] >= '0' &&
               name[nameLen - 1 - digits] <= '9') {
            digits++;
        }
        if (digits == n && strncmp(name + nameLen - n, tag, n) == 0) {
            return i;
        }
    }
    return -1;
}

//--------------------------------------------------------------------------
// 帧驱动状态机 —— 不创建线程、不 AttachCurrentThread。
// 内联钩住 gdi32!SwapBuffers, 由游戏自己的渲染线程每帧调用;
// 钩子内 GetEnv() 复用该线程已有的 JNIEnv, 解析/采样全部在该线程内
// 分帧完成 (每帧预算 8ms, 采样 5ms 节流)。
//--------------------------------------------------------------------------
typedef jint (JNICALL* GetCreatedVMs_t)(JavaVM**, jsize, jsize*);

// ---- SwapBuffers 钩子 ----
typedef BOOL (WINAPI* SwapBuffersFn)(HDC);
static SwapBuffersFn g_origSwapBuffers = NULL;
static LONG   g_attached = 0;      // DllMain 幂等

static JavaVM* g_vm = NULL;

// ---- 解析状态机 ----
enum {
    ST_CLS = 0, ST_ENV, ST_VER, ST_LAUNCH, ST_LAUNCH_V, ST_SCAN, ST_SYS,
    ST_MAPS, ST_FIX, ST_STEADY
};
static int      g_stage     = ST_CLS;
static int      g_probeIdx  = 0;
static int      g_mapIdx    = 0;
static int      g_mapStart  = 0;
static bool     g_mapWrap   = false;
static DWORD    g_retryAt   = 0;
static DWORD    g_scanStart = 0;
static DWORD    g_nullSince = 0;
static DWORD    g_lastWork  = 0;

static Resolved   g_res;
static const JniMap* g_resMap = NULL;
static jclass    g_clsCls      = NULL;
static jmethodID g_forName     = NULL;
static jobject   g_launchLoader = NULL;
static jobject   g_gameLoader   = NULL;
static jobject   g_sysLoader    = NULL;
static bool      g_useGameLoader = false;
static int       g_verHint = -2;

static jobject   g_scanIt = NULL;
static jclass    g_scanThreadCls = NULL;
static jmethodID g_scanHasNext = NULL, g_scanNext = NULL, g_scanGetCtx = NULL;
static jmethodID g_scanGetAll = NULL;

static const DWORD kFrameBudgetMs      = 8;
static const DWORD kLaunchVerifyBudgetMs = 300;
static const DWORD kScanTimeoutMs      = 10000;
static const DWORD kResolveRetryMs     = 500;
static const DWORD kSamplePeriodMs     = 5;

//--------------------------------------------------------------------------
// 本地引用 -> 全局引用
//--------------------------------------------------------------------------
static jobject ToGlobal(JNIEnv* env, jobject o)
{
    if (!o) return NULL;
    jobject g = env->NewGlobalRef(o);
    env->DeleteLocalRef(o);
    return g;
}

static bool PromoteResolved(JNIEnv* env, Resolved& r)
{
    jclass* c[] = { &r.mcClass, &r.mopClass, &r.typeClass, &r.entityClass,
                    &r.livingClass, &r.entityHitCls, &r.javaLangClass,
                    &r.itemStackCls, &r.itemBlockCls };
    for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); ++i) {
        if (!*c[i]) continue;
        jobject g = env->NewGlobalRef(*c[i]);
        if (!g) {
            for (size_t j = 0; j < i; ++j) {
                if (*c[j]) { env->DeleteGlobalRef(*c[j]); *c[j] = NULL; }
            }
            return false;
        }
        env->DeleteLocalRef(*c[i]);
        *c[i] = (jclass)g;
    }
    return true;
}

static void FreeResolved(JNIEnv* env, Resolved& r)
{
    jclass* c[] = { &r.mcClass, &r.mopClass, &r.typeClass, &r.entityClass,
                    &r.livingClass, &r.entityHitCls, &r.javaLangClass,
                    &r.itemStackCls, &r.itemBlockCls };
    for (size_t i = 0; i < sizeof(c)/sizeof(c[0]); ++i) {
        if (*c[i]) { env->DeleteGlobalRef(*c[i]); *c[i] = NULL; }
    }
    memset(&r, 0, sizeof(r));
}

static void ScanAbort(JNIEnv* env)
{
    if (g_scanIt)        { env->DeleteGlobalRef(g_scanIt);        g_scanIt = NULL; }
    if (g_scanThreadCls) { env->DeleteGlobalRef(g_scanThreadCls); g_scanThreadCls = NULL; }
    g_scanHasNext = g_scanNext = g_scanGetCtx = g_scanGetAll = NULL;
}

//--------------------------------------------------------------------------
// 遍历所有 Java 线程找游戏类加载器 (帧驱动可续)
//--------------------------------------------------------------------------
static int ThreadScanStep(JNIEnv* env, DWORD now, DWORD deadline)
{
    if (now - g_scanStart > kScanTimeoutMs) { ScanAbort(env); return 0; }

    if (!g_scanIt) {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
        g_scanThreadCls = (jclass)ToGlobal(env, env->FindClass("java/lang/Thread"));
        if (!g_scanThreadCls) { env->ExceptionClear(); return -1; }
        g_scanGetAll = env->GetStaticMethodID(g_scanThreadCls, "getAllStackTraces",
                                              "()Ljava/util/Map;");
        jclass mapCls = env->FindClass("java/util/Map");
        jclass setCls = env->FindClass("java/util/Set");
        jclass itCls  = env->FindClass("java/util/Iterator");
        if (!g_scanGetAll || !mapCls || !setCls || !itCls) {
            env->ExceptionClear(); ScanAbort(env); return 0;
        }
        jmethodID keySet   = env->GetMethodID(mapCls, "keySet", "()Ljava/util/Set;");
        jmethodID iterator = env->GetMethodID(setCls, "iterator", "()Ljava/util/Iterator;");
        g_scanHasNext = env->GetMethodID(itCls, "hasNext", "()Z");
        g_scanNext    = env->GetMethodID(itCls, "next", "()Ljava/lang/Object;");
        g_scanGetCtx  = env->GetMethodID(g_scanThreadCls, "getContextClassLoader",
                                         "()Ljava/lang/ClassLoader;");
        if (!keySet || !iterator || !g_scanHasNext || !g_scanNext || !g_scanGetCtx) {
            env->ExceptionClear(); ScanAbort(env); return 0;
        }
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
        jobject map = env->CallStaticObjectMethod(g_scanThreadCls, g_scanGetAll);
        if (env->ExceptionCheck() || !map) { env->ExceptionClear(); ScanAbort(env); return 0; }
        jobject set = env->CallObjectMethod(map, keySet);
        jobject it  = set ? env->CallObjectMethod(set, iterator) : NULL;
        if (env->ExceptionCheck() || !it) { env->ExceptionClear(); ScanAbort(env); return 0; }
        g_scanIt = ToGlobal(env, it);
        if (!g_scanIt) return -1;
        g_probeIdx = 0;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:iterator");
    }

    while (GetTickCount() < deadline) {
        if (!env->CallBooleanMethod(g_scanIt, g_scanHasNext)) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ScanAbort(env);
            return 0;
        }
        jobject thread = env->CallObjectMethod(g_scanIt, g_scanNext);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!thread) continue;
        jobject loader = env->CallObjectMethod(thread, g_scanGetCtx);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, g_clsCls, g_forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            g_gameLoader = ToGlobal(env, loader);
            g_useGameLoader = true;
            ScanAbort(env);
            return 1;
        }
        while (g_probeIdx < kGenMapCount) {
            if (GetTickCount() >= deadline) return -1;
            if (strcmp(kGenMaps[g_probeIdx].mcClass, "net/minecraft/client/Minecraft") == 0) {
                g_probeIdx++;
                continue;
            }
            jclass probe = LoadClass(env, kGenMaps[g_probeIdx].mcClass,
                                     loader, g_clsCls, g_forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                g_gameLoader = ToGlobal(env, loader);
                g_useGameLoader = true;
                ScanAbort(env);
                return 1;
            }
            g_probeIdx++;
        }
        g_probeIdx = 0;
    }
    return -1;
}

//--------------------------------------------------------------------------
// 按命名空间前缀 + 版本数字 tag 找映射表索引
//--------------------------------------------------------------------------
static int FindMapByNamespace(const char* ns, const char* version)
{
    char tag[32];
    size_t n = 0;
    for (const char* p = version; *p && n < sizeof(tag) - 1; ++p)
        if (*p >= '0' && *p <= '9') tag[n++] = *p;
    tag[n] = 0;
    if (n == 0) return -1;
    size_t nsLen = strlen(ns);
    for (int i = 0; i < kGenMapCount; ++i) {
        const char* name = kGenMaps[i].name;
        if (strncmp(name, ns, nsLen) != 0) continue;
        size_t nameLen = strlen(name);
        size_t digits = 0;
        while (digits < nameLen && name[nameLen - 1 - digits] >= '0' &&
               name[nameLen - 1 - digits] <= '9') digits++;
        if (digits == n && strncmp(name + nameLen - n, tag, n) == 0) return i;
    }
    return -1;
}

//--------------------------------------------------------------------------
// 每帧泵: 解析状态机 + 采样
//--------------------------------------------------------------------------
static void PumpInner(JNIEnv* env, DWORD now)
{
    switch (g_stage) {
    case ST_CLS: {
        jclass c = env->FindClass("java/lang/Class");
        if (!c) { env->ExceptionClear(); return; }
        g_clsCls = (jclass)env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
        if (!g_clsCls) return;
        g_forName = env->GetStaticMethodID(g_clsCls, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!g_forName) return;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H1:cls-ok");
        g_stage = ST_ENV;
    } /* fallthrough */
    case ST_ENV: {
#ifndef NO_ENV_DETECT
        DetectEnv(env, NULL, g_clsCls, g_forName,
                  g_status->envName, sizeof(g_status->envName));
#endif
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H2:env-ok");
        g_stage = ST_VER;
    } /* fallthrough */
    case ST_VER: {
        GetGameVersion(env, g_gameVer, sizeof(g_gameVer));
        g_verHint = g_gameVer[0] ? FindVersionMapIndex(g_gameVer) : -1;
        if (g_gameVer[0]) {
            size_t el = strlen(g_status->envName);
            if (el + 1 + strlen(g_gameVer) < sizeof(g_status->envName)) {
                if (el) g_status->envName[el++] = '+';
                strcpy(g_status->envName + el, g_gameVer);
            }
        }
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H3:ver-ok");
        g_stage = ST_LAUNCH;
    } /* fallthrough */
    case ST_LAUNCH: {
        g_launchLoader = ToGlobal(env, FindLaunchClassLoader(env, g_clsCls, g_forName));
        if (g_launchLoader) {
            g_probeIdx = 0;
            g_stage = ST_LAUNCH_V;
        } else {
            g_scanStart = now;
            g_stage = ST_SCAN;
        }
        return;
    }
    case ST_LAUNCH_V: {
        DWORD deadline = now + kLaunchVerifyBudgetMs;
        while (g_probeIdx < kGenMapCount) {
            if (GetTickCount() > deadline) return;
            jclass probe = LoadClass(env, kGenMaps[g_probeIdx].mcClass,
                                     g_launchLoader, g_clsCls, g_forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                g_gameLoader = env->NewGlobalRef(g_launchLoader);
                g_useGameLoader = true;
                CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                         "launch-loader");
                g_stage = ST_SYS;
                return;
            }
            g_probeIdx++;
        }
        CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                 "E4:launch-loader-cant-load-game");
        g_scanStart = now;
        g_stage = ST_SCAN;
        return;
    }
    case ST_SCAN: {
        DWORD deadline = now + kFrameBudgetMs;
        int rc = ThreadScanStep(env, now, deadline);
        if (rc == 1) {
            CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                     "thread-loader");
            g_stage = ST_SYS;
        } else if (rc == 0) {
            g_stage = ST_SYS;
        }
        return;
    }
    case ST_SYS: {
        jclass clCls = env->FindClass("java/lang/ClassLoader");
        jmethodID getSys = clCls ? env->GetStaticMethodID(clCls, "getSystemClassLoader",
                                                          "()Ljava/lang/ClassLoader;") : NULL;
        jobject sys = getSys ? env->CallStaticObjectMethod(clCls, getSys) : NULL;
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_sysLoader = ToGlobal(env, sys);
        if (!g_gameLoader) {
            CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                     g_sysLoader ? "system-loader" : "(null - 使用 FindClass)");
        }
        int start = (g_verHint >= 0) ? g_verHint
                  : DetectVersionHint(env, g_useGameLoader ? g_gameLoader : g_sysLoader,
                                      g_clsCls, g_forName, now + kLaunchVerifyBudgetMs);
        if (g_verHint >= 0 && g_status->envName[0]) {
            const char* ns = NULL;
            if (strstr(g_status->envName, "neoforge")) ns = "mojang";
            else if (strstr(g_status->envName, "fabric")) ns = "intermediary";
            else if (strstr(g_status->envName, "forge")) ns = "forge";
            if (ns) {
                int hint2 = FindMapByNamespace(ns, g_gameVer);
                if (hint2 >= 0) start = hint2;
            }
        }
        g_mapStart = start;
        g_mapIdx = start;
        g_mapWrap = false;
        g_status->failLog[0] = 0;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H4:loader-ok");
        g_stage = ST_MAPS;
    } /* fallthrough */
    case ST_MAPS: {
        if (g_mapWrap) {
            if (now < g_retryAt) return;
            g_mapWrap = false;
            g_mapIdx = g_mapStart;
            g_status->failLog[0] = 0;
        }
        DWORD deadline = now + kFrameBudgetMs;
        for (;;) {
            if (GetTickCount() >= deadline) return;
            const JniMap& m = kGenMaps[g_mapIdx];
            jobject loader = g_useGameLoader ? g_gameLoader : g_sysLoader;
            CopyName(g_status->errMsg, sizeof(g_status->errMsg), m.name);
            env->ExceptionClear();
            Resolved tr;
            bool ok = ResolveWith(env, m, tr, loader, g_clsCls, g_forName);
            if (ok) {
                jobject inst = env->CallStaticObjectMethod(tr.mcClass, tr.getMinecraft);
                bool hasInst = (inst != NULL);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (inst) env->DeleteLocalRef(inst);
                if (hasInst) {
                    if (PromoteResolved(env, tr)) {
                        g_resMap = &m;
                        FreeResolved(env, g_res);
                        g_res = tr;
                        g_stage = ST_FIX;
                        return;
                    }
                    return;
                }
                NoteErr(m.name, "getMinecraft()=null(副本)");
                ok = false;
            }
            if (!ok && g_gameLoader && g_sysLoader && g_gameLoader != g_sysLoader) {
                jobject loader2 = (loader == g_gameLoader) ? g_sysLoader : g_gameLoader;
                Resolved tr2;
                bool ok2 = ResolveWith(env, m, tr2, loader2, g_clsCls, g_forName);
                if (ok2) {
                    jobject inst2 = env->CallStaticObjectMethod(tr2.mcClass, tr2.getMinecraft);
                    bool hasInst2 = (inst2 != NULL);
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (inst2) env->DeleteLocalRef(inst2);
                    if (hasInst2) {
                        if (PromoteResolved(env, tr2)) {
                            g_resMap = &m;
                            FreeResolved(env, g_res);
                            g_res = tr2;
                            g_useGameLoader = (loader2 == g_gameLoader);
                            g_stage = ST_FIX;
                            return;
                        }
                        return;
                    }
                }
            }
            g_mapIdx++;
            if (g_mapIdx >= kGenMapCount) {
                g_mapIdx = 0;
                g_mapWrap = true;
                g_retryAt = now + kResolveRetryMs;
                return;
            }
        }
    }
    case ST_FIX: {
        const JniMap& mOk = *g_resMap;
#ifndef NO_REAL_FIX
        jclass realMc = FindLoadedGameClass(env, g_launchLoader, mOk.mcClass,
                                            g_clsCls, g_forName,
                                            mOk.getMinecraft, mOk.mcSig);
        if (realMc) {
            jmethodID getLdr = env->GetMethodID(g_clsCls, "getClassLoader",
                                                "()Ljava/lang/ClassLoader;");
            jobject realLoader = getLdr ? env->CallObjectMethod(realMc, getLdr) : NULL;
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (realLoader) {
                Resolved resReal;
                bool okReal = ResolveWith(env, mOk, resReal, realLoader, g_clsCls, g_forName);
                if (okReal && PromoteResolved(env, resReal)) {
                    FreeResolved(env, g_res);
                    g_res = resReal;
                    CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                             "findLoadedClass-real-loader");
                }
                env->DeleteLocalRef(realLoader);
            }
            env->DeleteLocalRef(realMc);
        }
#endif
        g_status->ready = 1;
        CopyName(g_status->mappingName, sizeof(g_status->mappingName), g_res.name);
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H5:ready");
        Log("ready: map=%s env=%s ver=%s loader=%s",
            g_status->mappingName, g_status->envName,
            g_gameVer[0] ? g_gameVer : "?", g_status->loaderName);
        g_stage = ST_STEADY;
        return;
    }
    case ST_STEADY: {
        if (g_status->mcNull) {
            if (!g_nullSince) g_nullSince = now;
            else if (now - g_nullSince >= 500) {
                g_nullSince = now;
                if (g_gameLoader && g_sysLoader) g_useGameLoader = !g_useGameLoader;
                else if (g_gameLoader) g_useGameLoader = true;
                FreeResolved(env, g_res);
                g_resMap = NULL;
                g_status->ready = 0;
                CopyName(g_status->mappingName, sizeof(g_status->mappingName), NULL);
                g_mapIdx = g_mapStart;
                g_mapWrap = false;
                g_stage = ST_MAPS;
                return;
            }
        } else {
            g_nullSince = 0;
        }
        if (now - g_lastWork >= kSamplePeriodMs) {
            g_lastWork = now;
            UpdateStatus(env, g_res);
            if (g_s.dbgClicks) {
                static LONG lastAtk = -1, lastPlace = -1;
                if (lastAtk != g_status->canAttack || lastPlace != g_status->canPlace) {
                    lastAtk = g_status->canAttack;
                    lastPlace = g_status->canPlace;
                    Log("status canAtk=%d canPlace=%d inGame=%d hit=%d held=%s",
                        (int)g_status->canAttack, (int)g_status->canPlace,
                        (int)g_status->inGame, (int)g_status->hitType,
                        g_status->heldItemName[0] ? g_status->heldItemName : "(none)");
                }
            }
        }
        return;
    }
    }
}

static void PumpFrame(JNIEnv* env)
{
    if (env->PushLocalFrame(512) < 0) return;
    PumpInner(env, GetTickCount());
    env->PopLocalFrame(NULL);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

//--------------------------------------------------------------------------
// gdi32!SwapBuffers 钩子: 解析状态 + 连点 + 菜单绘制都在此帧内完成
//--------------------------------------------------------------------------
static BOOL WINAPI HookSwapBuffers(HDC hdc)
{
    static bool g_frameLogged = false;
    if (!g_frameLogged) {
        g_frameLogged = true;
        Log("first-frame: hdc=%p vm=%p", (void*)hdc, (void*)g_vm);
    }

    PollInsertKey();
    PollMenuEvent();

    if (!g_vm) {
        HMODULE jvm = GetModuleHandleA("jvm.dll");
        if (jvm) {
            GetCreatedVMs_t getVMs =
                (GetCreatedVMs_t)(void*)GetProcAddress(jvm, "JNI_GetCreatedJavaVMs");
            if (getVMs) {
                JavaVM* v = NULL;
                jsize   n = 0;
                if (getVMs(&v, 1, &n) == JNI_OK && n >= 1 && v) g_vm = v;
            }
        }
    }
    if (g_vm) {
        JNIEnv* env = NULL;
        jint rc = g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (rc == JNI_OK && env) {
            static bool g_envLogged = false;
            if (!g_envLogged) { g_envLogged = true; Log("first-frame: getenv=OK"); }
            PumpFrame(env);
        } else {
            static bool g_envFailLogged = false;
            if (!g_envFailLogged) {
                g_envFailLogged = true;
                Log("first-frame: getenv=%d (not JNI_OK)", (int)rc);
            }
            if (g_stage == ST_CLS) {
                snprintf(g_status->errMsg, sizeof(g_status->errMsg), "HG:getenv=%d", (int)rc);
            }
        }
    } else if (g_stage == ST_CLS) {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "HV:no-vm");
    }

    ClickTick(hdc);

    BOOL r = g_origSwapBuffers ? g_origSwapBuffers(hdc) : FALSE;

    UpdateOverlay();
    return r;
}

//--------------------------------------------------------------------------
// 内联钩子 (x64): 覆盖 gdi32!SwapBuffers 的 12 字节导出存根
// (FF 25 rel32 + 6 字节 CC 填充) 为绝对跳转 (mov rax,imm64; jmp rax)。
// 真实函数地址从存根的槽位读出 (与参考实现完全一致的装法)。
//--------------------------------------------------------------------------
static void WriteAbsJmp(BYTE* dst, void* to)
{
    dst[0] = 0x48; dst[1] = 0xB8;               // mov rax, imm64
    memcpy(dst + 2, &to, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;             // jmp rax
}

static bool InstallSwapBuffersHook(void)
{
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) return false;
    BYTE* stub = (BYTE*)(void*)GetProcAddress(gdi, "SwapBuffers");
    if (!stub) return false;
    if (stub[0] != 0xFF || stub[1] != 0x25) return false;
    for (int i = 6; i < 12; ++i)
        if (stub[i] != 0xCC) return false;
    INT32 rel;
    memcpy(&rel, stub + 2, 4);
    g_origSwapBuffers = *(SwapBuffersFn*)(stub + 6 + rel);
    if (!g_origSwapBuffers) return false;

    BYTE patch[12];
    WriteAbsJmp(patch, (void*)(void(*)())&HookSwapBuffers);

    DWORD old = 0;
    if (!VirtualProtect(stub, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(stub, patch, 12);
    VirtualProtect(stub, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    return true;
}

//--------------------------------------------------------------------------
// 与参考实现一致: DllMain 内把本 DLL 从 PEB 模块三链表中摘除
// (InLoadOrder/InMemoryOrder/InInitializationOrder 全部摘下并自环),
// 之后模块枚举 / GetModuleHandle 都看不到本 DLL, DLL 仍正常驻留执行。
//--------------------------------------------------------------------------
static void HideModuleFromPeb(HMODULE self)
{
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return;
    LIST_ENTRY* memHead = (LIST_ENTRY*)(ldr + 0x20);
    for (LIST_ENTRY* e = memHead->Flink; e != memHead; e = e->Flink) {
        BYTE* entry = (BYTE*)e - 0x10;                 // InMemoryOrderLinks 偏移 0x10
        if (*(void**)(entry + 0x30) == (void*)self) {  // DllBase 偏移 0x30
            LIST_ENTRY* il = (LIST_ENTRY*)(entry + 0x00);  // InLoadOrderLinks
            LIST_ENTRY* im = (LIST_ENTRY*)(entry + 0x10);  // InMemoryOrderLinks
            LIST_ENTRY* ii = (LIST_ENTRY*)(entry + 0x20);  // InInitializationOrderLinks
            il->Blink->Flink = il->Flink; il->Flink->Blink = il->Blink;
            im->Blink->Flink = im->Flink; im->Flink->Blink = im->Blink;
            ii->Blink->Flink = ii->Flink; ii->Flink->Blink = ii->Blink;
            il->Flink = il->Blink = il;
            im->Flink = im->Blink = im;
            ii->Flink = ii->Blink = ii;
            break;
        }
    }
}

// 自检: PEB 链表中是否还能找到自己
static bool PebStillVisible(HMODULE self)
{
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return true;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return true;
    LIST_ENTRY* memHead = (LIST_ENTRY*)(ldr + 0x20);
    for (LIST_ENTRY* e = memHead->Flink; e != memHead; e = e->Flink) {
        BYTE* entry = (BYTE*)e - 0x10;
        if (*(void**)(entry + 0x30) == (void*)self) return true;
    }
    return false;
}

//--------------------------------------------------------------------------
// 导出函数
//--------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) BOOL WINAPI GetCanAttackNow(void)
{
    return g_status->ready ? (BOOL)g_status->canAttack : FALSE;
}

__declspec(dllexport) BOOL WINAPI IsJniReady(void)
{
    return g_status->ready ? TRUE : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetStatus(Status* out)
{
    if (!out) return FALSE;
    *out = g_st;
    return TRUE;
}

// 调试/自动化测试: 触发打开菜单 (injector -menu 通过命名事件调用)
__declspec(dllexport) void WINAPI DbgOpenMenu(void)
{
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\MCInGameMenuEvent");
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

} // extern "C"

//--------------------------------------------------------------------------
// DLL 入口: 不创建任何线程、不创建 socket、不创建共享内存。
// 全部工作在游戏渲染线程内由 SwapBuffers 钩子驱动。
//--------------------------------------------------------------------------
static HANDLE g_guardMutex = NULL;

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        if (InterlockedExchange(&g_attached, 1)) return TRUE; // 防重复 LoadLibrary 二次初始化
        g_hInst = hInst;

        // 单实例守卫: 同名互斥体已存在说明本进程已注入过
        HANDLE mut = CreateMutexA(NULL, FALSE, "Local\\MCInGameClicker");
        if (mut && GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
        g_guardMutex = mut;

        // 外部菜单命令事件 (injector -menu)
        g_menuEvent = CreateEventA(NULL, TRUE, FALSE, "Local\\MCInGameMenuEvent");

        // 与参考实现一致: 从 PEB 模块链表摘除自身 (模块枚举不可见)
        HideModuleFromPeb(hInst);

        memset(&g_st, 0, sizeof(g_st));
        g_st.magic   = kMagic;
        g_st.version = kVersion;
        CopyName(g_st.errMsg, sizeof(g_st.errMsg), "H0:dllmain-init");

        LoadSettings();
        g_lastFrame = GetTickCount();

        if (!InstallSwapBuffersHook()) {
            Log("dllmain: init, peb-hidden=%d, HOOK-INSTALL-FAIL",
                PebStillVisible(hInst) ? 0 : 1);
        } else {
            Log("dllmain: init, peb-hidden=%d, hook-ok, settings master=%d left=%d right=%d cps=%d/%d keep=%d gates=%d/%d/%d",
                PebStillVisible(hInst) ? 0 : 1,
                g_s.master, g_s.left, g_s.right, g_s.cpsLeft, g_s.cpsRight,
                g_s.keep, g_s.gatk, g_s.gplace, g_s.gcursor);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        // 还原 WndProc 子类 (窗口可能已销毁, IsWindow 保护)
        if (g_hwnd && g_origWndProc && IsWindow(g_hwnd)) {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        }
        if (g_ovl && IsWindow(g_ovl)) DestroyWindow(g_ovl);
        if (g_memDC) DeleteDC(g_memDC);
        if (g_menuEvent) { CloseHandle(g_menuEvent); g_menuEvent = NULL; }
        if (g_guardMutex) { CloseHandle(g_guardMutex); g_guardMutex = NULL; }
        // 钩子有意不还原: DLL 与进程同生命周期, 退出阶段还原补丁
        // 会与其他仍在执行的线程竞态, 无意义且有崩溃风险。
    }
    return TRUE;
}
