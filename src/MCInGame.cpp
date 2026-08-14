//============================================================================
//  MCInGame.dll  (v1.4)
//  注入到 Minecraft (java/javaw) 进程中的游戏内连点器。
//
//  本文件 = 原项目 src/MCCombatStatusJni.cpp (V65.1, 与参考实现对齐版)
//  直接删减增补而来, 不是重写:
//    * 注入机制与原项目逐行一致: gdi32!SwapBuffers 12 字节导出存根
//      绝对跳转补丁、PEB 三链表摘链、DllMain 顺序、帧内 GetEnv 复用
//      JNIEnv 的整套状态机 (类加载器探测/171 张映射表/分帧解析) 均未改动。
//    * 删减: 共享内存 (Local\MCCombatStatus_<pid>) 对外发布 —— 状态改为
//      进程内 CombatStatus 实例 (指针用法不变); UDP 早在 V65.1 已移除。
//      防重复注入由原项目的共享内存健康检查改为"检测存根是否已被补丁",
//      不创建任何新的命名内核对象。
//    * 增补 (文件尾部与钩子尾部): 游戏内菜单 (Insert, 分层悬浮窗双缓存),
//      连点逻辑, 快捷键与 Toast, 设置 INI。
//
//  版本历史:
//    v1.1 分层悬浮窗双缓存菜单; v1.2 按原连点器 v2.9 UI 重做
//    (可见滑块/档位块/玻璃开关/热键绑定/悬浮提示); v1.3 CPS 一位小数;
//    v1.4 注入机制代码逐行恢复原项目。
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
struct CombatStatus {
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

static CombatStatus g_statusBuf;
static CombatStatus* g_status = &g_statusBuf;

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
// CPS 以 0.1 为单位存储 (与原连点器一致): 5=0.5 CPS, 500=50.0 CPS
static const int kCpsMin10 = 5;     // 0.5 CPS
static const int kCpsMax10 = 500;   // 50.0 CPS (上限与原版一致)

struct Settings {
    int master;      // 总开关 (连点)
    int left;        // 左键连点
    int right;       // 右键连点
    int cpsLeft10;   // 左键 CPS ×10 (5~500, 0.1 步进)
    int cpsRight10;  // 右键 CPS ×10
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
    int dbgDump;     // 调试: 菜单打开时把合成帧存成 BMP (%TEMP%\MCInGameMenu.bmp)
};

// 默认热键: F8 总开关 / F6 左键 / F7 右键 / F9 保持 (F6=117 F7=118 F8=119 F9=120)
// 默认 CPS 200 = 20.0/s
static Settings g_s = { 0, 1, 1, 200, 200, 0, 0, 0, 0,
                        119, 117, 118, 120, 0, 0, 0, 0, 0 };

static int clampCps10(int v)
{
    if (v < kCpsMin10) v = kCpsMin10;
    if (v > kCpsMax10) v = kCpsMax10;
    return v;
}

// CPS 文本: 整数值省略小数位, 否则保留一位小数
static void CpsTextW(int v10, wchar_t* out, int cap)
{
    if (v10 % 10 == 0) swprintf(out, cap, L"%d/s", v10 / 10);
    else swprintf(out, cap, L"%d.%d/s", v10 / 10, v10 % 10);
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
    g_s.cpsLeft10  = clampCps10(g_s.cpsLeft10);
    g_s.cpsRight10 = clampCps10(g_s.cpsRight10);
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
        else if (!strcmp(k, "cpsLeft10")) g_s.cpsLeft10  = ReadInt(v);
        else if (!strcmp(k, "cpsRight10"))g_s.cpsRight10 = ReadInt(v);
        else if (!strcmp(k, "cpsLeft"))   g_s.cpsLeft10  = ReadInt(v) * 10;  // 旧版整数 CPS 迁移
        else if (!strcmp(k, "cpsRight"))  g_s.cpsRight10 = ReadInt(v) * 10;
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
        else if (!strcmp(k, "dbgDump"))   g_s.dbgDump   = ReadInt(v);
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
        "master=%d\nleft=%d\nright=%d\ncpsLeft10=%d\ncpsRight10=%d\n"
        "keep=%d\ngatk=%d\ngplace=%d\ngcursor=%d\n"
        "hotMaster=%d\nhotLeft=%d\nhotRight=%d\nhotKeep=%d\n"
        "hotGatk=%d\nhotGplace=%d\nhotGcursor=%d\ndbgClicks=%d\ndbgDump=%d\n",
        g_s.master, g_s.left, g_s.right, g_s.cpsLeft10, g_s.cpsRight10,
        g_s.keep, g_s.gatk, g_s.gplace, g_s.gcursor,
        g_s.hotMaster, g_s.hotLeft, g_s.hotRight, g_s.hotKeep,
        g_s.hotGatk, g_s.hotGplace, g_s.hotGcursor, g_s.dbgClicks, g_s.dbgDump);
    fclose(f);
}

//--------------------------------------------------------------------------
// 游戏内菜单 (Insert 呼出)
// 渲染: 独立分层悬浮窗 (WS_EX_LAYERED) + 内存 DC 双缓存位图合成,
//       每帧一次 UpdateLayeredWindow 交给 DWM —— 不再往游戏前缓冲上
//       画 GDI, 彻底消除闪烁。
//--------------------------------------------------------------------------
static const int MENU_W = 380;
static const int TITLE_H = 26, INFO_H = 17, ROW_H = 24;
static const int CPS_ROW_H = 46;                        // CPS 行 (含滑块+档位)
static const int ITEM_COUNT = 16;                       // 9 功能项 + 7 热键项
static const int ITEM_TOP = TITLE_H + INFO_H * 2 + 6;
static const int TOOL_W = 230;                          // 悬浮提示区
static const int OVL_W = MENU_W + TOOL_W;

// CPS 滑块几何 (在 CPS 行的第二行)
static const int CPS_TRACK_L = 112;                     // 轨道左
static const int CPS_CHIP_W = 26, CPS_CHIP_GAP = 4;     // 档位块
static const int CPS_CHIP_COUNT = 4;
static const int kCpsChips[CPS_CHIP_COUNT] = { 10, 20, 30, 40 };

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
    L"左键每秒点击次数 (0.5~50, 0.1 步进)；按住左右拖动 / 滚轮 / ←→ 调整",
    L"右键每秒点击次数 (0.5~50, 0.1 步进)；按住左右拖动 / 滚轮 / ←→ 调整",
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

// 行布局: CPS 行高 46 (两行内容), 其余 24
static int RowH(int i)
{
    return (i == IT_CPSL || i == IT_CPSR) ? CPS_ROW_H : ROW_H;
}
static int RowY(int i)
{
    int y = ITEM_TOP;
    for (int k = 0; k < i && k < ITEM_COUNT; ++k) y += RowH(k);
    return y;
}
static int MenuH(void)
{
    return RowY(ITEM_COUNT) + 6 + 36 + 4;
}

static volatile LONG g_menuOpen = 0;
static volatile LONG g_sel      = 0;
static volatile LONG g_hover    = -1;    // 鼠标悬停项 (悬浮提示)
static volatile LONG g_capturing = -1;   // 正在绑定热键的项
static int   g_dragItem = -1;            // 正在滑动的 CPS 项
static int   g_dragX = 0, g_dragVal = 0;
static bool  g_prevInsert = false;
static HWND  g_hwnd       = NULL;
static WNDPROC g_origWndProc = NULL;

// Toast (热键切换/绑定反馈, 与原连点器一致的提示方式)
static wchar_t g_toast[128];
static DWORD g_toastUntil = 0;

static void ShowToast(const wchar_t* text)
{
    wcsncpy(g_toast, text, 127);
    g_toast[127] = 0;
    g_toastUntil = GetTickCount() + 1400;
}

// 热键项 -> 功能项 映射与短名
static const int kHotTarget[7] = { IT_MASTER, IT_LEFT, IT_RIGHT, IT_KEEP,
                                   IT_GATK, IT_GPLACE, IT_CURSOR };
static const wchar_t* const kHotShortName[7] = {
    L"总开关", L"左键连点", L"右键连点", L"保持连点",
    L"能攻击闸门", L"能放置闸门", L"视角闸门",
};

static void VkNameW(int vk, wchar_t* out, int cap);   // 前向声明 (定义在绘制区)

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
static const COLORREF CLR_ACC  = RGB(99, 166, 255);    // 强调色 (v2.9 同款蓝)
static const COLORREF CLR_TRACK = RGB(56, 64, 88);     // 滑块轨道/开关关闭态

// 点击状态 (渲染线程单线程访问)
static bool  g_downL = false, g_downR = false;
static double g_accL = 0, g_accR = 0;
static DWORD g_lastFrame = 0;

static HFONT  g_fTitle = NULL, g_fRow = NULL, g_fDim = NULL;
static HBRUSH g_brBg = NULL, g_brSel = NULL, g_brBorder = NULL, g_brSep = NULL;
static HBRUSH g_brTip = NULL, g_brAcc = NULL, g_brTrack = NULL, g_brKnob = NULL;

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
    case IT_CPSL:   g_s.cpsLeft10  = clampCps10(g_s.cpsLeft10 + 1); break;
    case IT_CPSR:   g_s.cpsRight10 = clampCps10(g_s.cpsRight10 + 1); break;
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
    case IT_CPSL:   g_s.cpsLeft10  = clampCps10(g_s.cpsLeft10 + dir); break;
    case IT_CPSR:   g_s.cpsRight10 = clampCps10(g_s.cpsRight10 + dir); break;
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
    wchar_t kn[40], t[96];
    VkNameW(vk, kn, 40);
    swprintf(t, 96, L"%ls 已绑定 %ls", kHotShortName[item - IT_HOTMASTER], kn);
    ShowToast(t);
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
        bool nowOn = false;
        switch (i) {
        case IT_HOTMASTER: g_s.master = !g_s.master; nowOn = g_s.master != 0; break;
        case IT_HOTLEFT:   g_s.left   = !g_s.left;   nowOn = g_s.left != 0; break;
        case IT_HOTRIGHT:  g_s.right  = !g_s.right;  nowOn = g_s.right != 0; break;
        case IT_HOTKEEP:   g_s.keep   = !g_s.keep;   nowOn = g_s.keep != 0; break;
        case IT_HOTGATK:   g_s.gatk   = !g_s.gatk;   nowOn = g_s.gatk != 0; break;
        case IT_HOTGPLACE: g_s.gplace = !g_s.gplace; nowOn = g_s.gplace != 0; break;
        case IT_HOTCURSOR: g_s.gcursor = !g_s.gcursor; nowOn = g_s.gcursor != 0; break;
        default: return;
        }
        if (i == IT_HOTMASTER && !nowOn) {
            ReleaseClick(true);
            ReleaseClick(false);
            g_accL = g_accR = 0;
        }
        ItemChanged();
        wchar_t t[64];
        swprintf(t, 64, L"%ls %ls", kHotShortName[i - IT_HOTMASTER],
                 nowOn ? L"开" : L"关");
        ShowToast(t);
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
    case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
        // 绑定模式下鼠标中键/侧键也可作为热键 (左右键不允许, 与原版一致)
        if (g_capturing >= 0) {
            int vk = (msg == WM_MBUTTONDOWN) ? VK_MBUTTON
                   : ((HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2);
            BindHotkey((int)g_capturing, vk);
            g_capturing = -1;
        }
        return 0;
    }
    case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONUP:
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
    if (!g_brAcc)    g_brAcc    = CreateSolidBrush(CLR_ACC);
    if (!g_brTrack)  g_brTrack  = CreateSolidBrush(CLR_TRACK);
    if (!g_brKnob)   g_brKnob   = CreateSolidBrush(CLR_WHT);
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
    for (int i = 0; i < ITEM_COUNT; ++i) {
        if (y >= RowY(i) && y < RowY(i) + RowH(i)) return i;
    }
    return -1;
}

// CPS 行内子区域: 0=无 1=滑块轨道 2..5=档位块 (索引+2)
static int CpsHitZone(int item, int x, int y)
{
    if (item != IT_CPSL && item != IT_CPSR) return 0;
    int sl = RowY(item) + 24;
    if (y < sl - 8 || y > sl + 10) return 0;
    int chipsL = MENU_W - 12 - (CPS_CHIP_W + CPS_CHIP_GAP) * CPS_CHIP_COUNT;
    if (x >= chipsL) {
        for (int k = 0; k < CPS_CHIP_COUNT; ++k) {
            int x0 = chipsL + k * (CPS_CHIP_W + CPS_CHIP_GAP);
            if (x >= x0 && x < x0 + CPS_CHIP_W) return 2 + k;
        }
        return 0;
    }
    if (x >= CPS_TRACK_L && x <= chipsL - 6) return 1;
    return 0;
}

// CPS 轨道像素宽度 (与 DrawSlider 一致)
static int CpsTrackW(void)
{
    int chipsL = MENU_W - 12 - (CPS_CHIP_W + CPS_CHIP_GAP) * CPS_CHIP_COUNT;
    return chipsL - 6 - CPS_TRACK_L;
}

// 轨道 x 坐标 -> CPS 值 (0.1 单位, 按比例映射, 与原连点器一致)
static int CpsFromX(int x)
{
    int w = CpsTrackW();
    if (w <= 0) return kCpsMin10;
    return clampCps10(kCpsMin10 + (int)((x - CPS_TRACK_L) * (kCpsMax10 - kCpsMin10) / w));
}

// 悬浮窗消息处理: 点击/拖动滑 CPS/滚轮/悬停
static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (!g_menuOpen) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int item = HitItem(pt.x, pt.y);
        if (item < 0) return 0;
        g_sel = item;
        if (item == IT_CPSL || item == IT_CPSR) {
            int zone = CpsHitZone(item, pt.x, pt.y);
            if (zone >= 2) {                     // 点档位块: 直接设值
                int v = kCpsChips[zone - 2] * 10;
                if (item == IT_CPSL) g_s.cpsLeft10 = v; else g_s.cpsRight10 = v;
                ItemChanged();
            } else if (zone == 1) {              // 点轨道: 跳值并进入拖动
                int v = CpsFromX(pt.x);
                if (item == IT_CPSL) g_s.cpsLeft10 = v; else g_s.cpsRight10 = v;
                ItemChanged();
                g_dragItem = item;
                g_dragX = pt.x;
                g_dragVal = v;
                SetCapture(h);
            }
        } else if (item >= IT_HOTMASTER && item <= IT_HOTCURSOR) {
            g_capturing = item;                  // 点热键行直接进入绑定
        } else {
            ToggleItem();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_menuOpen) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        g_hover = HitItem(pt.x, pt.y);
        if (g_dragItem >= 0) {                    // 滑动调 CPS: 按轨道比例
            int v = CpsFromX(pt.x);
            if (g_dragItem == IT_CPSL) {
                if (g_s.cpsLeft10 != v) { g_s.cpsLeft10 = v; ItemChanged(); }
            } else {
                if (g_s.cpsRight10 != v) { g_s.cpsRight10 = v; ItemChanged(); }
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
            g_s.cpsLeft10 = clampCps10(g_s.cpsLeft10 + (d > 0 ? 1 : -1));  // ±0.1
            ItemChanged();
        } else if (g_hover == IT_CPSR) {
            g_s.cpsRight10 = clampCps10(g_s.cpsRight10 + (d > 0 ? 1 : -1));
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
        0, 0, OVL_W, MenuH(), NULL, NULL, inst, NULL);
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
    bi.bmiHeader.biHeight = -MenuH();        // top-down
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
    g_bitsH = MenuH();
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
            if (c == 0) continue;    // 透明区 (0,0,0) 保持透明, 不能提升 alpha
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

// ---- GDI 圆角/椭圆填充辅助 ----
static void FillRound(HDC dc, const RECT& r, int rad, HBRUSH br)
{
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(dc, br);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}
static void RingRound(HDC dc, const RECT& r, int rad, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}
static void FillEll(HDC dc, const RECT& r, HBRUSH br)
{
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(dc, br);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

// 玻璃开关 (v2.9 GToggle 同款结构: 圆角轨道 + 白色旋钮)
static void DrawSwitch(HDC dc, const RECT& r, bool on)
{
    FillRound(dc, r, 12, on ? g_brAcc : g_brTrack);
    int cy = (r.top + r.bottom) / 2;
    int kr = (r.bottom - r.top) / 2 - 3;
    int kx = on ? r.right - kr - 3 : r.left + kr + 3;
    RECT krr = { kx - kr, cy - kr, kx + kr, cy + kr };
    FillEll(dc, krr, g_brKnob);
}

// CPS 滑块: 轨道 + 强调色填充 + 白色拇指 + 档位块 (value 为 0.1 单位)
static void DrawSlider(HDC dc, int item, int value)
{
    int y = RowY(item) + 24;
    int chipsL = MENU_W - 12 - (CPS_CHIP_W + CPS_CHIP_GAP) * CPS_CHIP_COUNT;
    int trackR = chipsL - 6;
    RECT trk = { CPS_TRACK_L, y - 3, trackR, y + 3 };
    FillRound(dc, trk, 4, g_brTrack);
    int tx = CPS_TRACK_L + (value - kCpsMin10) * (trackR - CPS_TRACK_L) / (kCpsMax10 - kCpsMin10);
    if (tx > CPS_TRACK_L + 2) {
        RECT fill = { CPS_TRACK_L, y - 3, tx, y + 3 };
        FillRound(dc, fill, 4, g_brAcc);
    }
    RECT thumb = { tx - 6, y - 6, tx + 6, y + 6 };
    FillEll(dc, thumb, g_brKnob);

    for (int k = 0; k < CPS_CHIP_COUNT; ++k) {
        RECT ch = { chipsL + k * (CPS_CHIP_W + CPS_CHIP_GAP), y - 8,
                    chipsL + k * (CPS_CHIP_W + CPS_CHIP_GAP) + CPS_CHIP_W, y + 8 };
        if (kCpsChips[k] * 10 == value) FillRound(dc, ch, 6, g_brAcc);
        else RingRound(dc, ch, 6, CLR_BRD);
        wchar_t lb[8];
        swprintf(lb, 8, L"%d", kCpsChips[k]);
        SelectObject(dc, g_fDim);
        SetTextColor(dc, (kCpsChips[k] == value) ? CLR_WHT : CLR_DIM);
        DrawTextW(dc, lb, -1, (RECT*)&ch, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void ComposeFrame(void)
{
    if (!EnsureBackbuffer()) return;
    memset(g_bits, 0, (size_t)g_bitsW * 4 * g_bitsH);   // 全透明
    HDC dc = g_memDC;
    EnsureUiObjects();

    int mh = MenuH();
    RECT panel = { 0, 0, MENU_W, mh };
    FillRound(dc, panel, 10, g_brBg);
    RingRound(dc, panel, 10, CLR_BRD);
    SetBkMode(dc, TRANSPARENT);

    // 标题 + 强调线
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, CLR_WHT);
    RECT rt = { 12, 0, MENU_W - 12, TITLE_H };
    DrawTextW(dc, L"MCInGame 连点器  v1.4", -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rl = { 12, TITLE_H - 1, MENU_W - 12, TITLE_H };
    FillRect(dc, &rl, g_brAcc);

    // 状态行 (绑定热键时显示绑定指引)
    SelectObject(dc, g_fDim);
    SetTextColor(dc, CLR_DIM);
    wchar_t st[256];
    if (g_capturing >= 0) {
        swprintf(st, 256, L"正在绑定「%ls」— 请按下新键… (Esc 取消)",
                 kItemNames[g_capturing]);
    } else if (g_status->ready) {
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
    RECT sep1 = { 8, TITLE_H + INFO_H * 2 + 2, MENU_W - 8, TITLE_H + INFO_H * 2 + 3 };
    FillRect(dc, &sep1, g_brSep);

    // 菜单项
    for (int i = 0; i < ITEM_COUNT; ++i) {
        int ry = RowY(i), rh = RowH(i);
        RECT rr = { 6, ry, MENU_W - 6, ry + rh };
        if (i == g_sel) FillRound(dc, rr, 8, g_brSel);
        else if (i == g_hover) RingRound(dc, rr, 8, CLR_BRD);

        if (i == IT_CPSL || i == IT_CPSR) {
            // CPS 行: 名称 + 值 + 滑块 + 档位
            SelectObject(dc, g_fRow);
            SetTextColor(dc, CLR_TXT);
            RECT rn = { rr.left + 8, ry, 120, ry + 24 };
            DrawTextW(dc, kItemNames[i], -1, &rn, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            int v = (i == IT_CPSL) ? g_s.cpsLeft10 : g_s.cpsRight10;
            wchar_t val[16];
            CpsTextW(v, val, 16);
            SetTextColor(dc, CLR_ACC);
            RECT rv = { MENU_W - 78, ry, MENU_W - 12, ry + 24 };
            DrawTextW(dc, val, -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DrawSlider(dc, i, v);
        } else {
            SelectObject(dc, g_fRow);
            SetTextColor(dc, CLR_TXT);
            RECT rn = { rr.left + 8, rr.top, rr.right - 96, rr.bottom };
            DrawTextW(dc, kItemNames[i], -1, &rn, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (i >= IT_HOTMASTER && i <= IT_HOTCURSOR) {
                // 热键槽
                RECT slot = { MENU_W - 90, ry + 3, MENU_W - 12, ry + rh - 3 };
                if (i == g_capturing) {
                    FillRound(dc, slot, 8, g_brAcc);
                    bool blink = ((GetTickCount() / 300) & 1) != 0;
                    SetTextColor(dc, CLR_WHT);
                    DrawTextW(dc, blink ? L"请按下新键…" : L"…", -1, &slot,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    RingRound(dc, slot, 8, CLR_BRD);
                    wchar_t kn[40];
                    VkNameW(HotkeyOf(i), kn, 40);
                    SetTextColor(dc, HotkeyOf(i) ? CLR_WHT : CLR_DIM);
                    DrawTextW(dc, kn, -1, &slot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            } else {
                // 玻璃开关
                RECT sw = { MENU_W - 52, ry + 4, MENU_W - 12, ry + rh - 4 };
                DrawSwitch(dc, sw, ItemBool(i) != 0);
            }
        }
    }

    // 分隔线
    RECT sep2 = { 8, RowY(ITEM_COUNT) + 2, MENU_W - 8, RowY(ITEM_COUNT) + 3 };
    FillRect(dc, &sep2, g_brSep);

    // 底部提示
    SelectObject(dc, g_fDim);
    SetTextColor(dc, CLR_DIM);
    int fy = RowY(ITEM_COUNT) + 6;
    RECT rf1 = { 12, fy, MENU_W - 12, fy + 16 };
    DrawTextW(dc, L"Insert/Esc 或点菜单外关闭 · ↑↓ 选择 · ←→/回车 调整", -1, &rf1,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT rf2 = { 12, fy + 16, MENU_W - 12, fy + 32 };
    DrawTextW(dc, L"拖动滑块调 CPS · 点档位快速设值 · 热键项回车绑定", -1, &rf2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 悬浮提示 (鼠标悬停时, 显示在面板右侧)
    if (g_hover >= 0 && g_hover < ITEM_COUNT) {
        int ty = RowY(g_hover);
        if (ty + 46 > mh) ty = mh - 48;
        RECT tr = { MENU_W + 8, ty, OVL_W - 8, ty + 46 };
        FillRound(dc, tr, 8, g_brTip);
        RingRound(dc, tr, 8, CLR_BRD);
        SetTextColor(dc, CLR_TXT);
        RECT tt = { tr.left + 6, tr.top + 3, tr.right - 6, tr.bottom - 3 };
        DrawTextW(dc, kItemTips[g_hover], -1, &tt,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    ApplyAlpha();
}

// Toast 合成 (菜单关闭时, 热键切换/绑定的提示条)
static void ComposeToast(void)
{
    if (!EnsureBackbuffer()) return;
    memset(g_bits, 0, (size_t)g_bitsW * 4 * 36);
    HDC dc = g_memDC;
    EnsureUiObjects();
    RECT tr = { 0, 2, OVL_W, 34 };
    FillRound(dc, tr, 12, g_brBg);
    RingRound(dc, tr, 12, CLR_ACC);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fRow);
    SetTextColor(dc, CLR_TXT);
    RECT tt = { 10, 2, OVL_W - 10, 34 };
    DrawTextW(dc, g_toast, -1, &tt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ApplyAlpha();
}

// 调试: 把合成帧存成 BMP (视觉检查用)
static void DbgDumpFrame(void)
{
    if (!g_bits) return;
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return;
    strcpy(path + strlen(path), "MCInGameMenu.bmp");
    FILE* f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)(g_bitsW * 4 * g_bitsH);
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = g_bitsW;
    ih.biHeight = -g_bitsH;      // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(g_bits, 1, fh.bfSize - fh.bfOffBits, f);
    fclose(f);
}

// 每帧: 菜单打开 -> 菜单悬浮窗; 菜单关闭 -> Toast 提示条; 都没有 -> 隐藏
static void UpdateOverlay(void)
{
    bool toastOn = !g_menuOpen && g_toast[0] && GetTickCount() < g_toastUntil;
    if (!g_menuOpen && !toastOn) {
        if (g_ovl && IsWindowVisible(g_ovl)) ShowWindow(g_ovl, SW_HIDE);
        return;
    }
    if (!g_hwnd || IsIconic(g_hwnd)) {
        if (g_ovl && IsWindowVisible(g_ovl)) ShowWindow(g_ovl, SW_HIDE);
        return;
    }
    if (!EnsureOverlay() || !EnsureBackbuffer()) return;

    int w = OVL_W, h;
    POINT tl;
    if (g_menuOpen) {
        h = MenuH();
        POINT p = { 10, 10 };
        ClientToScreen(g_hwnd, &p);
        tl = p;
        ComposeFrame();
        if (g_s.dbgDump) DbgDumpFrame();
    } else {
        h = 36;
        RECT wr;
        GetClientRect(g_hwnd, &wr);
        POINT p = { wr.right / 2 - w / 2, 8 };
        ClientToScreen(g_hwnd, &p);
        tl = p;
        ComposeToast();
    }
    SetWindowPos(g_ovl, HWND_TOPMOST, tl.x, tl.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BLENDFUNCTION bf = { AC_SRC_ALPHA, 0, 255, 0 };
    POINT src = { 0, 0 };
    SIZE sz = { w, h };
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

static void EmitClicks(double& acc, int cps10, bool left, DWORD dt)
{
    double interval = 10000.0 / cps10;            // 0.1 CPS 精度: 毫秒间隔含小数
    if (interval < 1.0) interval = 1.0;
    acc += (double)dt;
    if (acc > interval * 3.0) acc = interval * 3.0;   // 防长时间暂停后爆发
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
    if (lActive) EmitClicks(g_accL, g_s.cpsLeft10,  true,  dt);
    if (rActive) EmitClicks(g_accR, g_s.cpsRight10, false, dt);
}

//--------------------------------------------------------------------------
// 追加失败原因到 failLog (每轮解析开始前清空)
//--------------------------------------------------------------------------
static void AppendFail(const char* s)
{
    if (!g_status || !s) return;
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
    // 斜杠名转点分名
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
    return c; // 可能为 NULL (未加载)
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
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L1:find-Launch");
    jclass launch = env->FindClass("net/minecraft/launchwrapper/Launch");
    if (!launch) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L2:Launch-ok");
    jfieldID cl = env->GetStaticFieldID(launch, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;");
    if (!cl) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "L3:field-ok");
    jobject loader = env->GetStaticObjectField(launch, cl);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg),
             loader ? "L4:loader-ok" : "L4:loader-null");
    return loader; // 可能为 NULL (未设置)
#endif
}

static jobject FindThreadClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_THREAD_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
    jclass threadCls = env->FindClass("java/lang/Thread");
    if (!threadCls) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
    jmethodID getAll = env->GetStaticMethodID(threadCls, "getAllStackTraces",
                                              "()Ljava/util/Map;");
    if (!getAll) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T2:getAll");
    jobject map = env->CallStaticObjectMethod(threadCls, getAll);
    if (env->ExceptionCheck() || !map) { env->ExceptionClear(); return NULL; }
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T3:map");

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
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:keySet");
    jobject it  = env->CallObjectMethod(set, iterator);
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T5:iterator");
    if (env->ExceptionCheck() || !it) { env->ExceptionClear(); return NULL; }

    DWORD tStart = GetTickCount();
    jobject found = NULL;
    jobject found2 = NULL; // 后备 loader (只能加载混淆名等非标准类)
    while (env->CallBooleanMethod(it, hasNext)) {
        if (GetTickCount() - tStart > 10000) break; // 10 秒超时保护
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T6:hasNext");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject thread = env->CallObjectMethod(it, next);
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T7:next");
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jobject loader = env->CallObjectMethod(thread, getCtx);
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T8:getCtx");
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        // 测试这个加载器能否加载游戏主类。
        // 注意: 验证标准不能太弱——AppClassLoader 若 classpath 混入其他版本
        // 残留 jar (如能加载 1.8.9 的 ave), 会被误选。优先要求能加载
        // MCP/Mojang 名 net/minecraft/client/Minecraft (1.8.9~1.20.1 通用);
        // 只有全部 loader 都不行时, 才接受能加载其他 mcClass 的 loader。
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, clsCls, forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            found = loader;      // 首选: 能加载标准 Minecraft 类
            break;
        }
        // 后备: 能加载其他候选 (如 1.8.9 混淆名 ave)
        for (int i = 0; i < kGenMapCount && !found2; ++i) {
            if (strcmp(kGenMaps[i].mcClass, "net/minecraft/client/Minecraft") == 0)
                continue; // 已测过
            jclass probe = LoadClass(env, kGenMaps[i].mcClass, loader, clsCls, forName);
            if (probe) {
                env->DeleteLocalRef(probe);
                found2 = loader;
                break;
            }
        }
    }
    if (!found) found = found2; // 无首选时用后备
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T9:done");
    if (env->ExceptionCheck()) env->ExceptionClear();
    return found;
#endif // NO_THREAD_LOADER
}

static jobject FindGameClassLoader(JNIEnv* env, jclass clsCls, jmethodID forName)
{
#ifdef NO_GAME_LOADER
    (void)env; (void)clsCls; (void)forName;
    return NULL;
#else
    // 1. Launch.classLoader —— 但必须先验证它能加载游戏类!
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
        // 不能加载游戏类 -> 记录后走 app loader
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg),
                               "E4:launch-loader-cant-load-game");
        env->DeleteLocalRef(loader);
    }
    // 2. 线程遍历: 1.17+ ModLauncher 环境的主线程 context loader
    //    = TransformingClassLoader (游戏类加载器), 带 10 秒超时保护
    loader = FindThreadClassLoader(env, clsCls, forName);
    if (loader) return loader;
    // 3. 系统类加载器 (app loader, -cp 一定有游戏 jar)
    if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "E5:system-loader");
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    if (!clCls) { env->ExceptionClear(); return NULL; }
    jmethodID getSys = env->GetStaticMethodID(clCls, "getSystemClassLoader",
                                              "()Ljava/lang/ClassLoader;");
    if (!getSys) { env->ExceptionClear(); return NULL; }
    jobject sys = env->CallStaticObjectMethod(clCls, getSys);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return sys; // 可能为 NULL (理论不会)
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
    if (!g_status) return;

    if (env->PushLocalFrame(32) < 0) {
        g_status->lastError = 100; // 本地引用栈溢出
        return;
    }
    g_status->lastError = 0;

    jobject mc = env->CallStaticObjectMethod(r.mcClass, r.getMinecraft);
    if (env->ExceptionCheck()) {
        // 记录异常类名, 便于诊断 (如双份类副本 / 类初始化失败)
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
        // 游戏尚未初始化 (主类未加载 / 双份类副本问题)
        g_status->inGame    = 0;
        g_status->canAttack = 0;
        g_status->canPlace  = 0;
        g_status->placeReady = 0;
        g_status->heldItemNull = 0;
        g_status->hitType   = 0;
        g_status->mcNull    = 1;   // 标记: getMinecraft() 拿不到对象
        g_status->tick++;          // 即使拿不到主类也计数, 便于判断 worker 是否存活
        env->PopLocalFrame(NULL);
        return;
    }
    g_status->mcNull = 0;

    jobject player = env->GetObjectField(mc, r.thePlayerField);
    jobject mop    = env->GetObjectField(mc, r.mopField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 102; }

    g_status->inGame = (player != NULL);

    if (!player) {
        // 未进入游戏 (无玩家): 全部清零 (canPlace 也依赖 player, 无法计算)
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

    // ---- 放置物判定: 手持物品是否为 ItemBlock/BlockItem ----
    // 只依赖 player (手持), 与准星/mop 无关; 在 mop 检查之前计算。
    // 链: player.getHeldItem()/getMainHandItem() -> stack.getItem() -> instanceof itemBlockCls
    // 可选解析: 任一 ID 为 NULL (解析失败) 时 canPlace 恒 0, 不影响 canAttack。
    g_status->placeReady = r.placeOk ? 1 : 0; // 诊断: 放置物链是否解析成功
    LONG canPlace    = 0;
    LONG heldNull    = 1; // 默认空手
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
                // 手持物品的 Item 类名 (如 ItemBlock / yo / cds), 便于调试
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
        // 未瞄准: canAttack=0, 瞄准相关字段清零; canPlace/heldNull 保留 (独立检测)
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

    // 命中类型: 0=miss 1=block 2=entity (字段方式或 getter 方式)
    jobject typeObj = r.typeOfHitGetter
        ? env->CallObjectMethod(mop, r.typeOfHitGetter)
        : env->GetObjectField(mop, r.typeOfHitField);
    jobject entConst = env->GetStaticObjectField(r.typeClass, r.entityConstField);
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 103; }

    int hit = 0;
    if (typeObj) {
        if (entConst && env->IsSameObject(typeObj, entConst)) hit = 2;
        else hit = 1; // BLOCK 或 MISS
    }
    g_status->hitType = hit;

    // 命中实体: 仅当命中实体时读取 (getter 方式下必须保证对象是 EntityHitResult)
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

        // 目标类名 (如 EntityZombie / pr / bfj), 便于调试
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

    // 当前手持物品是否允许攻击 (仅 1.8.9 体系有; 空手/武器=true, 食物=false)
    LONG canUseItem = 1;
    if (r.canAttackWithItem) {
        canUseItem = env->CallBooleanMethod(player, r.canAttackWithItem) ? 1 : 0;
        if (env->ExceptionCheck()) { env->ExceptionClear(); g_status->lastError = 107; }
    }

    // 目标是否可被攻击 (现代版本替代 canAttackWithItem 的检查)
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

// JVMTI 终极方案: 枚举所有已加载的类, 找到"真 Minecraft 类"
// (A()/getMinecraft 返回非 null 的那份 —— 无论它由哪个加载器加载,
//  规避所有双份类副本问题)
// (以下函数与原项目逐行一致, 供诊断/兜底使用)
static jclass FindRealMinecraft(JNIEnv* env, JavaVM* vm,
                                const char* clsSig,     // 如 "Lave;"
                                const char* getterName, // 如 "A" (可为 NULL)
                                const char* getterSig)  // 如 "()Lave;"
{
    jvmtiEnv* jvmti = NULL;
    if (vm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_2) != JNI_OK || !jvmti) return NULL;

    jint count = 0;
    jclass* classes = NULL;
    if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return NULL;

    // 候选 getter: 用户指定的优先, 再加常见别名
    const char* gNames[8];
    const char* gSigs[8];
    int gN = 0;
    if (getterName) { gNames[gN] = getterName; gSigs[gN] = getterSig; gN++; }
    const char* extraN[] = { "A", "func_71410_x", "getMinecraft", "getInstance" };
    const char* extraS[] = { "()Lave;", "()Lave;",
                             "()Lnet/minecraft/client/Minecraft;",
                             "()Lnet/minecraft/client/Minecraft;" };
    for (int i = 0; i < 4 && gN < 8; ++i) {
        bool dup = false;
        for (int j = 0; j < gN; ++j) if (strcmp(gNames[j], extraN[i]) == 0) { dup = true; break; }
        if (!dup) { gNames[gN] = extraN[i]; gSigs[gN] = extraS[i]; gN++; }
    }

    jclass found = NULL;
    for (jint i = 0; i < count && !found; ++i) {
        char* sig = NULL;
        if (jvmti->GetClassSignature(classes[i], &sig, NULL) != JVMTI_ERROR_NONE) continue;
        bool match = (clsSig && sig && strcmp(sig, clsSig) == 0);
        if (!match && clsSig == NULL) match = (sig != NULL); // 不匹配类名时全扫
        if (sig) jvmti->Deallocate((unsigned char*)sig);
        if (!match) continue;

        // 方法 1: 调用 getter 返回非 null -> 真类
        for (int k = 0; k < gN && !found; ++k) {
            jmethodID m = env->GetStaticMethodID(classes[i], gNames[k], gSigs[k]);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (!m) continue;
            jobject inst = env->CallStaticObjectMethod(classes[i], m);
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (inst) {
                env->DeleteLocalRef(inst);
                found = classes[i];
                break;
            }
        }
        // 方法 2: 静态字段 S/theMinecraft 非 null -> 真类
        if (!found) {
            const char* fNames[] = { "S", "theMinecraft" };
            for (int k = 0; k < 2 && !found; ++k) {
                char fSig[128];
                snprintf(fSig, sizeof(fSig), "L%s;", clsSig ? clsSig + 1 : "ave");
                // 去掉尾部 ';'
                size_t fl = strlen(fSig);
                if (fl > 0 && fSig[fl-1] == ';') fSig[fl-1] = 0;
                jfieldID f = env->GetStaticFieldID(classes[i], fNames[k], fSig);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (!f) continue;
                jobject v = env->GetStaticObjectField(classes[i], f);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (v) {
                    env->DeleteLocalRef(v);
                    found = classes[i];
                    break;
                }
            }
        }
    }
    jvmti->Deallocate((unsigned char*)classes);
    return found;
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
    // 优先: "versions" 目录后的版本号 (最可靠, 各启动器通用)
    const char* p = text;
    while ((p = strstr(p, "versions")) != NULL) {
        const char* s = p + 8;
        while (*s == '\\' || *s == '/' || *s == ' ' || *s == '"') s++;
        if (s[0] == '1' && s[1] == '.') { start = s; break; }
        p += 8;
    }
    // 回退: 路径组件开头就是 "1." 的版本号
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
    while (n > 0 && out[n - 1] == '.') out[--n] = 0;  // 去尾点
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
        // 名字末尾的连续数字段长度必须 == 版本 tag 长度 (精确匹配, 防后缀撞名)
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
static BYTE   g_patch[32];
static DWORD  g_patchLen = 0;
static LONG   g_attached = 0;      // DllMain 幂等 (防重复 LoadLibrary 二次钩)

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
            // 极端 OOM: 回滚已提升的引用, 调用方放弃本轮
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
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T0:start");
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
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T1:threadCls");
        jobject map = env->CallStaticObjectMethod(g_scanThreadCls, g_scanGetAll);
        if (env->ExceptionCheck() || !map) { env->ExceptionClear(); ScanAbort(env); return 0; }
        jobject set = env->CallObjectMethod(map, keySet);
        jobject it  = set ? env->CallObjectMethod(set, iterator) : NULL;
        if (env->ExceptionCheck() || !it) { env->ExceptionClear(); ScanAbort(env); return 0; }
        g_scanIt = ToGlobal(env, it);
        if (!g_scanIt) return -1;
        g_probeIdx = 0;
        if (g_status) CopyName(g_status->errMsg, sizeof(g_status->errMsg), "T4:iterator");
    }

    while (GetTickCount() < deadline) {
        if (!env->CallBooleanMethod(g_scanIt, g_scanHasNext)) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ScanAbort(env);
            return 0;   // 遍历完: 未找到
        }
        jobject thread = env->CallObjectMethod(g_scanIt, g_scanNext);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!thread) continue;
        jobject loader = env->CallObjectMethod(thread, g_scanGetCtx);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!loader) continue;
        // 首选: 能加载标准 Minecraft 类
        jclass mcProbe = LoadClass(env, "net/minecraft/client/Minecraft",
                                   loader, g_clsCls, g_forName);
        if (mcProbe) {
            env->DeleteLocalRef(mcProbe);
            g_gameLoader = ToGlobal(env, loader);
            g_useGameLoader = true;
            ScanAbort(env);
            return 1;
        }
        // 后备: 能加载其他 mcClass 候选 (分帧, 进度存 g_probeIdx)
        while (g_probeIdx < kGenMapCount) {
            if (GetTickCount() >= deadline) return -1;   // 下帧继续探测当前 loader
            if (strcmp(kGenMaps[g_probeIdx].mcClass, "net/minecraft/client/Minecraft") == 0) {
                g_probeIdx++;   // 首选已测过
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
        g_probeIdx = 0; // 该 loader 全部候选测完, 换下一个线程
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
    if (!g_status) return;

    switch (g_stage) {
    // ---- ST_CLS: java/lang/Class + forName ----
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
    } /* fallthrough: 同帧继续 */
    // ---- ST_ENV: 环境探测 ----
    case ST_ENV: {
#ifndef NO_ENV_DETECT
        DetectEnv(env, NULL, g_clsCls, g_forName,
                  g_status->envName, sizeof(g_status->envName));
#endif
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H2:env-ok");
        g_stage = ST_VER;
    } /* fallthrough: 同帧继续 */
    // ---- ST_VER: classpath 版本号提取 ----
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
    } /* fallthrough: 同帧继续 */
    // ---- ST_LAUNCH: Launch.classLoader ----
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
    // ---- ST_LAUNCH_V: 验证 launch loader 能加载游戏类 ----
    // 一次性预算较大: 只在启动期跑一次, 全部失败才放弃 (OptiFine 下它只含库)
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
    // ---- ST_SCAN: 线程遍历找游戏类加载器 (分帧可续) ----
    case ST_SCAN: {
        DWORD deadline = now + kFrameBudgetMs;
        int rc = ThreadScanStep(env, now, deadline);
        if (rc == 1) {
            CopyName(g_status->loaderName, sizeof(g_status->loaderName),
                     "thread-loader");
            g_stage = ST_SYS;
        } else if (rc == 0) {
            g_stage = ST_SYS;   // 未找到: 用系统类加载器
        }
        // rc == -1: 预算用完, 下帧继续
        return;
    }
    // ---- ST_SYS: 系统类加载器 + 版本指纹 + loaderName ----
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
        // 版本指纹: classpath 提取失败时, 用原版混淆 mcClass 指纹扫一遍
        int start = (g_verHint >= 0) ? g_verHint
                  : DetectVersionHint(env, g_useGameLoader ? g_gameLoader : g_sysLoader,
                                      g_clsCls, g_forName, now + kLaunchVerifyBudgetMs);
        // 环境感知优化: 按探测到的 mod 加载器直接定位命名空间表,
        // 跳过逐张失败的全表轮询 (NeoForge 的 CCNFE 异常路径极慢)。
        // 解析失败仍会向后轮询 + wrap 全表, 回退逻辑不变。
        if (g_verHint >= 0 && g_status->envName[0]) {
            const char* ns = NULL;
            if (strstr(g_status->envName, "neoforge")) ns = "mojang";          // NeoForge 1.20.2+: 全 Mojang
            else if (strstr(g_status->envName, "fabric")) ns = "intermediary"; // Fabric: intermediary
            else if (strstr(g_status->envName, "forge")) ns = "forge";         // Forge: MCP+SRG / Mojang+stable
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
    } /* fallthrough: 同帧开始尝试映射表 */
    // ---- ST_MAPS: 尝试映射表 (分帧) ----
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
                // 终极验证: getMinecraft() 必须返回真实例, 排除双份类副本
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
                    return; // 极端 OOM: 放弃本轮, 下帧同表重试
                }
                NoteErr(m.name, "getMinecraft()=null(副本)");
                ok = false;
            }
            // 当前加载器失败/副本时, 同表换另一个加载器再试 (TCL 与 app loader 都覆盖)
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
                        return; // 极端 OOM: 放弃本轮, 下帧同表重试
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
    // ---- ST_FIX: findLoadedClass 终极修正 (一次性, 几个 JNI 调用无需分帧) ----
    case ST_FIX: {
        const JniMap& mOk = *g_resMap;
#ifndef NO_REAL_FIX
        // 终极修正 A: findLoadedClass 拿游戏已加载的真类, 用它的类加载器重新解析
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
        Log("ready: map=%s env=%s ver=%s loader=%s",   // 本项目追加: 就绪日志
            g_status->mappingName, g_status->envName,
            g_gameVer[0] ? g_gameVer : "?", g_status->loaderName);
        g_stage = ST_STEADY;
        return;
    }
    // ---- ST_STEADY: 采样 + 上报 (5ms 节流) ----
    case ST_STEADY: {
        if (g_status->mcNull) {
            if (!g_nullSince) g_nullSince = now;
            else if (now - g_nullSince >= 500) {
                // 连续 0.5 秒拿不到 mc (双份类副本): 切换加载器重新解析
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
            // UDP 已移除: 状态仅经共享内存发布
            if (g_s.dbgClicks) {   // 本项目追加: 调试日志 (状态变化)
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
// gdi32!SwapBuffers 钩子 (与原项目逐行一致; 仅在其后追加本项目的
// 连点/菜单逻辑: ClickTick / UpdateOverlay, 以及菜单热键轮询)
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

    // 惰性获取 JavaVM (JNI_GetCreatedJavaVMs 不附着线程, 不触发 ThreadStart)
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
        // 复用调用线程已有的 JNIEnv (Client thread 是游戏自建的 Java 线程,
        // 天然 attached)。绝不 AttachCurrentThread —— 非 Java 线程调用时
        // GetEnv 返回 EDETACHED, 直接跳过本帧。
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
            if (g_status && g_stage == ST_CLS) {
                // 诊断 (仅解析前覆盖, 不干扰后续阶段信息)
                snprintf(g_status->errMsg, sizeof(g_status->errMsg), "HG:getenv=%d", (int)rc);
            }
        }
    } else if (g_status && g_stage == ST_CLS) {
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "HV:no-vm");
    }

    // ---- 以下为本项目追加 (原项目没有): 连点 + 游戏内菜单 ----
    ClickTick(hdc);

    BOOL r = g_origSwapBuffers ? g_origSwapBuffers(hdc) : FALSE;

    UpdateOverlay();
    return r;
}

//--------------------------------------------------------------------------
// 内联钩子 (x64): 5 字节 rel32 jmp (或 12 字节 mov rax,imm64;jmp rax) +
// 附近分配的 trampoline (原指令 + jmp back)。
// 以下代码与原项目 MCCombatStatusJni.cpp (V65.1) 逐行一致。
//--------------------------------------------------------------------------
static bool FitsRel32(void* from, void* to)
{
    INT64 d = (INT64)((BYTE*)to - (BYTE*)from);
    return d >= -0x80000000LL && d <= 0x7FFFFFFFLL;
}

static bool WriteRelJmp(BYTE* dst, void* to)
{
    if (!FitsRel32(dst, to)) return false;
    dst[0] = 0xE9;
    INT32 off = (INT32)((BYTE*)to - (dst + 5));
    memcpy(dst + 1, &off, 4);
    return true;
}

static void WriteAbsJmp(BYTE* dst, void* to)
{
    dst[0] = 0x48; dst[1] = 0xB8;               // mov rax, imm64
    memcpy(dst + 2, &to, 8);
    dst[10] = 0xFF; dst[11] = 0xE0;             // jmp rax
}

// modrm 长度 (modrm 位于 p[i]); rm==4 时任何 mod 下都存在 SIB 字节
static int ModRmLen(const BYTE* p, int i)
{
    BYTE modrm = p[i];
    int mod = modrm >> 6, rm = modrm & 7;
    int len = 1;
    if (rm == 4) {                                   // SIB
        len += 1;
        if (mod == 0 && (p[i + 1] & 7) == 5) len += 4;
    }
    if (mod == 0 && rm == 5)      len += 4;          // disp32 (无 SIB)
    else if (mod == 1)            len += 1;          // disp8
    else if (mod == 2)            len += 4;          // disp32
    return len;
}

// 最小 x86-64 长度反汇编器: 只覆盖 Windows 函数序言常见指令。
// 返回指令字节数, 无法识别返回 0。
static int InsnLen64(const BYTE* p)
{
    int i = 0;
    bool rexW = false;
    while (i < 15) {
        BYTE b = p[i];
        if (b >= 0x40 && b <= 0x4F) { rexW = (b & 8) != 0; i++; continue; }
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        break;
    }
    if (i >= 15) return 0;
    BYTE op = p[i];
    if (op == 0x0F) {
        BYTE op2 = p[i + 1];
        if (op2 >= 0x80 && op2 <= 0x8F) return i + 6;                 // jcc rel32
        if (op2 == 0x1E || op2 == 0x1F) return i + 2 + ModRmLen(p, i + 2); // nop (含 endbr64)
        if (op2 == 0x05 || op2 == 0x34) return i + 2;                 // syscall/sysenter
        return 0;
    }
    if (op >= 0x50 && op <= 0x5F) return i + 1;                       // push/pop r64
    if (op >= 0x70 && op <= 0x7F) return i + 2;                       // jcc rel8
    if (op == 0xEB) return i + 2;                                     // jmp rel8
    if (op == 0xE9) return i + 5;                                     // jmp rel32
    if (op == 0xE8) return i + 5;                                     // call rel32
    if (op >= 0xB8 && op <= 0xBF) return i + 1 + (rexW ? 8 : 4);      // mov r, imm
    if (op == 0x68) return i + 5;                                     // push imm32
    if (op == 0x6A) return i + 2;                                     // push imm8
    if (op == 0x80) return i + 2 + ModRmLen(p, i + 1) + 1;            // group1 r/m8, imm8
    if (op == 0x81) return i + 2 + ModRmLen(p, i + 1) + 4;            // group1 r/m, imm32
    if (op == 0x83) return i + 2 + ModRmLen(p, i + 1) + 1;            // group1 r/m, imm8
    if (op == 0xC7) return i + 2 + ModRmLen(p, i + 1) + 4;            // mov r/m, imm32
    if (op == 0x89 || op == 0x8B || op == 0x8D || op == 0x03 || op == 0x0B ||
        op == 0x2B || op == 0x33 || op == 0x3B || op == 0x01 || op == 0x09 ||
        op == 0x85 || op == 0x39 || op == 0x31 || op == 0x29 || op == 0x23 ||
        op == 0x63 || op == 0x8F || op == 0x21 || op == 0x87 || op == 0x86)
        return i + 1 + ModRmLen(p, i + 1);
    if (op == 0xFF) return i + 1 + ModRmLen(p, i + 1);                 // call/jmp r/m
    if (op == 0xC3) return i + 1;                                     // ret
    if (op == 0xC2) return i + 3;                                     // ret imm16
    if (op == 0xCC) return i + 1;                                     // int3
    if (op == 0x90) return i + 1;                                     // nop
    return 0;
}

// 追跳存根链到真实函数体:
//   gdi32!SwapBuffers 等系统导出常以 FF 25 disp32 (jmp [rip+disp32]) 或
//   E9 rel32 开头 (跳转存根)。若直接钩存根, trampoline 原样复制含 RIP
//   相对寻址的指令会因地址偏移而跳飞。必须追到真实函数体再打补丁。
static BYTE* ResolveRealEntry(BYTE* entry)
{
    for (int hop = 0; hop < 8 && entry; ++hop) {
        if (entry[0] == 0xFF && entry[1] == 0x25) {          // jmp [rip+disp32]
            INT32 disp;
            memcpy(&disp, entry + 2, 4);
            BYTE** slot = (BYTE**)(entry + 6 + disp);        // 槽在导出者自身数据段内
            entry = *slot;
            continue;
        }
        if (entry[0] == 0xE9) {                              // jmp rel32
            INT32 disp;
            memcpy(&disp, entry + 1, 4);
            entry = entry + 5 + disp;
            continue;
        }
        return entry;   // 真实函数体
    }
    return NULL;
}

// 检查已解码窗口内是否存在需要重定位的指令
// (相对跳转/调用, 或 mod=00 rm=101 的 RIP 相对寻址) —— 蹦床原样复制
// 这类指令会因地址偏移而跳飞, 一律拒绝内联补丁。
static bool InsnNeedsReloc(const BYTE* p, int len)
{
    if (len <= 0) return true;
    // 跳过指令前缀
    int i = 0;
    while (i < len) {
        BYTE b = p[i];
        if ((b >= 0x40 && b <= 0x4F) || b == 0x66 || b == 0x67 ||
            b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        break;
    }
    if (i >= len) return true;
    BYTE op = p[i];
    if (op == 0xE8 || op == 0xE9 || op == 0xEB ||
        (op >= 0x70 && op <= 0x7F) ||
        (op == 0x0F && i + 1 < len && p[i+1] >= 0x80 && p[i+1] <= 0x8F) ||
        (op == 0xFF && i + 1 < len && (p[i+1] & 0xC7) == 0x25))
        return true;
    if (i + 1 < len) {
        BYTE modrm = p[i + 1];
        if ((modrm >> 6) == 0 && (modrm & 7) == 5) return true;  // [rip+disp32]
    }
    return false;
}

// 模块映像大小 (PE 头解析, 无需 psapi)
static DWORD ModuleSizeOf(HMODULE m)
{
    BYTE* base = (BYTE*)m;
    if (!base) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

// 安装 gdi32!SwapBuffers 钩子 (与参考实现完全一致的装法):
// gdi32 导出入口是 "FF 25 rel32 + 6 字节 CC 填充" 的存根 —— 直接把整段
// 12 字节覆盖为绝对跳转 (mov rax, imm64; jmp rax) 指向本模块钩子。
// 真实函数地址从存根的槽位读出; 槽位若未解析, 读到的解析器在首次调用
// 时会自行解析并尾跳到真实实现 (槽位我们没动, 行为安全)。
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
    patch[0] = 0x48; patch[1] = 0xB8;              // mov rax, imm64
    void* hookAddr = (void*)(void(*)())&HookSwapBuffers;
    memcpy(patch + 2, &hookAddr, 8);
    patch[10] = 0xFF; patch[11] = 0xE0;            // jmp rax

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

__declspec(dllexport) BOOL WINAPI GetStatus(CombatStatus* out)
{
    if (!out) return FALSE;
    *out = *g_status;
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
// DLL 入口 (与原项目 V65.1 同顺序, 仅做删减与增补):
//   原项目:  DllMain -> g_attached -> HideModuleFromPeb -> 共享内存健康
//            检查(防重复注入) -> 共享内存创建 -> 钩子安装
//   本项目:  DllMain -> g_attached -> HideModuleFromPeb -> 存根补丁检查
//            (防重复注入, 替代共享内存健康检查, 不创建任何新内核对象)
//            -> 状态初始化 -> [追加: 设置加载/菜单事件] -> 钩子安装
//--------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        if (InterlockedExchange(&g_attached, 1)) return TRUE; // 防重复 LoadLibrary 二次初始化
        g_hInst = hInst;

        // 与参考实现一致: 从 PEB 模块链表摘除自身 (模块枚举不可见)
        HideModuleFromPeb(hInst);

        // 防重复注入 (替代原项目的共享内存健康检查):
        // 若 gdi32!SwapBuffers 存根已被补丁 (48 B8 开头), 说明本 DLL 已在运行
        {
            HMODULE gdi = GetModuleHandleA("gdi32.dll");
            if (gdi) {
                BYTE* stub = (BYTE*)(void*)GetProcAddress(gdi, "SwapBuffers");
                if (stub && stub[0] == 0x48 && stub[1] == 0xB8) return TRUE;
            }
        }

        memset(g_status, 0, sizeof(*g_status));
        g_status->magic   = kMagic;
        g_status->version = kVersion;
        CopyName(g_status->errMsg, sizeof(g_status->errMsg), "H0:dllmain-init");

        // ---- 以下为本项目追加 (原项目没有): 设置加载 + 菜单命令事件 ----
        LoadSettings();
        g_lastFrame = GetTickCount();
        g_menuEvent = CreateEventA(NULL, TRUE, FALSE, "Local\\MCInGameMenuEvent");

        // 钩住 gdi32!SwapBuffers (游戏渲染线程每帧调用)
        if (!InstallSwapBuffersHook()) {
            Log("dllmain: init, peb-hidden=%d, HOOK-INSTALL-FAIL",
                PebStillVisible(hInst) ? 0 : 1);
        } else {
            Log("dllmain: init, peb-hidden=%d, hook-ok, settings master=%d left=%d right=%d cps=%d.%d/%d.%d keep=%d gates=%d/%d/%d",
                PebStillVisible(hInst) ? 0 : 1,
                g_s.master, g_s.left, g_s.right,
                g_s.cpsLeft10 / 10, g_s.cpsLeft10 % 10,
                g_s.cpsRight10 / 10, g_s.cpsRight10 % 10,
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
        // 钩子有意不还原: DLL 与进程同生命周期, 退出阶段还原补丁
        // 会与其他仍在执行的线程竞态, 无意义且有崩溃风险。
    }
    return TRUE;
}
