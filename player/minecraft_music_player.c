/* ==========================================================================
 *  minecraft_music_player.c  ——  Minecraft 音乐播放器  by ZeroRaltal
 *
 *  支持格式: .cbnd / .nbs / .mid / .midi
 *  依赖: stb_vorbis.c, samples.h
 *  编译:
 *      gcc -O2 -o minecraft_music_player.exe minecraft_music_player.c app.res -lcomctl32 -lcomdlg32 -lshell32 -lwinhttp -lwinmm -lgdi32 -luser32 -lm -mwindows  
 * ========================================================================== */

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <wctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

#include "stb_vorbis.c"
#include "samples.h"

#define MY_DWMWA_USE_IMMERSIVE_DARK_MODE             20
#define MY_DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#define MY_DWMWA_CAPTION_COLOR                       35
#define MY_DWMWA_TEXT_COLOR                          36

typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
typedef BOOL    (WINAPI *PFN_SetProcessDPIAware)(void);
typedef BOOL    (WINAPI *PFN_EnableMouseInPointer)(BOOL);

static PFN_DwmSetWindowAttribute g_pDwmSet   = NULL;
static PFN_SetWindowTheme        g_pSetTheme = NULL;

static void init_optional_apis(void)
{
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) g_pDwmSet = (PFN_DwmSetWindowAttribute)(void *)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    HMODULE hUx = LoadLibraryW(L"uxtheme.dll");
    if (hUx) g_pSetTheme = (PFN_SetWindowTheme)(void *)GetProcAddress(hUx, "SetWindowTheme");
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        PFN_SetProcessDPIAware pDpi = (PFN_SetProcessDPIAware)(void *)GetProcAddress(hUser, "SetProcessDPIAware");
        if (pDpi) pDpi();

        /* Win8+：把触摸/笔输入转成鼠标消息，让所有鼠标逻辑自动支持触摸 */
        PFN_EnableMouseInPointer pEnable = (PFN_EnableMouseInPointer)
            (void *)GetProcAddress(hUser, "EnableMouseInPointer");
        if (pEnable) pEnable(TRUE);
    }
}

#define WM_APP_STATUS_SET     (WM_APP + 1)
#define WM_APP_PLAYLIST_READY (WM_APP + 2)
#define WM_APP_PLAY_DONE      (WM_APP + 3)
#define WM_APP_SET_WF         (WM_APP + 4)
#define WM_APP_ERROR          (WM_APP + 5)

#define IDC_LIST       1001
#define IDC_PROGRESS   1002
#define IDC_SPEED      1003
#define IDC_PAUSE      1004
#define IDC_SEARCH     1005
#define IDC_MODES      1006
#define IDC_OPENFILE   1007
#define IDC_HELPTIP       1008

/* ===== 布局 ===== */
#define TOPBAR_H      40
#define SEARCH_AREA_H 56
#define CTRL_H        100
#define LEFT_MIN      360
#define LEFT_MAX      500
#define RIGHT_MIN     320

#define CARD_H        56
#define OUT_RATE      44100
#define MIX_FRAMES    512
#define MAX_VOICES    96
#define ATTACK_SAMPLES 88
#define TIME_W        100
#define SCROLLBAR_W   14
#define LISTVIEW_EXTRA_W   (-SCROLLBAR_W)
#define LISTVIEW_EXTRA_H   200
/* WF_WINDOW_TICKS 已改为全局变量 g_wfWindowTicks */

/* ===== 黑黄主题配色 ===== */
#define C_BG        RGB(0x0d, 0x0d, 0x0d)
#define C_BG_HEADER RGB(0x00, 0x00, 0x00)
#define C_BG_CTRL   RGB(0x00, 0x00, 0x00)
#define C_BG_INPUT  RGB(0x2a, 0x2a, 0x2a)
#define C_CARD      RGB(0x1a, 0x1a, 0x1a)
#define C_CARD_HOV  RGB(0x2a, 0x2a, 0x2a)
#define C_CARD_PLAY RGB(0x2a, 0x2a, 0x1a)
#define C_ACCENT    RGB(0xd4, 0xa0, 0x20)
#define C_ACCENT_HI RGB(0xff, 0xe1, 0x00)
#define C_TEXT      RGB(0xff, 0xff, 0xff)
#define C_TEXT_SUB  RGB(0xf0, 0xd0, 0x60)
#define C_MUTED     RGB(0xa0, 0x90, 0x50)
#define C_TRACK     RGB(0x2a, 0x2a, 0x2a)
#define C_WF_GRID   RGB(0x2a, 0x2a, 0x2a)
#define C_WF_BG     RGB(0x0d, 0x0d, 0x0d)

#define MODE_ORDER   0
#define MODE_RANDOM  1
#define MODE_SINGLE  2

static HWND g_hMain = NULL, g_hViewport = NULL, g_hList = NULL,
            g_hProgress = NULL, g_hSpeed = NULL, g_hPause = NULL,
            g_hSearch = NULL, g_hModes = NULL, g_hOpenFile = NULL,
            g_hWaterfall = NULL, g_hHelp = NULL, g_hHelpOverlay = NULL;

static HFONT g_fontTitle = NULL, g_fontNormal = NULL,
             g_fontBold = NULL, g_fontSmall = NULL;

static HBRUSH g_searchBgBrush = NULL;

static volatile LONG g_stopRequest  = 0;
static volatile LONG g_seekTick     = -1;
static volatile LONG g_speedMilli   = 1000;
static volatile LONG g_currentTick  = 0;
static volatile LONG g_currentTickF = 0;   /* 千分之一 tick 的浮点进度，用于瀑布图平滑 */
static volatile LONG g_totalTicks   = 0;
static volatile LONG g_playingState = 0;
static volatile LONG g_currentGen   = 0;
static volatile LONG g_paused       = 0;
static volatile LONG g_totalSec     = 0;
static volatile LONG g_curSec       = 0;

static int  g_scrollPos = 0, g_scrollTarget = 0, g_scrollContentH = 0;
static int  g_scrollViewH = 0, g_scrollViewW = 0, g_scrollTimerOn = 0;
static int  g_sbDragging = 0, g_sbDragOffset = 0;

/* 倒放 */
static volatile LONG g_reverse = 0;
/* 按住右键之前的速度（松开时恢复） */
static double g_savedSpeed = 1.0;
/* 瀑布图窗口 tick 数（可缩放） */
static int g_wfWindowTicks = 32;
/* 错误重试动作 */
typedef struct {
    int   type;    /* 0=无, 1=重试歌单, 2=重试在线歌曲 */
    char *url;
} RetryAction;
static RetryAction g_retry = {0, NULL};

static double g_progValue = 0.0, g_progDragVal = 0.0;
static int    g_progDragging = 0;
static DWORD  g_lastSeekTime = 0;
static double g_speedVal = 1.0;
static int    g_sliderDrag = 0;
static int    g_hoverIdx = -1;
static int    g_playingIdx = -1;

static int  g_playMode = MODE_ORDER;

static wchar_t g_statusText[256] = L"正在加载...";
static wchar_t g_timeText[64]    = L"0:00 / 0:00";
static wchar_t g_currentPlayingName[256] = L"";

/* 错误提示 */
static wchar_t g_errorMsg[512] = L"";
static int     g_showError = 0;

/* 帮助面板 */
static int g_showHelp = 0;

static const wchar_t *HELP_LINES[] = {
    L"【 键盘快捷键 】",
    L"    空格键 ......... 播放 / 暂停",
    L"    ← 长按 ......... 倒放",
    L"    → 长按 ......... 两倍速",
    L"    ↑ / ↓ .......... 上一首 / 下一首",
    L"    Esc ............ 关闭弹窗",
    L"",
    L"【 鼠标操作 】",
    L"    单击卡片 ........ 播放该歌曲",
    L"    拖动右侧滑块 .... 滚动列表",
    L"    拖动进度条 ...... 跳转",
    L"    Ctrl + 滚轮 ..... 缩放瀑布图",
    L"    拖放文件 ........ 拖 cbnd/nbs/mid 到窗口直接播放",
    L"",
    L"【 搜索框 】",
    L"    输入文字 ........ 按名称搜索",
    L"    输入 #数字 ......... 跳到编号 {数字} 的歌曲",
    L"",
    L"【 播放模式 】",
    L"    顺序  ⇉ ......... 顺序播放",
    L"    随机  ⇆ ......... 随机播放",
    L"    单曲  ↻ ......... 单曲循环",
    L"",
    L"【 支持的格式 】",
    L"    .cbnd  本项目压缩序列格式",
    L"    .nbs   Note Block Studio 乐谱",
    L"    .mid / .midi  标准 MIDI 文件",
    L"",
    L"【 歌曲获取源 】",
    L"    https://github.com/Zero-Raltal/MCBE-CBNoteBlock/tree/main/songs",
    NULL
};

static RECT g_statusRect = {0,0,0,0};
static RECT g_timeRect   = {0,0,0,0};

typedef struct { char *name, *url, *ext; } SongEntry;
static SongEntry *g_songs = NULL;
static size_t     g_songCount = 0;
static int       *g_filteredIdx = NULL;
static int        g_filteredCount = 0;
static wchar_t    g_searchText[64] = L"";

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdr[4];
    int16_t  buf[4][MIX_FRAMES * 2];
    int      cur;
} AudioDev;

static AudioDev g_audioDev;
static int      g_audioDevOpen = 0;

/* ===== 音符与歌曲 ===== */
typedef struct { uint8_t instrument, key, velocity; uint32_t tick; } Note;
typedef struct { int tempo; Note *notes; size_t n, cap; uint32_t max_tick; } Song;

static Note  *g_wfNotes = NULL;
static size_t g_wfNotesN = 0;
static int    g_wfMinKey = 0, g_wfMaxKey = 127;

static const COLORREF WF_COLORS[16] = {
    RGB(0x3C,0x6B,0x9A), RGB(0x53,0x83,0x59), RGB(0xA0,0x6D,0x6E), RGB(0xAD,0xAD,0x3C),
    RGB(0x99,0x6B,0x98), RGB(0x7C,0x59,0x50), RGB(0xA5,0xA1,0x67), RGB(0xB1,0x3C,0xB1),
    RGB(0x65,0x8F,0x99), RGB(0xD0,0xD0,0xD0), RGB(0x3C,0x8C,0xAB), RGB(0xB1,0x44,0x47),
    RGB(0xB0,0x68,0x47), RGB(0x3C,0xB1,0x3C), RGB(0xA5,0x3C,0x64), RGB(0x69,0x69,0x69)
};

/* ===== 列表末尾的链接行 ===== */
#define INFO_ROW_COUNT 3
static const wchar_t *INFO_URLS[3] = {
    L"https://github.com/Zero-Raltal/MCBE-CBNoteBlock/tree/main/songs",
    L"https://github.com/Zero-Raltal/MCBE-CBNoteBlock/",
    L"https://zero-raltal.github.io/MCBE-CBNoteBlock/"
};
static const wchar_t *INFO_LABELS[3] = {
    L"歌单源：https://github.com/Zero-Raltal/MCBE-CBNoteBlock/tree/main/songs",
    L"项目仓库：https://github.com/Zero-Raltal/MCBE-CBNoteBlock/",
    L"音乐生成器：https://zero-raltal.github.io/MCBE-CBNoteBlock/"
};

/* 瀑布图音符排序：先按 tick，再按 key */
static int cmp_wf_note(const void *a, const void *b)
{
    const Note *x = (const Note *)a;
    const Note *y = (const Note *)b;
    if (x->tick != y->tick) return (x->tick < y->tick) ? -1 : 1;
    if (x->key  != y->key)  return (x->key  < y->key)  ? -1 : 1;
    return 0;
}

/* ==========================================================================
 *  斜条纹矩形：用于显示同一 tick 同一音高的多个重叠音符
 * ========================================================================== */
static void draw_striped_rect(HDC hdc, int x, int y, int w, int h,
                              const COLORREF *colors, int n)
{
    if (n <= 0 || w <= 0 || h <= 0) return;

    /* 单色直接填满 */
    if (n == 1) {
        HBRUSH br = CreateSolidBrush(colors[0]);
        RECT nr = { x, y, x + w, y + h };
        FillRect(hdc, &nr, br);
        DeleteObject(br);
        return;
    }

    int saved = SaveDC(hdc);

    /* 严格裁剪到音符块矩形 */
    HRGN clip = CreateRectRgn(x, y, x + w, y + h);
    SelectClipRgn(hdc, clip);
    DeleteObject(clip);

    /* 逐行扫描填充：
       对每个像素 (px, py)，斜条纹索引 = (px + py) * n / (w + h)
       因为 (px + py) 的等值线就是 -45° 斜线。 */
    double denom = (double)(w + h);
    for (int py = 0; py < h; py++) {
        int runStart = 0;
        int runIdx = -1;

        for (int px = 0; px <= w; px++) {
            int idx = -1;
            if (px < w) {
                double t = (double)(px + py);
                idx = (int)(t * n / denom);
                if (idx < 0) idx = 0;
                if (idx >= n) idx = n - 1;
            }

            /* 遇到颜色变化（或行尾）→ 输出上一段 */
            if (px == w || idx != runIdx) {
                if (runIdx >= 0 && px > runStart) {
                    HBRUSH br = CreateSolidBrush(colors[runIdx % n]);
                    RECT seg = { x + runStart, y + py, x + px, y + py + 1 };
                    FillRect(hdc, &seg, br);
                    DeleteObject(br);
                }
                runStart = px;
                runIdx = idx;
            }
        }
    }

    RestoreDC(hdc, saved);
}

/* 颜色插值：t=0 → c1，t=1 → c2 */
static COLORREF lerp_color(COLORREF c1, COLORREF c2, double t)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    int r1 = GetRValue(c1), g1 = GetGValue(c1), b1 = GetBValue(c1);
    int r2 = GetRValue(c2), g2 = GetGValue(c2), b2 = GetBValue(c2);
    int r = (int)(r1 + (r2 - r1) * t + 0.5);
    int g = (int)(g1 + (g2 - g1) * t + 0.5);
    int b = (int)(b1 + (b2 - b1) * t + 0.5);
    return RGB(r, g, b);
}


/* ==========================================================================
 *  工具
 * ========================================================================== */
typedef struct { uint8_t *data; size_t len, cap; } Buf;
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void u8_to_w_buf(const char *s, wchar_t *dst, int n)
{
    if (!s) s = "";
    MultiByteToWideChar(CP_UTF8, 0, s, -1, dst, n);
}
static wchar_t *u8_to_w(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void title_to_wide(const char *src, wchar_t *dst, int dst_cap)
{
    dst[0] = 0;
    if (!src || dst_cap <= 0) return;
    int r = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_cap);
    int suspicious = 0;
    if (r <= 2) suspicious = 1;
    if (!suspicious && r > 1) {
        for (int k = 0; k < r - 1 && k < dst_cap - 1; k++)
            if (dst[k] == 0xFFFD) { suspicious = 1; break; }
    }
    if (!suspicious) return;
    {
        const wchar_t *w = (const wchar_t *)src;
        wchar_t tmp[256];
        int i = 0;
        while (i < 255 && w[i] != 0) {
            if (w[i] < 0x20) break;
            if (w[i] >= 0xD800 && w[i] <= 0xDFFF) break;
            tmp[i] = w[i];
            i++;
        }
        if (i >= 2) {
            tmp[i] = 0;
            int k;
            for (k = 0; k < i && k < dst_cap - 1; k++) dst[k] = tmp[k];
            dst[k] = 0;
            return;
        }
    }
    if (r <= 0) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_cap);
}
static void post_status_w(HWND hwnd, const wchar_t *msg)
{
    size_t n = wcslen(msg) + 1;
    wchar_t *c = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!c) return;
    wcscpy(c, msg);
    PostMessage(hwnd, WM_APP_STATUS_SET, 0, (LPARAM)c);
}
static void post_status(HWND hwnd, const char *utf8)
{
    wchar_t *w = u8_to_w(utf8);
    if (w) PostMessage(hwnd, WM_APP_STATUS_SET, 0, (LPARAM)w);
}

/* ==========================================================================
 *  错误提示
 * ========================================================================== */
static void show_error(const wchar_t *msg)
{
    if (!msg) msg = L"发生未知错误";
    wcsncpy(g_errorMsg, msg, 511);
    g_errorMsg[511] = 0;
    g_showError = 1;
    if (g_hMain) InvalidateRect(g_hMain, NULL, FALSE);
}
static void clear_error(void)
{
    g_showError = 0;
    if (g_hMain) InvalidateRect(g_hMain, NULL, FALSE);
}
/* 从后台线程投递错误（宽字符版） */
static void post_error(HWND hwnd, const wchar_t *msg)
{
    if (!hwnd || !msg) return;
    size_t n = wcslen(msg) + 1;
    wchar_t *copy = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!copy) return;
    wcscpy(copy, msg);
    PostMessage(hwnd, WM_APP_ERROR, 0, (LPARAM)copy);
}
/* 从后台线程投递错误（UTF-8 版） */
static void post_error_utf8(HWND hwnd, const char *utf8)
{
    wchar_t *w = u8_to_w(utf8);
    if (w) PostMessage(hwnd, WM_APP_ERROR, 0, (LPARAM)w);
}
static int note_push(Song *s, Note nt)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 2048;
        Note *p = (Note *)realloc(s->notes, nc * sizeof(Note));
        if (!p) return -1;
        s->notes = p; s->cap = nc;
    }
    s->notes[s->n++] = nt;
    if (nt.tick > s->max_tick) s->max_tick = nt.tick;
    return 0;
}
static int cmp_note(const void *a, const void *b)
{
    const Note *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return 0;
}

/* ==========================================================================
 *  HTTP GET
 * ========================================================================== */
#define SONGS_BASE  "https://zero-raltal.github.io/MCBE-CBNoteBlock/songs/"
#define SONGS_LIST  SONGS_BASE "songs.list"

static int http_get_ex(const char *url, Buf *out, HWND hwnd, const wchar_t *label)
{
    int is_https; const char *p;
    if (strncmp(url, "https://", 8) == 0)      { is_https = 1; p = url + 8; }
    else if (strncmp(url, "http://", 7) == 0)  { is_https = 0; p = url + 7; }
    else return -1;

    char host[256];
    const char *path = "/";
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t hl = (size_t)(slash - p);
        if (hl >= sizeof(host)) return -1;
        memcpy(host, p, hl); host[hl] = '\0';
        path = slash;
    } else snprintf(host, sizeof(host), "%s", p);

    wchar_t whost[256], wpath[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256)) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096)) return -1;

    HINTERNET hs = WinHttpOpen(L"cbnd_player/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return -1;
    WinHttpSetTimeouts(hs, 15000, 15000, 60000, 60000);

    HINTERNET hc = WinHttpConnect(hs, whost,
        (INTERNET_PORT)(is_https ? INTERNET_DEFAULT_HTTPS_PORT
                                 : INTERNET_DEFAULT_HTTP_PORT), 0);
    if (!hc) { WinHttpCloseHandle(hs); return -1; }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hr = WinHttpOpenRequest(hc, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return -1; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hr, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    int rc = -1;
    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hr, NULL)) goto cleanup;

    DWORD st = 0, sl = sizeof(st);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
    if (st != 200) goto cleanup;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail)) break;
        if (avail == 0) { rc = 0; break; }
        if (out->len + avail > out->cap) {
            size_t nc = out->cap ? out->cap : 8192;
            while (nc < out->len + avail) nc *= 2;
            uint8_t *nb = (uint8_t *)realloc(out->data, nc);
            if (!nb) break;
            out->data = nb; out->cap = nc;
        }
        DWORD rd = 0;
        if (!WinHttpReadData(hr, out->data + out->len, avail, &rd)) break;
        out->len += rd;

        if (hwnd && label && out->len >= 32768) {
            static volatile LONG s_lastKb = 0;
            LONG curKb = (LONG)(out->len / 1024);
            if (curKb - s_lastKb >= 32) {
                InterlockedExchange(&s_lastKb, curKb);
                wchar_t buf[128];
                swprintf(buf, 128, L"%ls %.1f KB", label, out->len / 1024.0);
                post_status_w(hwnd, buf);
            }
        }
    }
cleanup:
    WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); WinHttpCloseHandle(hs);
    return rc;
}

static int http_get(const char *url, Buf *out)
{
    return http_get_ex(url, out, NULL, NULL);
}

/* ==========================================================================
 *  CBND 解析
 * ========================================================================== */
typedef struct { const uint8_t *d; size_t nbits, pos; } BitReader;

static int br_read(BitReader *br, int n, uint32_t *out)
{
    if (br->pos + (size_t)n > br->nbits) return -1;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = br->pos >> 3;
        int bit = 7 - (int)(br->pos & 7);
        v = (v << 1) | ((br->d[byte] >> bit) & 1u);
        br->pos++;
    }
    *out = v; return 0;
}

static int parse_cbnd(const uint8_t *data, size_t size, Song *s)
{
    memset(s, 0, sizeof(*s));
    if (size < 4) return -1;
    BitReader br = { data, size * 8, 0 };
    uint32_t v;
    if (br_read(&br, 8, &v) < 0) return -1;
    s->tempo = (int)v; if (s->tempo <= 0) s->tempo = 20;
    if (br_read(&br, 16, &v) < 0) return -1;
    if (v != 0) return -1;
    int done = 0;
    while (!done) {
        uint32_t instr, key;
        if (br_read(&br, 4, &instr) < 0) break;
        if (br_read(&br, 7, &key)   < 0) break;
        for (;;) {
            if (br_read(&br, 16, &v) < 0) { done = 1; break; }
            if (v == 0xFFFFu)             { done = 1; break; }
            if (v == 0)                     break;
            Note nt;
            nt.instrument = (uint8_t)(instr & 0x0F);
            nt.key        = (uint8_t)(key & 0x7F);
            nt.velocity   = 100;   /* CBND 无力度概念，默认满力度 */
            nt.tick       = v - 1;
            if (note_push(s, nt) < 0) return -1;
        }
    }
    return 0;
}

/* ==========================================================================
 *  NBS 解析器
 * ========================================================================== */
typedef struct { const uint8_t *buf; size_t len, pos; } Reader;

static int r_u8 (Reader *r, uint8_t  *out) { if (r->pos + 1 > r->len) return -1; *out = r->buf[r->pos++]; return 0; }
static int r_u16(Reader *r, uint16_t *out) { if (r->pos + 2 > r->len) return -1; memcpy(out, r->buf + r->pos, 2); r->pos += 2; return 0; }
static int r_i32(Reader *r, int32_t  *out) { if (r->pos + 4 > r->len) return -1; memcpy(out, r->buf + r->pos, 4); r->pos += 4; return 0; }

static char *r_str(Reader *r)
{
    int32_t n;
    if (r_i32(r, &n) < 0 || n < 0) return NULL;
    if (r->pos + (size_t)n > r->len) return NULL;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, r->buf + r->pos, (size_t)n);
    s[n] = 0;
    r->pos += (size_t)n;
    return s;
}

static int parse_nbs(const uint8_t *data, size_t size, Song *song)
{
    memset(song, 0, sizeof(*song));
    Reader r = { data, size, 0 };

    uint16_t first;
    if (r_u16(&r, &first) < 0) return -1;

    int version;
    if (first == 0) {
        uint8_t ver, inst_count;
        if (r_u8(&r, &ver) < 0) return -1;
        if (r_u8(&r, &inst_count) < 0) return -1;
        version = ver;
        if (version >= 3) { uint16_t sl; if (r_u16(&r, &sl) < 0) return -1; }
    } else {
        version = 1;
    }
    if (version > 6) return -1;

    uint16_t layer_count = 0;
    if (r_u16(&r, &layer_count) < 0) return -1;

    char *song_name   = r_str(&r);
    char *song_author = r_str(&r);
    char *song_orig   = r_str(&r);
    char *song_desc   = r_str(&r);
    free(song_author); free(song_orig); free(song_desc); free(song_name);

    uint16_t tempo100;
    if (r_u16(&r, &tempo100) < 0) return -1;
    song->tempo = (tempo100 + 50) / 100;
    if (song->tempo <= 0) song->tempo = 10;

    uint8_t tmp8;
    if (r_u8(&r, &tmp8) < 0) return -1;
    if (r_u8(&r, &tmp8) < 0) return -1;
    if (r_u8(&r, &tmp8) < 0) return -1;
    int32_t tmp32;
    for (int i = 0; i < 5; i++)
        if (r_i32(&r, &tmp32) < 0) return -1;
    char *imp = r_str(&r); free(imp);

    if (version >= 4) {
        if (r_u8(&r, &tmp8) < 0) return -1;
        if (r_u8(&r, &tmp8) < 0) return -1;
        uint16_t tmp16;
        if (r_u16(&r, &tmp16) < 0) return -1;
    }

    int32_t tick = -1;
    for (;;) {
        int16_t jump;
        if (r_u16(&r, (uint16_t *)&jump) < 0) return -1;
        if (jump == 0) break;
        tick += jump;
        int32_t layer = -1;
        for (;;) {
            int16_t layerJump;
            if (r_u16(&r, (uint16_t *)&layerJump) < 0) return -1;
            if (layerJump == 0) break;
            layer += layerJump;
            uint8_t inst, key;
            if (r_u8(&r, &inst) < 0) return -1;
            if (r_u8(&r, &key)  < 0) return -1;
            uint8_t velocity = 100;
            int16_t pitch = 0;
            if (version >= 4) {
                if (r_u8(&r, &velocity) < 0) return -1;
                if (r_u8(&r, &tmp8) < 0) return -1;
                if (r_u16(&r, (uint16_t *)&pitch) < 0) return -1;
            }
            (void)layer; (void)pitch;
            if (key <= 87) {
                Note nt;
                /* NBS 自定义乐器 ID ≥ 16 → 转成 harp */
                nt.instrument = (inst < 16) ? inst : 0;
                nt.key        = (uint8_t)(key + 9);
                nt.velocity   = (velocity > 0) ? velocity : 100;
                nt.tick       = (uint32_t)(tick < 0 ? 0 : tick);
                note_push(song, nt);
            }
        }
    }

    if (version > 0) {
        for (int i = 0; i < layer_count; i++) {
            char *ln = r_str(&r); free(ln);
            if (version >= 4) { uint8_t flags; if (r_u8(&r, &flags) < 0) return -1; }
            uint8_t vol;
            if (r_u8(&r, &vol) < 0) return -1;
            if (version >= 2) { if (r_u8(&r, &tmp8) < 0) return -1; }
        }
    }

    uint8_t ci_count = 0;
    if (r_u8(&r, &ci_count) < 0) return song->n > 0 ? 0 : -1;
    for (int i = 0; i < ci_count; i++) {
        char *n1 = r_str(&r); free(n1);
        char *n2 = r_str(&r); free(n2);
        uint8_t k; if (r_u8(&r, &k) < 0) break;
        uint8_t p; if (r_u8(&r, &p) < 0) break;
    }
    return song->n > 0 ? 0 : -1;
}

/* ==========================================================================
 *  MIDI 解析器
 * ========================================================================== */
static uint32_t midi_varint(const uint8_t *p, size_t *used)
{
    uint32_t v = 0; size_t i = 0; uint8_t b;
    do { if (i > 4) break; b = p[i]; v = (v << 7) | (b & 0x7F); i++; } while (b & 0x80);
    *used = i;
    return v;
}

static const uint8_t GM_TO_MC[128] = {
0,15,15,15,0,0,5,14,7,7,7,10,10,9,7,5,6,10,6,6,6,6,6,6,5,5,0,5,1,12,12,5,
1,1,1,1,5,5,1,15,6,6,6,6,6,1,0,3,6,6,6,6,6,6,6,3,6,6,6,12,6,12,12,6,
6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,13,6,6,6,5,6,6,1,7,6,6,6,6,6,6,8,
8,6,8,5,15,6,6,5,14,14,14,5,10,6,6,6,8,11,10,9,2,3,3,8,4,6,8,6,7,2,3,3
};
static const int8_t GM_OCT[128] = {
0,0,0,0,0,0,12,0,-24,-24,-24,0,0,-24,-24,12,-12,0,-12,-12,-12,-12,-12,-12,
12,12,0,12,24,24,24,36,24,24,24,24,12,12,24,0,-12,-12,-12,-12,-12,24,0,0,
-12,-12,-12,-12,-12,-12,-12,-12,-12,-12,-12,24,-12,24,24,-12,-12,-12,-12,-12,
-12,-12,-12,-12,-12,-12,-12,-12,-12,-12,-12,-12,0,-12,-12,-12,12,-12,-12,24,
-24,-12,-12,-12,-12,-12,-12,-24,-24,-12,-24,12,0,-12,-12,12,0,0,0,12,0,-12,
-12,-12,-24,-12,0,-24,0,0,0,-24,12,-12,-24,12,24,0,0,0
};
static const uint8_t DRUM_TO_MC[128] = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
13,3,4,3,3,4,4,4,4,4,8,2,2,4,3,4,3,2,3,2,3,2,3,2,
2,3,2,3,3,3,4,3,11,3,4,3,4,4,4,2,2,3,3,9,9,4,4,6,
6,4,4,4,4,4,12,12,4,8,3,8,8,4,2,2,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0
};
static const uint8_t DRUM_PITCH[128] = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
39,8,25,18,27,16,13,9,6,2,17,10,6,6,8,6,4,6,22,13,22,15,18,20,
23,17,23,24,8,13,18,18,5,13,2,13,9,2,8,22,15,13,8,12,5,20,23,34,
33,17,11,18,10,5,25,26,16,19,22,6,15,21,14,7,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0
};

typedef struct {
    uint8_t  active;
    uint8_t  noteNumber;
    uint8_t  velocity;
    uint8_t  instrument;
    uint32_t tick;
} ActiveNote;

typedef struct { double sec; uint8_t key; uint8_t vel; uint8_t inst; uint8_t ch; } TempNote;

static int parse_midi(const uint8_t *data, size_t size, Song *song, int enableSnap)
{
    memset(song, 0, sizeof(*song));
    if (size < 14) return -1;
    if (memcmp(data, "MThd", 4) != 0) return -1;

    uint16_t track_count = (data[10] << 8) | data[11];
    uint16_t timeDiv     = (data[12] << 8) | data[13];

    typedef struct { uint32_t tick; uint32_t tempo; } TempoEv;
    TempoEv *tempos = NULL; size_t tempo_n = 0, tempo_cap = 0;
    TempNote *tempNotes = NULL; size_t temp_n = 0, temp_cap = 0;

    size_t off = 14;
    for (int t = 0; t < track_count; t++) {
        if (off + 8 > size) break;
        if (memcmp(data + off, "MTrk", 4) != 0) break;
        uint32_t len = (data[off+4]<<24)|(data[off+5]<<16)|(data[off+6]<<8)|data[off+7];
        size_t track_end = off + 8 + len;
        if (track_end > size) track_end = size;
        size_t p = off + 8;
        uint32_t absTick = 0;
        int runningStatus = -1;
        uint8_t channelInst[16] = {0};
        ActiveNote active[16][128];
        memset(active, 0, sizeof(active));

        while (p < track_end) {
            size_t used;
            uint32_t delta = midi_varint(data + p, &used);
            p += used;
            if (p >= track_end) break;
            absTick += delta;
            uint8_t status = data[p];
            if (status < 0x80) {
                if (runningStatus < 0) break;
                status = (uint8_t)runningStatus;
            } else { p++; runningStatus = status; }

            if (status == 0xFF) {
                if (p >= track_end) break;
                uint8_t metaType = data[p++];
                uint32_t mlen = midi_varint(data + p, &used);
                p += used;
                if (metaType == 0x51 && mlen == 3 && p + 3 <= track_end) {
                    uint32_t tempo = (data[p]<<16) | (data[p+1]<<8) | data[p+2];
                    if (tempo_n == tempo_cap) {
                        tempo_cap = tempo_cap ? tempo_cap * 2 : 16;
                        tempos = (TempoEv *)realloc(tempos, tempo_cap * sizeof(TempoEv));
                    }
                    if (tempos) { tempos[tempo_n].tick = absTick; tempos[tempo_n].tempo = tempo; tempo_n++; }
                }
                p += mlen;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                uint32_t slen = midi_varint(data + p, &used);
                p += used + slen;
                continue;
            }
            if (status >= 0x80 && status < 0xF0) {
                uint8_t type = status >> 4;
                uint8_t ch   = status & 0x0F;
                if (type == 0x8 || type == 0x9) {
                    if (p + 2 > track_end) break;
                    uint8_t note = data[p++];
                    uint8_t vel  = data[p++];
                    if (type == 0x9 && vel > 0) {
                        active[ch][note].active = 1;
                        active[ch][note].noteNumber = note;
                        active[ch][note].velocity = vel;
                        active[ch][note].instrument = channelInst[ch];
                        active[ch][note].tick = absTick;
                    } else {
                        if (active[ch][note].active) {
                            ActiveNote *a = &active[ch][note];
                            if (temp_n == temp_cap) {
                                temp_cap = temp_cap ? temp_cap * 2 : 1024;
                                TempNote *nt = (TempNote *)realloc(tempNotes, temp_cap * sizeof(TempNote));
                                if (!nt) break;
                                tempNotes = nt;
                            }
                            tempNotes[temp_n].sec = (double)a->tick;
                            tempNotes[temp_n].vel = a->velocity;
                            tempNotes[temp_n].ch  = ch;
                            if (ch == 9) {
                                tempNotes[temp_n].inst = DRUM_TO_MC[note];
                                tempNotes[temp_n].key = (uint8_t)(note - 12 + DRUM_PITCH[note]);
                            } else {
                                tempNotes[temp_n].inst = GM_TO_MC[a->instrument];
                                tempNotes[temp_n].key = (uint8_t)(note - 12 + GM_OCT[a->instrument]);
                            }
                            temp_n++;
                            a->active = 0;
                        }
                    }
                } else if (type == 0xC) {
                    if (p + 1 > track_end) break;
                    channelInst[ch] = data[p++];
                } else if (type == 0xA) p += 2;
                else if (type == 0xB) p += 2;
                else if (type == 0xD) p += 1;
                else if (type == 0xE) p += 2;
            } else p++;
        }
        off = track_end;
    }

    for (size_t i = 0; i < tempo_n; i++)
        for (size_t j = i + 1; j < tempo_n; j++)
            if (tempos[j].tick < tempos[i].tick) {
                TempoEv tmp = tempos[i]; tempos[i] = tempos[j]; tempos[j] = tmp;
            }

    double *secs = (double *)malloc(temp_n * sizeof(double));
    if (!secs) { free(tempos); free(tempNotes); return -1; }
    for (size_t i = 0; i < temp_n; i++) {
        uint32_t tick = (uint32_t)tempNotes[i].sec;
        double total = 0.0;
        uint32_t prevTick = 0;
        uint32_t prevTempo = 500000;
        for (size_t k = 0; k < tempo_n; k++) {
            if (tempos[k].tick > tick) break;
            total += (double)(tempos[k].tick - prevTick) * (prevTempo / 1000000.0) / timeDiv;
            prevTick = tempos[k].tick;
            prevTempo = tempos[k].tempo;
        }
        total += (double)(tick - prevTick) * (prevTempo / 1000000.0) / timeDiv;
        secs[i] = total;
    }

    double bestTempo = 20.0;
    if (enableSnap && temp_n > 0) {
        double bestErr = 1e300;
        for (double v = 15.0; v <= 25.0; v += 0.05) {
            double err = 0;
            for (size_t i = 0; i < temp_n; i++) {
                double t = secs[i] * v;
                double d = t - floor(t + 0.5);
                err += d * d;
            }
            if (err < bestErr) { bestErr = err; bestTempo = v; }
        }
    }

    for (size_t i = 0; i < temp_n; i++) {
        Note nt;
        double t = secs[i] * bestTempo;
        nt.tick = (uint32_t)(t + 0.5);
        uint8_t k = tempNotes[i].key;
        if (tempNotes[i].ch == 9) {
            while (k < 42) k += 12;
            while (k > 65) k -= 12;
        }
        nt.key = k;
        nt.instrument = tempNotes[i].inst;
        nt.velocity   = (tempNotes[i].vel > 0) ? tempNotes[i].vel : 100;
        note_push(song, nt);
    }

    song->tempo = (int)(bestTempo + 0.5);
    free(tempos); free(tempNotes); free(secs);
    return song->n > 0 ? 0 : -1;
}

static int parse_any_file(const uint8_t *data, size_t len, const wchar_t *ext, Song *out)
{
    if (!ext) ext = L"";
    if (_wcsicmp(ext, L"cbnd") == 0) return parse_cbnd(data, len, out);
    if (_wcsicmp(ext, L"nbs")  == 0) {
        if (parse_nbs(data, len, out) != 0) return -1;
        if (out->tempo <= 0) out->tempo = 20;
        return 0;
    }
    if (_wcsicmp(ext, L"mid") == 0 || _wcsicmp(ext, L"midi") == 0)
        return parse_midi(data, len, out, 1);
    return parse_cbnd(data, len, out);
}

/* ==========================================================================
 *  采样加载
 * ========================================================================== */
typedef struct { short *pcm; int ch, frames, rate; double base_step; } Sample;
static Sample g_samples[16];
static volatile LONG g_samplesReady = 0;

static DWORD WINAPI LoadSamplesThread(LPVOID param)
{
    (void)param;
    for (int i = 0; i < 16; i++) {
        int ch = 0, sr = 0;
        short *pcm = NULL;
        int n = stb_vorbis_decode_memory(SAMPLE_TABLE[i].data,
                                         (int)SAMPLE_TABLE[i].size,
                                         &ch, &sr, &pcm);
        if (n <= 0 || !pcm) continue;
        g_samples[i].pcm = pcm;
        g_samples[i].ch = ch;
        g_samples[i].frames = n;
        g_samples[i].rate = sr;
        g_samples[i].base_step = (double)sr / OUT_RATE;
    }
    InterlockedExchange(&g_samplesReady, 1);
    return 0;
}

/* ==========================================================================
 *  合成器
 * ========================================================================== */
typedef struct {
    int active, sample_idx;
    double pos, step, gain;
    int attack_count, attack_len;
} Voice;

static Voice g_voices[MAX_VOICES];
static int g_voice_rr = 0;

static void note_on(uint8_t inst, uint8_t key, uint8_t velocity)
{
    if (inst >= 16) return;
    Sample *s = &g_samples[inst];
    if (!s->pcm || s->frames < 2) return;

    int slot = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        int k = (g_voice_rr + i) % MAX_VOICES;
        if (!g_voices[k].active) { slot = k; break; }
    }
    if (slot < 0) slot = g_voice_rr;
    g_voice_rr = (slot + 1) % MAX_VOICES;

    Voice *v = &g_voices[slot];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->sample_idx = inst;
    v->pos = 0.0;
    double playback_rate = pow(2.0, ((double)key - 54.0) / 12.0);
    v->step = s->base_step * playback_rate;

    /* velocity 0~127 → 音量增益系数 0.3~1.0 */
    double volScale = 0.3 + 0.7 * ((double)velocity / 127.0);
    if (volScale > 1.0) volScale = 1.0;
    v->gain = 0.40 * volScale;

    v->attack_count = 0;
    v->attack_len = ATTACK_SAMPLES;
}
static void voice_next(Voice *v, double *outL, double *outR)
{
    Sample *s = &g_samples[v->sample_idx];
    int i0 = (int)v->pos;
    if (i0 >= s->frames - 1) { v->active = 0; *outL = *outR = 0.0; return; }
    double frac = v->pos - i0;
    double m_l, m_r;
    if (s->ch == 1) {
        double a = s->pcm[i0], b = s->pcm[i0 + 1];
        m_l = m_r = (a + (b - a) * frac) / 32768.0;
    } else {
        double a0 = s->pcm[i0*2],     b0 = s->pcm[(i0+1)*2];
        double a1 = s->pcm[i0*2 + 1], b1 = s->pcm[(i0+1)*2 + 1];
        m_l = (a0 + (b0 - a0) * frac) / 32768.0;
        m_r = (a1 + (b1 - a1) * frac) / 32768.0;
    }
    double env = 1.0;
    if (v->attack_count < v->attack_len) {
        env = (double)v->attack_count / v->attack_len;
        v->attack_count++;
    }
    *outL = m_l * v->gain * env;
    *outR = m_r * v->gain * env;
    v->pos += v->step;
}
static double soft_clip(double x)
{
    const double th = 0.80;
    if (x > th)  return  th + (1.0 - th) * tanh((x - th) / (1.0 - th));
    if (x < -th) return -th - (1.0 - th) * tanh((-x - th) / (1.0 - th));
    return x;
}
static int any_voice_active(void)
{
    for (int i = 0; i < MAX_VOICES; i++) if (g_voices[i].active) return 1;
    return 0;
}

/* ==========================================================================
 *  waveOut
 * ========================================================================== */
static int audio_open(AudioDev *d, int rate)
{
    WAVEFORMATEX wf; memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&d->hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return -1;
    memset(d->hdr, 0, sizeof(d->hdr));
    d->cur = 0;
    return 0;
}
static void audio_write(AudioDev *d, const int16_t *s, int frames)
{
    if (g_stopRequest) return;
    WAVEHDR *h = &d->hdr[d->cur];
    if (h->dwFlags & WHDR_PREPARED) {
        while ((h->dwFlags & WHDR_INQUEUE) && !g_stopRequest) Sleep(2);
        if (g_stopRequest) return;
        waveOutUnprepareHeader(d->hwo, h, sizeof(WAVEHDR));
    }
    memcpy(d->buf[d->cur], s, (size_t)frames * 2 * sizeof(int16_t));
    h->lpData         = (LPSTR)d->buf[d->cur];
    h->dwBufferLength = frames * 2 * sizeof(int16_t);
    h->dwFlags        = 0;
    waveOutPrepareHeader(d->hwo, h, sizeof(WAVEHDR));
    waveOutWrite(d->hwo, h, sizeof(WAVEHDR));
    d->cur = (d->cur + 1) % 4;
}
static void audio_close(AudioDev *d)
{
    waveOutReset(d->hwo);
    for (int i = 0; i < 4; i++) {
        WAVEHDR *h = &d->hdr[i];
        if (h->dwFlags & WHDR_PREPARED) {
            while ((h->dwFlags & WHDR_INQUEUE)) Sleep(2);
            waveOutUnprepareHeader(d->hwo, h, sizeof(WAVEHDR));
        }
    }
    waveOutClose(d->hwo);
}

/* ==========================================================================
 *  播放核心
 * ========================================================================== */
static int play_song(const Song *song, HWND hwnd)
{
    if (audio_open(&g_audioDev, OUT_RATE) < 0) {
        post_status(hwnd, "无法打开音频设备");
        return 0;
    }
    g_audioDevOpen = 1;
    InterlockedExchange(&g_paused, 0);

    memset(g_voices, 0, sizeof(g_voices));
    g_voice_rr = 0;
    InterlockedExchange(&g_currentTick, 0);
    InterlockedExchange(&g_currentTickF, 0);
    InterlockedExchange(&g_seekTick, -1);
    InterlockedExchange(&g_playingState, 1);

    if (song->n > 0) {
        Note *copy = (Note *)malloc(song->n * sizeof(Note));
        if (copy) {
            memcpy(copy, song->notes, song->n * sizeof(Note));
            PostMessage(hwnd, WM_APP_SET_WF, (WPARAM)song->n, (LPARAM)copy);
        }
    }

    double pos = 0.0;
    size_t next = 0;
    size_t prev = 0;
    LONG lastReverse = -1;
    int16_t out[MIX_FRAMES * 2];
    int natural_end = 0;

    int global_attack = OUT_RATE * 15 / 1000;
    int global_atk_count = 0;

    double song_tempo = song->tempo > 0 ? (double)song->tempo : 20.0;
    int total_sec = (int)((song->max_tick + 1) / song_tempo);
    InterlockedExchange(&g_totalSec, total_sec);
    int last_sec = -1;

    while (!g_stopRequest) {
        double speed = (double)g_speedMilli / 1000.0;
        double tpf   = song_tempo * speed / OUT_RATE;

        /* 倒放模式切换检测 */
        {
            LONG curRev = g_reverse;
            int rev = (curRev != 0);
            if (curRev != lastReverse) {
                lastReverse = curRev;
                next = 0;
                while (next < song->n && (double)song->notes[next].tick < pos) next++;
                prev = song->n;
                while (prev > 0 && (double)song->notes[prev-1].tick > pos) prev--;
            }

            LONG sk = g_seekTick;
            if (sk >= 0) {
                InterlockedExchange(&g_seekTick, -1);
                if (sk > (LONG)song->max_tick) sk = song->max_tick;
                pos = (double)sk;
                InterlockedExchange(&g_currentTickF, (LONG)(pos * 1000.0));
                for (int i = 0; i < MAX_VOICES; i++) g_voices[i].active = 0;
                next = 0;
                while (next < song->n && (double)song->notes[next].tick < pos) next++;
                prev = song->n;
                while (prev > 0 && (double)song->notes[prev-1].tick > pos) prev--;
                global_atk_count = 0;
            }

            for (int i = 0; i < MIX_FRAMES; i++) {
                if (rev) {
                    while (prev > 0 && (double)song->notes[prev-1].tick >= pos) {
                        prev--;
                        note_on(song->notes[prev].instrument, song->notes[prev].key,
                                song->notes[prev].velocity);
                    }
                } else {
                    while (next < song->n && (double)song->notes[next].tick <= pos) {
                        note_on(song->notes[next].instrument, song->notes[next].key,
                                song->notes[next].velocity);
                        next++;
                    }
                }
                double l = 0.0, r = 0.0;
                for (int vi = 0; vi < MAX_VOICES; vi++) {
                    if (!g_voices[vi].active) continue;
                    double vl, vr;
                    voice_next(&g_voices[vi], &vl, &vr);
                    l += vl; r += vr;
                }
                double ge = 1.0;
                if (global_atk_count < global_attack) {
                    ge = (double)global_atk_count / global_attack;
                    global_atk_count++;
                }
                l = soft_clip(l) * ge;
                r = soft_clip(r) * ge;
                out[i*2]     = (int16_t)(l * 32000.0);
                out[i*2 + 1] = (int16_t)(r * 32000.0);
                if (rev) pos -= tpf;
                else     pos += tpf;
            }
        }

        uint32_t curTick = (uint32_t)pos;
        InterlockedExchange(&g_currentTick, (LONG)curTick);
        InterlockedExchange(&g_currentTickF, (LONG)(pos * 1000.0));

        int sec = (int)((double)curTick / song_tempo);
        if (sec != last_sec) {
            last_sec = sec;
            InterlockedExchange(&g_curSec, sec);
        }

        audio_write(&g_audioDev, out, MIX_FRAMES);
        if (g_stopRequest) break;

        if (g_reverse) {
            if (pos <= 0.0) { natural_end = 1; break; }
        } else {
            if ((double)song->max_tick + 2.0 * song_tempo < pos) { natural_end = 1; break; }
        }
        if (next >= song->n && !any_voice_active() &&
            global_atk_count >= global_attack) { natural_end = 1; break; }
    }

    PostMessage(hwnd, WM_APP_SET_WF, 0, 0);

    audio_close(&g_audioDev);
    g_audioDevOpen = 0;
    InterlockedExchange(&g_playingState, 0);
    return natural_end;
}

/* ==========================================================================
 *  歌单
 * ========================================================================== */
static void free_playlist(SongEntry *arr, size_t n)
{
    if (!arr) return;
    for (size_t i = 0; i < n; i++) {
        free(arr[i].name); free(arr[i].url); free(arr[i].ext);
    }
    free(arr);
}
static SongEntry *fetch_playlist(size_t *out_n)
{
    Buf b = {0};
    if (http_get(SONGS_LIST, &b) < 0 || b.len == 0) { buf_free(&b); return NULL; }

    size_t cap = 64, n = 0;
    SongEntry *arr = (SongEntry *)malloc(cap * sizeof(SongEntry));
    if (!arr) { buf_free(&b); return NULL; }

    char *p = (char *)b.data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(p);
        while (len && (p[len-1]=='\r' || p[len-1]==' ' || p[len-1]=='\t')) p[--len]='\0';
        while (*p==' ' || *p=='\t') p++;
        if (*p) {
            if (n == cap) {
                cap *= 2;
                SongEntry *t = realloc(arr, cap * sizeof(SongEntry));
                if (!t) break;
                arr = t;
            }
            const char *rel = p;
            const char *slash = strrchr(rel, '/');
            const char *base = slash ? slash + 1 : rel;
            const char *dot = strrchr(base, '.');
            char namebuf[256];
            if (dot && dot != base) {
                size_t l = (size_t)(dot - base);
                if (l > sizeof(namebuf) - 1) l = sizeof(namebuf) - 1;
                memcpy(namebuf, base, l); namebuf[l] = '\0';
            } else snprintf(namebuf, sizeof(namebuf), "%s", base);
            size_t urllen = strlen(SONGS_BASE) + strlen(rel) + 1;
            char *url = (char *)malloc(urllen);
            if (!url) break;
            snprintf(url, urllen, "%s%s", SONGS_BASE, rel);
            char extbuf[16] = "";
            if (dot && dot[1]) {
                size_t l = strlen(dot + 1);
                if (l > sizeof(extbuf) - 1) l = sizeof(extbuf) - 1;
                for (size_t k = 0; k < l; k++) extbuf[k] = (char)tolower((unsigned char)dot[1+k]);
                extbuf[l] = '\0';
            }
            arr[n].name = _strdup(namebuf);
            arr[n].url  = url;
            arr[n].ext  = _strdup(extbuf);
            n++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    buf_free(&b);
    *out_n = n;
    return arr;
}
static DWORD WINAPI LoadPlaylistThread(LPVOID param)
{
    HWND hwnd = (HWND)param;
    size_t n = 0;
    SongEntry *list = fetch_playlist(&n);
    PostMessage(hwnd, WM_APP_PLAYLIST_READY, (WPARAM)n, (LPARAM)list);
    return 0;
}

/* ==========================================================================
 *  播放请求线程
 * ========================================================================== */
typedef struct { HWND hwnd; char *url; char *title; LONG gen; } PlayRequest;
typedef struct { HWND hwnd; wchar_t *path; wchar_t *title; LONG gen; } LocalPlayRequest;

static DWORD WINAPI PlayThread(LPVOID param)
{
    PlayRequest *req = (PlayRequest *)param;
    HWND hwnd = req->hwnd;
    LONG myGen = req->gen;

    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) != myGen) {
        free(req->url); free(req->title); free(req);
        return 0;
    }

    post_status(hwnd, "正在下载...");
    Buf b = {0};
    if (http_get_ex(req->url, &b, hwnd, L"正在下载...") < 0 || b.len == 0) {
        buf_free(&b);
        if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen) {
            post_status(hwnd, "下载失败");
            /* 保存重试信息 */
            free(g_retry.url);
            g_retry.type = 2;
            g_retry.url = _strdup(req->url);
            post_error_utf8(hwnd,
                "无法从服务器下载歌曲。\n\n"
                "可能原因：\n"
                "  • 网络连接异常\n"
                "  • github.io 无法访问\n"
                "  • 歌曲地址已变更\n\n"
                "请检查网络或稍后重试。");
        }
        PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, 0);
        free(req->url); free(req->title); free(req);
        return 0;
    }
    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) != myGen) {
        buf_free(&b);
        free(req->url); free(req->title); free(req);
        return 0;
    }

    post_status(hwnd, "正在解析...");
    Song song;
    const char *dot = strrchr(req->url, '.');
    wchar_t extw[16] = L"cbnd";
    if (dot && dot[1]) {
        char ext[16] = "";
        size_t l = strlen(dot+1);
        if (l > 15) l = 15;
        for (size_t k = 0; k < l; k++) ext[k] = (char)tolower((unsigned char)dot[1+k]);
        ext[l] = 0;
        MultiByteToWideChar(CP_UTF8, 0, ext, -1, extw, 16);
    }
    int parsed = parse_any_file(b.data, b.len, extw, &song);
    buf_free(&b);
    if (parsed < 0 || song.n == 0) {
        if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen) {
            post_status(hwnd, "解析失败");
            post_error_utf8(hwnd,
                "歌曲文件损坏或格式不支持。\n\n"
                "支持的格式：\n"
                "  • CBND (.cbnd)\n"
                "  • NBS (.nbs)\n"
                "  • MIDI (.mid/.midi)");
        }
        PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, 0);
        free(req->url); free(req->title); free(req);
        return 0;
    }
    qsort(song.notes, song.n, sizeof(Note), cmp_note);

    if (!g_samplesReady) {
        post_status(hwnd, "等待音源加载...");
        while (!g_samplesReady &&
               InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen &&
               !g_stopRequest) Sleep(50);
    }

    int natural_end = 0;
    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen &&
        !g_stopRequest) {
        InterlockedExchange(&g_totalTicks, (LONG)(song.max_tick + 1));

        wchar_t wtitle[256];
        ZeroMemory(wtitle, sizeof(wtitle));
        title_to_wide(req->title, wtitle, 256);
        wchar_t st[512];
        ZeroMemory(st, sizeof(st));
        swprintf(st, 512, L"正在播放：%ls", wtitle);
        st[511] = 0;
        post_status_w(hwnd, st);

        {
            size_t wl = wcslen(wtitle) + 1;
            wchar_t *copy = (wchar_t *)malloc(wl * sizeof(wchar_t));
            if (copy) {
                wcscpy(copy, wtitle);
                PostMessage(hwnd, WM_APP_STATUS_SET, 0xAA, (LPARAM)copy);
            }
        }
        natural_end = play_song(&song, hwnd);
    }
    free(song.notes);

    PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, (LPARAM)natural_end);
    free(req->url); free(req->title); free(req);
    return 0;
}

static DWORD WINAPI PlayLocalThread(LPVOID param)
{
    LocalPlayRequest *req = (LocalPlayRequest *)param;
    HWND hwnd = req->hwnd;
    LONG myGen = req->gen;

    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) != myGen) {
        free(req->path); free(req->title); free(req);
        return 0;
    }

    post_status(hwnd, "正在读取文件...");

    HANDLE hFile = CreateFileW(req->path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        post_status(hwnd, "无法打开文件");
        post_error(hwnd,
            L"无法打开文件。\n\n"
            L"文件可能已被删除、移动，或正被其他程序占用。");
        PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, 0);
        free(req->path); free(req->title); free(req);
        return 0;
    }
    DWORD size = GetFileSize(hFile, NULL);
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    DWORD rd = 0;
    if (buf) ReadFile(hFile, buf, size, &rd, NULL);
    CloseHandle(hFile);

    if (!buf || rd == 0) {
        free(buf);
        post_status(hwnd, "文件为空或读取失败");
        post_error(hwnd,
            L"文件为空或读取失败。\n\n"
            L"请确认文件不是 0 字节，并且没有被其他程序锁定。");
        PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, 0);
        free(req->path); free(req->title); free(req);
        return 0;
    }
    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) != myGen) {
        free(buf); free(req->path); free(req->title); free(req);
        return 0;
    }

    const wchar_t *ext = wcsrchr(req->path, L'.');
    if (ext) ext++; else ext = L"";

    wchar_t stmsg[128];
    swprintf(stmsg, 128, L"正在解析 .%ls 文件...", ext);
    post_status_w(hwnd, stmsg);

    Song song;
    if (parse_any_file(buf, rd, ext, &song) < 0 || song.n == 0) {
        free(buf);
        post_status(hwnd, "解析失败（格式不识别或无音符）");
        post_error(hwnd,
            L"文件损坏或格式不支持。\n\n"
            L"支持的格式：\n"
            L"  • CBND (.cbnd)\n"
            L"  • NBS (.nbs)\n"
            L"  • MIDI (.mid/.midi)\n\n"
            L"若这是 MIDI 文件，请确认它是标准 MIDI (MThd 开头)，\n"
            L"而不是 RIFF MIDI (RIFF 开头)。");
        PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, 0);
        free(req->path); free(req->title); free(req);
        return 0;
    }
    free(buf);
    qsort(song.notes, song.n, sizeof(Note), cmp_note);

    if (!g_samplesReady) {
        post_status(hwnd, "等待音源加载...");
        while (!g_samplesReady &&
               InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen &&
               !g_stopRequest) Sleep(50);
    }

    int natural_end = 0;
    if (InterlockedCompareExchange(&g_currentGen, myGen, myGen) == myGen &&
        !g_stopRequest) {
        InterlockedExchange(&g_totalTicks, (LONG)(song.max_tick + 1));

        wchar_t st[512];
        ZeroMemory(st, sizeof(st));
        swprintf(st, 512, L"正在播放：%ls", req->title);
        st[511] = 0;
        post_status_w(hwnd, st);

        {
            size_t wl = wcslen(req->title) + 1;
            wchar_t *copy = (wchar_t *)malloc(wl * sizeof(wchar_t));
            if (copy) {
                wcscpy(copy, req->title);
                PostMessage(hwnd, WM_APP_STATUS_SET, 0xAA, (LPARAM)copy);
            }
        }
        natural_end = play_song(&song, hwnd);
    }
    free(song.notes);

    PostMessage(hwnd, WM_APP_PLAY_DONE, (WPARAM)myGen, (LPARAM)natural_end);
    free(req->path); free(req->title); free(req);
    return 0;
}

/* ==========================================================================
 *  瀑布图窗口
 * ========================================================================== */
static LRESULT CALLBACK WaterfallProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcScreen = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        if (W < 40 || H < 40) { EndPaint(hwnd, &ps); return 0; }

        /* ========== 双缓冲：先在内存 DC 上画，最后一次性 BitBlt ========== */
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, W, H);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);
        HDC hdc = hdcMem;

        HBRUSH bg = CreateSolidBrush(C_WF_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        int minK = g_wfMinKey, maxK = g_wfMaxKey;
        int keyRange = maxK - minK;
        if (keyRange <= 0) keyRange = 1;

        /* 竖直网格线 */
        HPEN gridPen = CreatePen(PS_SOLID, 1, C_WF_GRID);
        HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
        int startK = ((minK + 11) / 12) * 12;
        for (int k = startK; k <= maxK; k += 12) {
            int x = (k - minK) * W / keyRange;
            MoveToEx(hdc, x, 0, NULL);
            LineTo(hdc, x, H);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        /* 当前浮点 tick */
        double curTick;
        if (g_progDragging) {
            curTick = g_progDragVal * (double)g_totalTicks;
        } else {
            curTick = (double)g_currentTickF / 1000.0;
        }

        int midY = H / 2;

        /* 音符 */
        if (g_wfNotes && g_wfNotesN > 0) {
            double halfWin = g_wfWindowTicks / 2.0;
            double topTick = curTick + halfWin;
            double botTick = curTick - halfWin;

            size_t lo = 0, hi = g_wfNotesN;
            while (lo < hi) {
                size_t m = (lo + hi) / 2;
                if ((double)g_wfNotes[m].tick < botTick) lo = m + 1;
                else hi = m;
            }

            int noteW = W / keyRange;
            if (noteW < 5) noteW = 5;
            if (noteW > 24) noteW = 24;
            int noteH = H / g_wfWindowTicks;
            if (noteH < 4) noteH = 4;
            if (noteH > 22) noteH = 22;

            COLORREF white = RGB(0xff, 0xff, 0xff);

            for (size_t i = lo; i < g_wfNotesN; ) {
                uint32_t t = g_wfNotes[i].tick;
                uint8_t  key = g_wfNotes[i].key;
                if ((double)t > topTick) break;

                /* 收集同 tick 同 key 的所有音符 */
                size_t j = i;
                while (j < g_wfNotesN &&
                       g_wfNotes[j].tick == t &&
                       g_wfNotes[j].key  == key) {
                    j++;
                }

                /* 不同乐器 → 不同颜色（去重），同时统计 velocity */
                COLORREF colors[16];
                int colorCount = 0;
                int velSum = 0, velCount = 0;
                for (size_t q = i; q < j; q++) {
                    COLORREF c = WF_COLORS[g_wfNotes[q].instrument & 0x0F];
                    int found = 0;
                    for (int m = 0; m < colorCount; m++) {
                        if (colors[m] == c) { found = 1; break; }
                    }
                    if (!found && colorCount < 16) colors[colorCount++] = c;
                    velSum += g_wfNotes[q].velocity;
                    velCount++;
                }

                double yCenter = (topTick - (double)t) / (double)g_wfWindowTicks * H;
                int y = (int)(yCenter - noteH / 2.0 + 0.5);
                int x = (key - minK) * W / keyRange - noteW / 2;
                if (x < 0) x = 0;
                if (x + noteW > W) x = W - noteW;

                i = j;   /* 推进 */

                if (y + noteH < 0 || y > H) continue;

                /* 白色逻辑 */
                double dist = yCenter - midY;
                double tt;
                if (dist < -noteH * 0.5) {
                    tt = 1.0;
                } else if (dist <= noteH * 0.5) {
                    tt = 0.0;
                } else if (dist < noteH * 3.0) {
                    tt = (dist - noteH * 0.5) / (noteH * 2.5);
                    if (tt > 1.0) tt = 1.0;
                } else {
                    tt = 1.0;
                }

                /* velocity 调整明暗：0.5~1.0 亮度系数 */
                int avgVel = (velCount > 0) ? (velSum / velCount) : 100;
                double vk = 0.5 + 0.5 * ((double)avgVel / 127.0);
                if (vk > 1.0) vk = 1.0;
                for (int m = 0; m < colorCount; m++) {
                    int r = (int)(GetRValue(colors[m]) * vk + 0.5);
                    int g = (int)(GetGValue(colors[m]) * vk + 0.5);
                    int b = (int)(GetBValue(colors[m]) * vk + 0.5);
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                    colors[m] = RGB(r, g, b);
                }

                /* 每个颜色都做白色插值 */
                COLORREF finalColors[16];
                for (int m = 0; m < colorCount; m++)
                    finalColors[m] = lerp_color(white, colors[m], tt);

                /* 绘制：单个颜色直接填，多色用斜条纹 */
                if (colorCount <= 1) {
                    HBRUSH br = CreateSolidBrush(finalColors[0]);
                    RECT nr = { x, y, x + noteW, y + noteH };
                    FillRect(hdc, &nr, br);
                    DeleteObject(br);
                } else {
                    draw_striped_rect(hdc, x, y, noteW, noteH,
                                      finalColors, colorCount);
                }

                /* 半透明白色细描边 */
                HPEN edgePen = CreatePen(PS_SOLID, 1, RGB(0xA8, 0xA8, 0xA8));
                HPEN oldEdgePen = (HPEN)SelectObject(hdc, edgePen);
                HBRUSH oldEdgeBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, x, y, x + noteW, y + noteH);
                SelectObject(hdc, oldEdgeBr);
                SelectObject(hdc, oldEdgePen);
                DeleteObject(edgePen);
            }
        }

        /* ========== 乐器图例（自适应文本宽度，多行自动换行） ========== */
        if (g_wfNotes && g_wfNotesN > 0 && H > 80) {
            static const wchar_t *WF_NAMES[16] = {
                L"harp",     L"bass",        L"bd",        L"snare",
                L"hat",      L"guitar",      L"flute",     L"bell",
                L"chime",    L"xylophone",   L"iron_xylophone",
                L"cow_bell", L"didgeridoo",  L"bit",       L"banjo",
                L"pling"
            };
            int used[16] = {0};
            for (size_t k = 0; k < g_wfNotesN; k++)
                used[g_wfNotes[k].instrument & 0x0F] = 1;

            int list[16], listN = 0;
            for (int inst = 0; inst < 16; inst++)
                if (used[inst]) list[listN++] = inst;

            if (listN > 0) {
                int itemH   = 18;
                int padX    = 8;
                int swatchW = 12;   /* 色块宽 */
                int gap     = 4;    /* 色块与文本间隔 */
                int itemGap = 10;   /* 两个图例项之间的最小间隔 */

                HFONT oldF = (HFONT)SelectObject(hdc, g_fontSmall);
                SetBkMode(hdc, TRANSPARENT);

                /* 测量每个音色名的宽度 */
                int textW[16];
                for (int k = 0; k < listN; k++) {
                    SIZE sz = {0, 0};
                    GetTextExtentPoint32W(hdc, WF_NAMES[list[k]],
                                          (int)wcslen(WF_NAMES[list[k]]), &sz);
                    textW[k] = sz.cx;
                }

                /* 排版：逐项累加，放不下就换行 */
                int avail = W - padX * 2 - 8;
                if (avail < 60) avail = 60;

                int lineX[16];         /* 每项相对内容区左侧的 x 偏移 */
                int lineY[16];         /* 每项相对内容区顶部的 y 偏移 */
                int lineNum[16];       /* 每项所在的行号 */
                int curX = 0;
                int curRow = 0;
                int maxRowW = 0;

                for (int k = 0; k < listN; k++) {
                    int iw = swatchW + gap + textW[k];   /* 本项宽度 */
                    if (curX > 0 && curX + itemGap + iw > avail) {
                        /* 换行 */
                        if (curX > maxRowW) maxRowW = curX;
                        curRow++;
                        curX = 0;
                    }
                    if (curX > 0) curX += itemGap;
                    lineX[k] = curX;
                    lineY[k] = curRow * itemH;
                    lineNum[k] = curRow;
                    curX += iw;
                }
                if (curX > maxRowW) maxRowW = curX;

                int rowsUsed = curRow + 1;
                if (rowsUsed > 6) rowsUsed = 6;

                int boxW = padX * 2 + maxRowW;
                if (boxW > W - 8) boxW = W - 8;
                int boxH = rowsUsed * itemH + 4;
                int boxX = 4;
                int boxY = H - boxH - 2;

                /* 深色底 */
                HBRUSH bgB = CreateSolidBrush(RGB(0x18, 0x18, 0x18));
                RECT bgR = { boxX, boxY, boxX + boxW, boxY + boxH };
                FillRect(hdc, &bgR, bgB);
                DeleteObject(bgB);

                HPEN bp = CreatePen(PS_SOLID, 1, RGB(0x50, 0x50, 0x50));
                HPEN obp = (HPEN)SelectObject(hdc, bp);
                HBRUSH obb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, boxX, boxY, boxX + boxW, boxY + boxH);
                SelectObject(hdc, obb);
                SelectObject(hdc, obp);
                DeleteObject(bp);

                for (int k = 0; k < listN; k++) {
                    if (lineNum[k] >= rowsUsed) break;
                    int cx = boxX + padX + lineX[k];
                    int cy = boxY + 2 + lineY[k];

                    RECT sq = { cx, cy + 3, cx + swatchW, cy + 15 };
                    HBRUSH bb = CreateSolidBrush(WF_COLORS[list[k]]);
                    FillRect(hdc, &sq, bb);
                    DeleteObject(bb);

                    SetTextColor(hdc, RGB(0xd0, 0xd0, 0xd0));
                    RECT tx = { cx + swatchW + gap, cy,
                                cx + swatchW + gap + textW[k], cy + itemH };
                    DrawTextW(hdc, WF_NAMES[list[k]], -1, &tx,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                SelectObject(hdc, oldF);
            }
        }

        /* ========== 中央白线 >---------< ========== */
        {
            int triSize = 8;

            HBRUSH wb = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
            HBRUSH ob = (HBRUSH)SelectObject(hdc, wb);
            HPEN   op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));

            POINT triL[3] = {
                { 0,                midY - triSize },
                { 0,                midY + triSize },
                { triSize + 6,      midY }
            };
            POINT triR[3] = {
                { W - 1,            midY - triSize },
                { W - 1,            midY + triSize },
                { W - triSize - 7,  midY }
            };
            Polygon(hdc, triL, 3);
            Polygon(hdc, triR, 3);

            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(wb);

            HPEN whitePen = CreatePen(PS_SOLID, 2, RGB(0xff, 0xff, 0xff));
            oldPen = (HPEN)SelectObject(hdc, whitePen);
            MoveToEx(hdc, triSize + 6, midY, NULL);
            LineTo(hdc, W - triSize - 7, midY);
            SelectObject(hdc, oldPen);
            DeleteObject(whitePen);
        }

        /* 一次性拷回屏幕 */
        BitBlt(hdcScreen, 0, 0, W, H, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_wfWindowTicks += (d > 0) ? -4 : 4;
            if (g_wfWindowTicks < 8) g_wfWindowTicks = 8;
            if (g_wfWindowTicks > 128) g_wfWindowTicks = 128;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/* ==========================================================================
 *  帮助覆盖窗口
 * ========================================================================== */
static LRESULT CALLBACK HelpOverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        /* 填充整个客户区 */
        HBRUSH bg = CreateSolidBrush(RGB(0x18, 0x18, 0x18));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* 边框 */
        HPEN hp = CreatePen(PS_SOLID, 2, C_ACCENT);
        HPEN ohp = (HPEN)SelectObject(hdc, hp);
        HBRUSH obh = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 1, 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, obh);
        SelectObject(hdc, ohp);
        DeleteObject(hp);

        SetBkMode(hdc, TRANSPARENT);

        /* 标题 */
        SetTextColor(hdc, C_ACCENT_HI);
        HFONT oldF = (HFONT)SelectObject(hdc, g_fontBold);
        RECT tr = { 24, 10, rc.right - 24, 44 };
        DrawTextW(hdc, L"使用帮助", -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldF);

        /* 分隔线 */
        HPEN sep = CreatePen(PS_SOLID, 1, RGB(0x60, 0x50, 0x20));
        HPEN osep = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, 16, 48, NULL);
        LineTo(hdc, rc.right - 16, 48);
        SelectObject(hdc, osep);
        DeleteObject(sep);

        /* 内容 */
        SetTextColor(hdc, RGB(0xE0, 0xE0, 0xE0));
        oldF = (HFONT)SelectObject(hdc, g_fontSmall);
        int yy = 56;
        int lineH = 17;
        int yLimit = rc.bottom - 30;
        for (int i = 0; HELP_LINES[i]; i++) {
            if (yy + lineH > yLimit) break;
            RECT lr = { 24, yy, rc.right - 24, yy + lineH };
            DrawTextW(hdc, HELP_LINES[i], -1, &lr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            yy += lineH;
        }
        SelectObject(hdc, oldF);

        /* 底部提示 */
        SetTextColor(hdc, C_ACCENT);
        oldF = (HFONT)SelectObject(hdc, g_fontSmall);
        RECT xr = { 24, rc.bottom - 28, rc.right - 24, rc.bottom - 8 };
        DrawTextW(hdc, L"点击任意处关闭", -1, &xr,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldF);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        g_showHelp = 0;
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_CLOSE:
        g_showHelp = 0;
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_showHelp = 0;
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  绘制模式图标
 * ========================================================================== */
static void draw_mode_icon(HDC hdc, int x, int y, int size, int mode, int active)
{
    COLORREF col = active ? C_ACCENT : C_MUTED;
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    if (mode == MODE_ORDER) {
        int y1 = y + size/3, y2 = y + size*2/3;
        int x1 = x + 2, x2 = x + size - 2;
        MoveToEx(hdc, x1, y1, NULL); LineTo(hdc, x2, y1);
        MoveToEx(hdc, x2, y1, NULL); LineTo(hdc, x2 - 4, y1 - 3);
        MoveToEx(hdc, x2, y1, NULL); LineTo(hdc, x2 - 4, y1 + 3);
        MoveToEx(hdc, x1, y2, NULL); LineTo(hdc, x2, y2);
        MoveToEx(hdc, x2, y2, NULL); LineTo(hdc, x2 - 4, y2 - 3);
        MoveToEx(hdc, x2, y2, NULL); LineTo(hdc, x2 - 4, y2 + 3);
    } else if (mode == MODE_RANDOM) {
        int ymid = y + size/2;
        int x1 = x + 2, x2 = x + size - 2;
        MoveToEx(hdc, x1, ymid - 4, NULL); LineTo(hdc, x2, ymid - 4);
        MoveToEx(hdc, x2, ymid - 4, NULL); LineTo(hdc, x2 - 4, ymid - 7);
        MoveToEx(hdc, x2, ymid - 4, NULL); LineTo(hdc, x2 - 4, ymid - 1);
        MoveToEx(hdc, x2, ymid + 4, NULL); LineTo(hdc, x1, ymid + 4);
        MoveToEx(hdc, x1, ymid + 4, NULL); LineTo(hdc, x1 + 4, ymid + 1);
        MoveToEx(hdc, x1, ymid + 4, NULL); LineTo(hdc, x1 + 4, ymid + 7);
    } else {
        int cx = x + size / 2, cy = y + size / 2;
        int r  = size / 2 - 3;
        if (r < 5) r = 5;
        double a0 = -M_PI / 3.0;
        double a1 = M_PI * 4.0 / 3.0;
        int steps = 40;
        for (int i = 0; i <= steps; i++) {
            double a = a0 + (a1 - a0) * i / steps;
            int nx = cx + (int)(r * cos(a) + 0.5);
            int ny = cy + (int)(r * sin(a) + 0.5);
            if (i == 0) MoveToEx(hdc, nx, ny, NULL);
            else        LineTo(hdc, nx, ny);
        }
        int ax = cx + (int)(r * cos(a0) + 0.5);
        int ay = cy + (int)(r * sin(a0) + 0.5);
        double at = a0 + M_PI / 2.0;
        double len = 6.0, spread = 0.55;
        POINT tri[3];
        tri[0].x = ax; tri[0].y = ay;
        tri[1].x = ax - (int)(len * cos(at - spread));
        tri[1].y = ay - (int)(len * sin(at - spread));
        tri[2].x = ax - (int)(len * cos(at + spread));
        tri[2].y = ay - (int)(len * sin(at + spread));
        HBRUSH br = CreateSolidBrush(col);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        HPEN   op2 = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, tri, 3);
        SelectObject(hdc, ob);
        SelectObject(hdc, op2);
        DeleteObject(br);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

/* ==========================================================================
 *  搜索框子类化：绘制 placeholder
 * ========================================================================== */
static WNDPROC g_oldSearchProc = NULL;

static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        LRESULT r = CallWindowProc(g_oldSearchProc, hwnd, msg, wp, lp);

        /* 编辑框为空且没有焦点时，绘制提示文本 */
        int len = GetWindowTextLengthW(hwnd);
        if (len == 0 && GetFocus() != hwnd) {
            HDC hdc = GetDC(hwnd);
            RECT rc; GetClientRect(hwnd, &rc);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0x70, 0x70, 0x70));
            HFONT old = (HFONT)SelectObject(hdc, g_fontNormal);
            rc.left += 10;
            DrawTextW(hdc, L"搜索歌曲...", -1, &rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            ReleaseDC(hwnd, hdc);
        }
        return r;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    return CallWindowProc(g_oldSearchProc, hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  模式按钮
 * ========================================================================== */
static LRESULT CALLBACK ModesProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG_CTRL);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        int size = (w < h ? w : h) - 10;
        if (size < 16) size = 16;
        int x = (w - size) / 2, y = (h - size) / 2;
        draw_mode_icon(hdc, x, y, size, g_playMode, 1);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_playMode = (g_playMode + 1) % 3;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  进度条
 * ========================================================================== */
static LRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG_CTRL);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        int cy = (rc.top + rc.bottom) / 2;
        int th = 8;
        RECT track = { 0, cy - th/2, rc.right, cy + th/2 };
        HBRUSH trk = CreateSolidBrush(C_TRACK);
        FillRect(hdc, &track, trk);
        DeleteObject(trk);
        double v = g_progDragging ? g_progDragVal : g_progValue;
        if (v < 0) v = 0; if (v > 1) v = 1;
        int fx = (int)(rc.right * v);
        RECT fill = { 0, cy - th/2, fx, cy + th/2 };
        HBRUSH fl = CreateSolidBrush(C_ACCENT);
        FillRect(hdc, &fill, fl);
        DeleteObject(fl);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        g_progDragging = 1;
        SetCapture(hwnd);
        RECT rc; GetClientRect(hwnd, &rc);
        int x = (short)LOWORD(lp);
        double v = (double)x / rc.right;
        if (v < 0) v = 0; if (v > 1) v = 1;
        g_progDragVal = v;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_progDragging) {
            RECT rc; GetClientRect(hwnd, &rc);
            int x = (short)LOWORD(lp);
            double v = (double)x / rc.right;
            if (v < 0) v = 0; if (v > 1) v = 1;
            g_progDragVal = v;
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_hWaterfall)
                InvalidateRect(g_hWaterfall, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_progDragging) {
            g_progDragging = 0;
            ReleaseCapture();
            LONG total = g_totalTicks;
            LONG seek = (LONG)(g_progDragVal * total);
            if (seek < 0) seek = 0;
            if (total > 0 && seek >= total) seek = total - 1;
            InterlockedExchange(&g_seekTick, seek);
            InterlockedExchange(&g_currentTick, seek);
            InterlockedExchange(&g_currentTickF, (LONG)(seek * 1000));
            g_progValue = g_progDragVal;
            g_lastSeekTime = GetTickCount();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  倍速滑块
 * ========================================================================== */
#define SPEED_MIN 0.10
#define SPEED_MAX 5.00
#define SPEED_TEXT_W 66

static LRESULT CALLBACK SpeedProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG_CTRL);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        int trackW = rc.right - SPEED_TEXT_W - 4;
        int cy = (rc.top + rc.bottom) / 2;
        int th = 4;
        RECT track = { 8, cy - th/2, trackW, cy + th/2 };
        HBRUSH trk = CreateSolidBrush(C_TRACK);
        FillRect(hdc, &track, trk);
        DeleteObject(trk);
        double t = (g_speedVal - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
        if (t < 0) t = 0; if (t > 1) t = 1;
        int px = 8 + (int)((trackW - 16) * t);
        RECT fill = { 8, cy - th/2, px, cy + th/2 };
        HBRUSH fl = CreateSolidBrush(C_ACCENT);
        FillRect(hdc, &fill, fl);
        DeleteObject(fl);
        int hs = 14;
        HBRUSH kn = CreateSolidBrush(C_ACCENT);
        HBRUSH ok = (HBRUSH)SelectObject(hdc, kn);
        HPEN   op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, px - hs/2, cy - hs/2, px + hs/2, cy + hs/2);
        SelectObject(hdc, ok);
        SelectObject(hdc, op);
        DeleteObject(kn);
        wchar_t sb[32];
        swprintf(sb, 32, L"%.2fx", g_speedVal);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, C_TEXT);
        HFONT old = (HFONT)SelectObject(hdc, g_fontSmall);
        RECT tr = { trackW + 4, 0, rc.right, rc.bottom };
        DrawTextW(hdc, sb, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        int trackW = rc.right - SPEED_TEXT_W - 4;
        int x = (short)LOWORD(lp);
        if (x < 8 || x > trackW) return 0;
        g_sliderDrag = 1;
        SetCapture(hwnd);
        double t = (double)(x - 8) / (trackW - 16);
        if (t < 0) t = 0; if (t > 1) t = 1;
        g_speedVal = SPEED_MIN + t * (SPEED_MAX - SPEED_MIN);
        InterlockedExchange(&g_speedMilli, (LONG)(g_speedVal * 1000));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_sliderDrag) {
            RECT rc; GetClientRect(hwnd, &rc);
            int trackW = rc.right - SPEED_TEXT_W - 4;
            int x = (short)LOWORD(lp);
            double t = (double)(x - 8) / (trackW - 16);
            if (t < 0) t = 0; if (t > 1) t = 1;
            g_speedVal = SPEED_MIN + t * (SPEED_MAX - SPEED_MIN);
            InterlockedExchange(&g_speedMilli, (LONG)(g_speedVal * 1000));
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_sliderDrag) { g_sliderDrag = 0; ReleaseCapture(); }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  滚动辅助
 * ========================================================================== */
static void scroll_by_wheel(int delta)
{
    g_scrollTarget -= (delta * 60) / WHEEL_DELTA;
    int maxScroll = g_scrollContentH - g_scrollViewH;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollTarget < 0) g_scrollTarget = 0;
    if (g_scrollTarget > maxScroll) g_scrollTarget = maxScroll;
    if (!g_scrollTimerOn && g_hMain) {
        SetTimer(g_hMain, 2, 8, NULL);
        g_scrollTimerOn = 1;
    }
}
static void compute_thumb(int *thumbY, int *thumbH, int *maxThumbY, int *maxScroll)
{
    int viewH = g_scrollViewH, contentH = g_scrollContentH;
    if (contentH <= viewH) {
        *thumbY = 0; *thumbH = viewH; *maxThumbY = 0; *maxScroll = 0;
        return;
    }
    int th = (int)((double)viewH * viewH / contentH);
    if (th < 24) th = 24;
    *thumbH = th;
    *maxThumbY = viewH - th;
    *maxScroll = contentH - viewH;
    *thumbY = (*maxScroll > 0) ? (int)((double)g_scrollPos * (*maxThumbY) / (*maxScroll)) : 0;
    if (*thumbY < 0) *thumbY = 0;
    if (*thumbY > *maxThumbY) *thumbY = *maxThumbY;
}

static WNDPROC g_oldListProc = NULL;
static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEMOVE) {
        LVHITTESTINFO hti = {0};
        hti.pt.x = (short)LOWORD(lp);
        hti.pt.y = (short)HIWORD(lp);
        int idx = ListView_HitTest(hwnd, &hti);
        if (idx != g_hoverIdx) { g_hoverIdx = idx; InvalidateRect(hwnd, NULL, FALSE); }
        TRACKMOUSEEVENT tme = {0};
        tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        if (g_hoverIdx != -1) { g_hoverIdx = -1; InvalidateRect(hwnd, NULL, FALSE); }
    } else if (msg == WM_MOUSEWHEEL) {
        scroll_by_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    return CallWindowProc(g_oldListProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ViewportProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(C_BG);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(C_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        RECT barRc = { rc.right - SCROLLBAR_W, 0, rc.right, rc.bottom };
        HBRUSH tb = CreateSolidBrush(C_BG_HEADER);
        FillRect(hdc, &barRc, tb);
        DeleteObject(tb);
        if (g_scrollContentH > g_scrollViewH && g_scrollViewH > 0) {
            int thumbY, thumbH, maxThumbY, maxScroll;
            compute_thumb(&thumbY, &thumbH, &maxThumbY, &maxScroll);
            RECT thumb = { rc.right - SCROLLBAR_W + 2, thumbY + 2,
                           rc.right - 2, thumbY + thumbH - 2 };
            HBRUSH thb = CreateSolidBrush(C_ACCENT);
            HRGN rgn = CreateRoundRectRgn(thumb.left, thumb.top,
                                          thumb.right + 1, thumb.bottom + 1, 6, 6);
            FillRgn(hdc, rgn, thb);
            DeleteObject(rgn);
            DeleteObject(thb);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_by_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hwnd, &rc);
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        if (x >= rc.right - SCROLLBAR_W && g_scrollContentH > g_scrollViewH) {
            int thumbY, thumbH, maxThumbY, maxScroll;
            compute_thumb(&thumbY, &thumbH, &maxThumbY, &maxScroll);
            g_sbDragging = 1;
            if (y >= thumbY && y < thumbY + thumbH) {
                g_sbDragOffset = y - thumbY;
            } else {
                g_sbDragOffset = thumbH / 2;
                int newY = y - g_sbDragOffset;
                if (newY < 0) newY = 0;
                if (newY > maxThumbY) newY = maxThumbY;
                int newScroll = (maxThumbY > 0) ? (int)((double)newY * maxScroll / maxThumbY) : 0;
                g_scrollPos = newScroll; g_scrollTarget = newScroll;
                if (g_hList) SetWindowPos(g_hList, NULL, 0, -g_scrollPos,
                    rc.right + LISTVIEW_EXTRA_W, g_scrollContentH + LISTVIEW_EXTRA_H,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_sbDragging) {
            RECT rc; GetClientRect(hwnd, &rc);
            int y = (short)HIWORD(lp);
            int thumbY, thumbH, maxThumbY, maxScroll;
            compute_thumb(&thumbY, &thumbH, &maxThumbY, &maxScroll);
            int newY = y - g_sbDragOffset;
            if (newY < 0) newY = 0;
            if (newY > maxThumbY) newY = maxThumbY;
            int newScroll = (maxThumbY > 0) ? (int)((double)newY * maxScroll / maxThumbY) : 0;
            g_scrollPos = newScroll; g_scrollTarget = newScroll;
            if (g_hList) SetWindowPos(g_hList, NULL, 0, -g_scrollPos,
                rc.right + LISTVIEW_EXTRA_W, g_scrollContentH + LISTVIEW_EXTRA_H,
                SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_sbDragging) { g_sbDragging = 0; ReleaseCapture(); return 0; }
        break;
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (g_hMain) return SendMessage(g_hMain, msg, wp, lp);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void reposition_listview(void)
{
    if (!g_hList || !g_hViewport) return;
    RECT vrc; GetClientRect(g_hViewport, &vrc);
    int contentH = g_scrollContentH + LISTVIEW_EXTRA_H;
    if (contentH < vrc.bottom) contentH = vrc.bottom;
    SetWindowPos(g_hList, NULL, 0, -g_scrollPos,
                 vrc.right + LISTVIEW_EXTRA_W, contentH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    LVCOLUMNW col = {0};
    col.mask = LVCF_WIDTH;
    col.cx = vrc.right - SCROLLBAR_W - 4;
    ListView_SetColumn(g_hList, 0, &col);
}

static void layout_controls(HWND hwnd);

static void rebuild_filter(void)
{
    if (!g_songs || g_songCount == 0) {
        free(g_filteredIdx); g_filteredIdx = NULL;
        g_filteredCount = 0;
        if (g_hList) ListView_DeleteAllItems(g_hList);
        g_scrollContentH = 0;
        return;
    }
    free(g_filteredIdx);
    g_filteredIdx = (int *)malloc(sizeof(int) * g_songCount);
    g_filteredCount = 0;
    if (!g_filteredIdx) return;
    wchar_t lf[64];
    int flen = (int)wcslen(g_searchText);
    for (int i = 0; i < flen; i++) lf[i] = towlower(g_searchText[i]);
    lf[flen] = 0;

    /* 检测 #数字 格式：优先显示对应编号的歌曲 */
    int priorityIdx = -1;
    if (flen >= 2 && g_searchText[0] == L'#') {
        int allDigits = 1;
        long num = 0;
        for (int i = 1; i < flen; i++) {
            if (g_searchText[i] < L'0' || g_searchText[i] > L'9') {
                allDigits = 0;
                break;
            }
            num = num * 10 + (g_searchText[i] - L'0');
            if (num > 1000000) { allDigits = 0; break; }
        }
        if (allDigits && num >= 1 && num <= (long)g_songCount) {
            priorityIdx = (int)(num - 1);
            g_filteredIdx[g_filteredCount++] = priorityIdx;
        }
    }

    for (size_t i = 0; i < g_songCount; i++) {
        if ((int)i == priorityIdx) continue;
        if (flen == 0) { g_filteredIdx[g_filteredCount++] = (int)i; continue; }
        wchar_t wname[512];
        u8_to_w_buf(g_songs[i].name, wname, 512);
        int wlen = (int)wcslen(wname);
        int match = 0;
        for (int k = 0; k + flen <= wlen; k++) {
            int j;
            for (j = 0; j < flen; j++)
                if (towlower(wname[k+j]) != lf[j]) break;
            if (j == flen) { match = 1; break; }
        }
        if (match) g_filteredIdx[g_filteredCount++] = (int)i;
    }
    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hList);
    for (int i = 0; i < g_filteredCount; i++) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT; item.iItem = i; item.pszText = L"";
        ListView_InsertItem(g_hList, &item);
    }
    /* 追加末尾 3 个信息行 */
    for (int i = 0; i < INFO_ROW_COUNT; i++) {
        LVITEMW item = {0};
        item.mask = LVIF_TEXT; item.iItem = g_filteredCount + i; item.pszText = L"";
        ListView_InsertItem(g_hList, &item);
    }
    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    g_scrollContentH = (g_filteredCount + INFO_ROW_COUNT) * CARD_H;
    g_scrollPos = 0; g_scrollTarget = 0;
    if (g_hMain) layout_controls(g_hMain);
    InvalidateRect(g_hList, NULL, TRUE);
}

/* 计算右侧面板宽度：窗口的 35%，钳制在 360~500，且保证左侧至少 360 */
static int calc_right_width(int cw)
{
    int w = cw * 35 / 100;
    if (w < 360) w = 360;
    if (w > 500) w = 500;
    if (cw - w < 360) w = cw - 360;
    if (w < 180) w = 180;
    return w;
}

static void layout_controls(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

    int rightW = calc_right_width(cw);
    int leftW  = cw - rightW;

    /* ===== 左侧面板 ===== */
    int ctrlTop = ch - CTRL_H;
    if (g_hWaterfall)
        MoveWindow(g_hWaterfall, 0, TOPBAR_H, leftW, ctrlTop - TOPBAR_H, TRUE);

    int baseY = ctrlTop;
    int lpad = 10;

    /* 第一行：状态文字 */
    g_statusRect.left   = lpad;
    g_statusRect.top    = baseY + 6;
    g_statusRect.right  = leftW - lpad;
    g_statusRect.bottom = baseY + 26;

    /* 第二行：暂停 + 时间 + 进度条 */
    int pauseW = 40;
    MoveWindow(g_hPause, lpad, baseY + 30, pauseW, 26, TRUE);

    int timeLeft = lpad + pauseW + 6;
    g_timeRect.left   = timeLeft;
    g_timeRect.top    = baseY + 32;
    g_timeRect.right  = timeLeft + TIME_W;
    g_timeRect.bottom = baseY + 52;

    int progLeft = g_timeRect.right + 6;
    if (progLeft < lpad + 140) progLeft = lpad + 140;
    MoveWindow(g_hProgress, progLeft, baseY + 32,
               leftW - lpad - progLeft, 20, TRUE);

    /* 第三行：打开文件 + 速度 + 模式 */
    int modesW = 40;
    int helpW  = 32;
    int openW  = 110;
    int modesLeft  = leftW - lpad - modesW;
    int helpLeft   = modesLeft - 6 - helpW;
    int speedLeft  = lpad + openW + 6;
    int speedRight = helpLeft - 6;
    if (speedRight < speedLeft + 60) speedRight = speedLeft + 60;

    MoveWindow(g_hOpenFile, lpad, baseY + 62, openW, 28, TRUE);
    MoveWindow(g_hSpeed, speedLeft, baseY + 66,
               speedRight - speedLeft, 20, TRUE);
    MoveWindow(g_hHelp, helpLeft, baseY + 60, helpW, 32, TRUE);
    MoveWindow(g_hModes, modesLeft, baseY + 60, modesW, 32, TRUE);

    /* ===== 右侧面板 ===== */
    int rx = leftW;

    if (g_hSearch) {
        int srchY = TOPBAR_H + (SEARCH_AREA_H - 34) / 2;
        MoveWindow(g_hSearch, rx + 10, srchY, rightW - 20, 34, TRUE);
    }

    int listTop = TOPBAR_H + SEARCH_AREA_H;
    if (g_hViewport)
        MoveWindow(g_hViewport, rx, listTop, rightW, ch - listTop, TRUE);

    g_scrollViewW = rightW;
    g_scrollViewH = ch - listTop;
    g_scrollContentH = (g_filteredCount + INFO_ROW_COUNT) * CARD_H;

    reposition_listview();

    int maxScroll = g_scrollContentH - g_scrollViewH;
    if (maxScroll < 0) maxScroll = 0;
    if (g_scrollTarget > maxScroll) g_scrollTarget = maxScroll;
    if (g_scrollPos    > maxScroll) g_scrollPos    = maxScroll;

    InvalidateRect(hwnd, NULL, FALSE);
    if (g_hViewport) InvalidateRect(g_hViewport, NULL, FALSE);
    if (g_hWaterfall) InvalidateRect(g_hWaterfall, NULL, FALSE);
}

static int pick_next_song(int curRealIdx)
{
    if (g_songCount == 0) return -1;
    if (g_playMode == MODE_SINGLE) return curRealIdx;
    if (g_playMode == MODE_RANDOM) {
        if (g_songCount == 1) return 0;
        int n;
        do { n = rand() % (int)g_songCount; } while (n == curRealIdx);
        return n;
    }
    if (curRealIdx < 0) return 0;
    return (curRealIdx + 1) % (int)g_songCount;
}

static void start_play_idx(HWND hwnd, int idx);

static void start_play_filtered(HWND hwnd, int filteredIdx)
{
    if (filteredIdx < 0 || filteredIdx >= g_filteredCount) return;
    int real = g_filteredIdx[filteredIdx];
    start_play_idx(hwnd, real);
}

static void open_local_file(HWND hwnd)
{
    wchar_t path[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd;
    ofn.lpstrFilter  = L"音频文件 (*.cbnd;*.nbs;*.mid;*.midi)\0*.cbnd;*.nbs;*.mid;*.midi\0"
                       L"CBND 序列 (*.cbnd)\0*.cbnd\0"
                       L"NBS 乐谱 (*.nbs)\0*.nbs\0"
                       L"MIDI 文件 (*.mid;*.midi)\0*.mid;*.midi\0"
                       L"所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH * 2;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle   = L"选择音频文件";
    ofn.lpstrDefExt  = L"cbnd";

    if (!GetOpenFileNameW(&ofn)) return;

    if (g_audioDevOpen && g_paused) waveOutRestart(g_audioDev.hwo);
    InterlockedExchange(&g_paused, 0);
    SetWindowTextW(g_hPause, L"\u275A\u275A");
    InvalidateRect(g_hPause, NULL, FALSE);

    if (g_playingState) {
        InterlockedExchange(&g_stopRequest, 1);
        for (int i = 0; i < 200 && g_playingState; i++) Sleep(5);
    }
    InterlockedExchange(&g_stopRequest, 0);

    LONG myGen = InterlockedIncrement(&g_currentGen);
    g_playingIdx = -1;
    InvalidateRect(g_hList, NULL, FALSE);

    LocalPlayRequest *req = (LocalPlayRequest *)malloc(sizeof(LocalPlayRequest));
    req->hwnd = hwnd;
    req->path = _wcsdup(path);
    const wchar_t *base = wcsrchr(path, L'\\');
    if (!base) base = wcsrchr(path, L'/');
    if (base) base++; else base = path;
    req->title = _wcsdup(base);
    if (req->title) {
        wchar_t *dot = wcsrchr(req->title, L'.');
        if (dot) *dot = 0;
    }
    req->gen = myGen;

    HANDLE h = CreateThread(NULL, 0, PlayLocalThread, req, 0, NULL);
    if (h) CloseHandle(h);
    else { free(req->path); free(req->title); free(req); }
}

static void start_play_idx(HWND hwnd, int idx)
{
    if (idx < 0 || idx >= (int)g_songCount) return;
    if (g_audioDevOpen && g_paused) waveOutRestart(g_audioDev.hwo);
    InterlockedExchange(&g_paused, 0);
    SetWindowTextW(g_hPause, L"\u275A\u275A");
    InvalidateRect(g_hPause, NULL, FALSE);

    if (g_playingState) {
        InterlockedExchange(&g_stopRequest, 1);
        for (int i = 0; i < 200 && g_playingState; i++) Sleep(5);
    }
    InterlockedExchange(&g_stopRequest, 0);

    LONG myGen = InterlockedIncrement(&g_currentGen);
    g_playingIdx = idx;
    InvalidateRect(g_hList, NULL, FALSE);

    PlayRequest *req = (PlayRequest *)malloc(sizeof(PlayRequest));
    req->hwnd  = hwnd;
    req->url   = _strdup(g_songs[idx].url);
    {
        size_t tlen = strlen(g_songs[idx].name);
        if (tlen > 250) tlen = 250;
        req->title = (char *)malloc(tlen + 2);
        if (req->title) {
            memcpy(req->title, g_songs[idx].name, tlen);
            req->title[tlen]     = 0;
            req->title[tlen + 1] = 0;
        } else req->title = _strdup("");
    }
    req->gen = myGen;

    HANDLE h = CreateThread(NULL, 0, PlayThread, req, 0, NULL);
    if (h) CloseHandle(h);
    else { free(req->url); free(req->title); free(req); g_playingIdx = -1; }
}

static void set_dark_titlebar(HWND hwnd)
{
    if (!g_pDwmSet) return;
    BOOL dark = TRUE;
    if (FAILED(g_pDwmSet(hwnd, MY_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        g_pDwmSet(hwnd, MY_DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1, &dark, sizeof(dark));
    COLORREF capColor = C_BG_HEADER;
    g_pDwmSet(hwnd, MY_DWMWA_CAPTION_COLOR, &capColor, sizeof(capColor));
    COLORREF txtColor = C_TEXT;
    g_pDwmSet(hwnd, MY_DWMWA_TEXT_COLOR, &txtColor, sizeof(txtColor));
}

/* ==========================================================================
 *  主窗口过程
 * ========================================================================== */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandle(NULL);

        g_fontTitle  = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_fontBold   = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        g_fontSmall  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        g_searchBgBrush = CreateSolidBrush(C_BG_INPUT);

        g_hViewport = CreateWindowExW(0, L"CbndViewport", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            420, SEARCH_AREA_H, 700, 400, hwnd, NULL, hi, NULL);

        g_hList = CreateWindowExW(0, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | LVS_REPORT |
            LVS_SINGLESEL | LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER |
            LVS_NOSORTHEADER | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, g_hViewport, (HMENU)IDC_LIST, hi, NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(g_hList, C_BG);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        if (g_pSetTheme) g_pSetTheme(g_hList, L"DarkMode_Explorer", NULL);

        LVCOLUMNW col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = L""; col.cx = 800;
        ListView_InsertColumn(g_hList, 0, &col);

        g_oldListProc = (WNDPROC)SetWindowLongPtrW(g_hList, GWLP_WNDPROC,
                                                   (LONG_PTR)ListProc);

        g_hWaterfall = CreateWindowExW(0, L"CbndWaterfall", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, hi, NULL);

        g_hSearch = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SEARCH, hi, NULL);
        SendMessageW(g_hSearch, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        SendMessageW(g_hSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(8, 8));
        g_oldSearchProc = (WNDPROC)SetWindowLongPtrW(g_hSearch, GWLP_WNDPROC,
                                                     (LONG_PTR)SearchEditProc);

        g_hModes = CreateWindowExW(0, L"CbndModes", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_MODES, hi, NULL);

        g_hProgress = CreateWindowExW(0, L"CbndProgress", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_PROGRESS, hi, NULL);

        g_hSpeed = CreateWindowExW(0, L"CbndSpeed", L"",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_SPEED, hi, NULL);

        g_hOpenFile = CreateWindowW(L"BUTTON", L"打开本地文件",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_OPENFILE, hi, NULL);
        SendMessageW(g_hOpenFile, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

        g_hHelp = CreateWindowW(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_HELPTIP, hi, NULL);
        SendMessageW(g_hHelp, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

        g_hPause = CreateWindowW(L"BUTTON", L"\u25B6",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDC_PAUSE, hi, NULL);
        SendMessageW(g_hPause, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

        /* 帮助窗口：WS_POPUP + owned window，一定在主窗口之上 */
        g_hHelpOverlay = CreateWindowExW(WS_EX_TOOLWINDOW, L"CbndHelpOverlay",
            L"使用帮助",
            WS_POPUP | WS_BORDER,
            0, 0, 560, 560, hwnd, NULL, hi, NULL);

        HANDLE h1 = CreateThread(NULL, 0, LoadPlaylistThread, hwnd, 0, NULL);
        if (h1) CloseHandle(h1);
        HANDLE h2 = CreateThread(NULL, 0, LoadSamplesThread, NULL, 0, NULL);
        if (h2) CloseHandle(h2);

        SetTimer(hwnd, 1, 100, NULL);       /* 时间/进度 */
        SetTimer(hwnd, 3, 8, NULL);         /* 瀑布图刷新，配合 timeBeginPeriod 达 ~120fps */
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SIZE:
        layout_controls(hwnd);
        /* 主窗口改变大小 → 关闭帮助窗口（避免位置错乱） */
        if (g_hHelpOverlay && g_showHelp) {
            g_showHelp = 0;
            ShowWindow(g_hHelpOverlay, SW_HIDE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 760;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_ERASEBKGND: return 1;

    case WM_KEYDOWN: {
        if (GetFocus() == g_hSearch) break;

        switch (wp) {
        case VK_SPACE:
            if (g_playingState) {
                SendMessageW(hwnd, WM_COMMAND, IDC_PAUSE, 0);
            } else if (g_filteredCount > 0) {
                start_play_filtered(hwnd, 0);
            }
            return 0;

        case VK_LEFT: {
            /* 按住 → 倒放（bit 30 为 0 表示首次按下） */
            if (!g_playingState) return 0;
            if (((lp >> 30) & 1) == 0) {
                InterlockedExchange(&g_reverse, 1);
                wcscpy(g_statusText, L"倒放中…（松开恢复）");
                InvalidateRect(hwnd, &g_statusRect, FALSE);
            }
            return 0;
        }

        case VK_RIGHT: {
            /* 按住 → 2 倍速 */
            if (!g_playingState) return 0;
            if (((lp >> 30) & 1) == 0) {
                g_savedSpeed = g_speedVal;   /* 记下当前速度 */
                InterlockedExchange(&g_speedMilli, 2000);
                g_speedVal = 2.0;
                InvalidateRect(g_hSpeed, NULL, FALSE);
                wcscpy(g_statusText, L"2 倍速（松开恢复）");
                InvalidateRect(hwnd, &g_statusRect, FALSE);
            }
            return 0;
        }

        case VK_UP: {
            /* 上一首 */
            if (g_playingIdx < 0 || g_songCount == 0) return 0;
            int prev;
            if (g_playMode == MODE_RANDOM && g_songCount > 1) {
                do { prev = rand() % (int)g_songCount; } while (prev == g_playingIdx);
            } else if (g_playingIdx == 0) {
                prev = (int)g_songCount - 1;
            } else {
                prev = g_playingIdx - 1;
            }
            start_play_idx(hwnd, prev);
            return 0;
        }

        case VK_DOWN: {
            /* 下一首 */
            if (g_playingIdx < 0 || g_songCount == 0) return 0;
            int nxt;
            if (g_playMode == MODE_RANDOM && g_songCount > 1) {
                do { nxt = rand() % (int)g_songCount; } while (nxt == g_playingIdx);
            } else {
                nxt = (g_playingIdx + 1) % (int)g_songCount;
            }
            start_play_idx(hwnd, nxt);
            return 0;
        }

        case VK_ESCAPE:
            if (g_showHelp) {
                g_showHelp = 0;
                if (g_hHelpOverlay) ShowWindow(g_hHelpOverlay, SW_HIDE);
                return 0;
            }
            if (g_showError) { clear_error(); return 0; }
            break;
        }
        break;
    }

    case WM_KEYUP: {
        if (GetFocus() == g_hSearch) break;
        switch (wp) {
        case VK_LEFT:
            /* 松开左键 → 恢复正放 */
            if (g_reverse) {
                InterlockedExchange(&g_reverse, 0);
                if (g_playingState) {
                    wcscpy(g_statusText, L"正常播放");
                    InvalidateRect(hwnd, &g_statusRect, FALSE);
                }
            }
            return 0;
        case VK_RIGHT:
            /* 松开右键 → 恢复原速 */
            if (g_speedMilli != 1000) {
                LONG nv = (LONG)(g_savedSpeed * 1000);
                if (nv < 100) nv = 100;
                if (nv > 5000) nv = 5000;
                InterlockedExchange(&g_speedMilli, nv);
                g_speedVal = nv / 1000.0;
                InvalidateRect(g_hSpeed, NULL, FALSE);
                if (g_playingState) {
                    wchar_t b[64];
                    swprintf(b, 64, L"速度 %.2fx", g_speedVal);
                    wcscpy(g_statusText, b);
                    InvalidateRect(hwnd, &g_statusRect, FALSE);
                }
            }
            return 0;
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        wchar_t path[MAX_PATH * 2] = L"";
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH * 2) > 0) {
            /* 复用 open_local_file 后半段逻辑 */
            if (g_audioDevOpen && g_paused) waveOutRestart(g_audioDev.hwo);
            InterlockedExchange(&g_paused, 0);
            SetWindowTextW(g_hPause, L"\u275A\u275A");
            InvalidateRect(g_hPause, NULL, FALSE);
            if (g_playingState) {
                InterlockedExchange(&g_stopRequest, 1);
                for (int i = 0; i < 200 && g_playingState; i++) Sleep(5);
            }
            InterlockedExchange(&g_stopRequest, 0);
            LONG myGen = InterlockedIncrement(&g_currentGen);
            g_playingIdx = -1;
            InvalidateRect(g_hList, NULL, FALSE);
            LocalPlayRequest *req = (LocalPlayRequest *)malloc(sizeof(LocalPlayRequest));
            req->hwnd = hwnd;
            req->path = _wcsdup(path);
            const wchar_t *base = wcsrchr(path, L'\\');
            if (!base) base = wcsrchr(path, L'/');
            if (base) base++; else base = path;
            req->title = _wcsdup(base);
            if (req->title) {
                wchar_t *dot = wcsrchr(req->title, L'.');
                if (dot) *dot = 0;
            }
            req->gen = myGen;
            HANDLE h = CreateThread(NULL, 0, PlayLocalThread, req, 0, NULL);
            if (h) CloseHandle(h);
            else { free(req->path); free(req->title); free(req); }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (g_showHelp) {
            g_showHelp = 0;
            if (g_hHelpOverlay) ShowWindow(g_hHelpOverlay, SW_HIDE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_showError) {
            RECT rc; GetClientRect(hwnd, &rc);
            int rw = calc_right_width(rc.right);
            int pl = rc.right - rw;
            RECT errRc = { pl + 10, TOPBAR_H + SEARCH_AREA_H + 10,
                           rc.right - 10, TOPBAR_H + SEARCH_AREA_H + 100 };
            int mx = (short)LOWORD(lp);
            int my = (short)HIWORD(lp);
            if (mx >= errRc.left && mx <= errRc.right &&
                my >= errRc.top  && my <= errRc.bottom) {
                /* 检查是否点到"重试"区域（右侧下角 60px 宽，24px 高） */
                int rbLeft = errRc.right - 72;
                int rbTop  = errRc.bottom - 28;
                if (g_retry.type != 0 &&
                    mx >= rbLeft && mx <= errRc.right - 8 &&
                    my >= rbTop  && my <= errRc.bottom - 4) {
                    int t = g_retry.type;
                    char *u = g_retry.url;
                    g_retry.type = 0;
                    g_retry.url = NULL;
                    clear_error();
                    if (t == 1) {
                        /* 重试歌单 */
                        HANDLE h = CreateThread(NULL, 0, LoadPlaylistThread, hwnd, 0, NULL);
                        if (h) CloseHandle(h);
                        wcscpy(g_statusText, L"正在加载...");
                        InvalidateRect(hwnd, &g_statusRect, FALSE);
                    } else if (t == 2 && u) {
                        /* 重试在线歌曲 */
                        LONG myGen = InterlockedIncrement(&g_currentGen);
                        PlayRequest *req = (PlayRequest *)malloc(sizeof(PlayRequest));
                        req->hwnd  = hwnd;
                        req->url   = u;
                        req->title = _strdup("");
                        req->gen   = myGen;
                        HANDLE h = CreateThread(NULL, 0, PlayThread, req, 0, NULL);
                        if (h) CloseHandle(h);
                    } else {
                        free(u);
                    }
                    return 0;
                }
                clear_error();
                return 0;
            }
        }
        break;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, C_TEXT);
        SetBkColor(hdc, C_BG_INPUT);
        return (LRESULT)g_searchBgBrush;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bb = CreateSolidBrush(C_BG);
        FillRect(hdc, &rc, bb);
        DeleteObject(bb);

        int rightW = calc_right_width(rc.right);
        int leftW  = rc.right - rightW;

        /* 左侧底部控制区背景 */
        RECT ctrl = { 0, rc.bottom - CTRL_H, leftW, rc.bottom };
        HBRUSH cb = CreateSolidBrush(C_BG_CTRL);
        FillRect(hdc, &ctrl, cb);
        DeleteObject(cb);

        /* 顶部两条标题栏背景 */
        RECT tbL = { 0, 0, leftW, TOPBAR_H };
        HBRUSH tbLB = CreateSolidBrush(C_BG_HEADER);
        FillRect(hdc, &tbL, tbLB);
        DeleteObject(tbLB);

        RECT tbR = { leftW, 0, rc.right, TOPBAR_H };
        HBRUSH tbRB = CreateSolidBrush(C_BG_HEADER);
        FillRect(hdc, &tbR, tbRB);
        DeleteObject(tbRB);

        /* 标题栏分隔线：左右分界 + 底部分界 */
        HPEN tsep = CreatePen(PS_SOLID, 1, RGB(0x50, 0x40, 0x10));
        HPEN otsep = (HPEN)SelectObject(hdc, tsep);
        MoveToEx(hdc, leftW, 0, NULL);
        LineTo(hdc, leftW, TOPBAR_H);
        MoveToEx(hdc, 0, TOPBAR_H - 1, NULL);
        LineTo(hdc, rc.right, TOPBAR_H - 1);
        /* 标题栏金色装饰线（左侧） */
        SelectObject(hdc, otsep);
        DeleteObject(tsep);

        HPEN accSep = CreatePen(PS_SOLID, 2, C_ACCENT);
        HPEN oacc = (HPEN)SelectObject(hdc, accSep);
        MoveToEx(hdc, 12, TOPBAR_H - 2, NULL);
        LineTo(hdc, leftW - 12, TOPBAR_H - 2);
        MoveToEx(hdc, leftW + 12, TOPBAR_H - 2, NULL);
        LineTo(hdc, rc.right - 12, TOPBAR_H - 2);
        SelectObject(hdc, oacc);
        DeleteObject(accSep);

        /* 右侧顶部搜索栏背景（下移 TOPBAR_H） */
        RECT srchArea = { leftW, TOPBAR_H, rc.right, TOPBAR_H + SEARCH_AREA_H };
        HBRUSH sab = CreateSolidBrush(C_BG_HEADER);
        FillRect(hdc, &srchArea, sab);
        DeleteObject(sab);

        SetBkMode(hdc, TRANSPARENT);

        /* 标题栏文字 */
        HFONT oldT = (HFONT)SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, C_TEXT);
        RECT tL = { 16, 0, leftW - 16, TOPBAR_H };
        DrawTextW(hdc, L"Minecraft 音乐播放器", -1, &tL,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT tR = { leftW + 16, 0, rc.right - 16, TOPBAR_H };
        DrawTextW(hdc, L"在线歌单", -1, &tR,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, oldT);

        HFONT old = (HFONT)SelectObject(hdc, g_fontSmall);

        SetTextColor(hdc, C_TEXT_SUB);
        RECT sr = g_statusRect;
        DrawTextW(hdc, g_statusText, -1, &sr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SetTextColor(hdc, C_TEXT);
        RECT tr3 = g_timeRect;
        DrawTextW(hdc, g_timeText, -1, &tr3,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, old);

        /* 搜索框圆角背景 */
        if (g_hSearch) {
            RECT src;
            GetWindowRect(g_hSearch, &src);
            MapWindowPoints(NULL, hwnd, (LPPOINT)&src, 2);
            RECT r = src;
            r.left -= 2; r.top -= 2; r.right += 2; r.bottom += 2;
            HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, 10, 10);
            HBRUSH br = CreateSolidBrush(C_BG_INPUT);
            FillRgn(hdc, rgn, br);
            DeleteObject(br);
            DeleteObject(rgn);
        }

        /* 错误提示覆盖层（右侧面板顶部） */
        if (g_showError && g_errorMsg[0]) {
            int rw = calc_right_width(rc.right);
            int pl = rc.right - rw;
            RECT errRc = { pl + 10, SEARCH_AREA_H + 10,
                           rc.right - 10, SEARCH_AREA_H + 100 };

            HBRUSH eb = CreateSolidBrush(RGB(0x50, 0x10, 0x10));
            FillRect(hdc, &errRc, eb);
            DeleteObject(eb);

            HPEN ep = CreatePen(PS_SOLID, 1, RGB(0xff, 0x50, 0x50));
            HPEN oep = (HPEN)SelectObject(hdc, ep);
            HBRUSH obr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, errRc.left, errRc.top, errRc.right, errRc.bottom);
            SelectObject(hdc, obr);
            SelectObject(hdc, oep);
            DeleteObject(ep);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0xff, 0xa0, 0xa0));
            HFONT old2 = (HFONT)SelectObject(hdc, g_fontSmall);
            RECT tr = errRc;
            tr.left += 12; tr.right -= 12;
            tr.top += 4;   tr.bottom -= 20;
            DrawTextW(hdc, g_errorMsg, -1, &tr,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, old2);

            SetTextColor(hdc, RGB(0xff, 0x70, 0x70));
            old2 = (HFONT)SelectObject(hdc, g_fontSmall);
            RECT xr = errRc;
            xr.right -= 12;
            xr.bottom -= 4;
            if (g_retry.type == 0) {
                DrawTextW(hdc, L"（单击关闭）", -1, &xr,
                          DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
            } else {
                DrawTextW(hdc, L"（单击关闭）", -1, &xr,
                          DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
                /* 重试按钮 */
                RECT rb = errRc;
                rb.left   = errRc.right - 72;
                rb.top    = errRc.bottom - 28;
                rb.right  = errRc.right - 8;
                rb.bottom = errRc.bottom - 4;
                HBRUSH rbb = CreateSolidBrush(RGB(0x80, 0x30, 0x30));
                HRGN rr = CreateRoundRectRgn(rb.left, rb.top, rb.right + 1, rb.bottom + 1, 6, 6);
                FillRgn(hdc, rr, rbb);
                DeleteObject(rr);
                DeleteObject(rbb);
                SetTextColor(hdc, RGB(0xff, 0xff, 0xff));
                DrawTextW(hdc, L"重试", -1, &rb,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(hdc, old2);
        }

        /* 帮助面板（已由覆盖窗口绘制，此处禁用） */
        if (0 && g_showHelp) {
            int pw = 560, ph = 540;
            int px = (rc.right - pw) / 2;
            int py = (rc.bottom - ph) / 2;
            if (px < 20) px = 20;
            if (py < 20) py = 20;
            if (px + pw > rc.right - 20) pw = rc.right - px - 20;
            if (py + ph > rc.bottom - 20) ph = rc.bottom - py - 20;
            if (pw < 300) pw = 300;
            if (ph < 300) ph = 300;

            RECT pr = { px, py, px + pw, py + ph };

            /* 背景 + 边框 */
            HBRUSH hb = CreateSolidBrush(RGB(0x18, 0x18, 0x18));
            FillRect(hdc, &pr, hb);
            DeleteObject(hb);
            HPEN hp = CreatePen(PS_SOLID, 2, C_ACCENT);
            HPEN ohp = (HPEN)SelectObject(hdc, hp);
            HBRUSH obh = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, px, py, px + pw, py + ph);
            SelectObject(hdc, obh);
            SelectObject(hdc, ohp);
            DeleteObject(hp);

            SetBkMode(hdc, TRANSPARENT);

            /* 标题 */
            SetTextColor(hdc, C_ACCENT_HI);
            HFONT oldF = (HFONT)SelectObject(hdc, g_fontBold);
            RECT tr = { px + 24, py + 10, px + pw - 24, py + 44 };
            DrawTextW(hdc, L"使用帮助", -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldF);

            /* 分隔线 */
            HPEN sep = CreatePen(PS_SOLID, 1, RGB(0x60, 0x50, 0x20));
            HPEN osep = (HPEN)SelectObject(hdc, sep);
            MoveToEx(hdc, px + 16, py + 48, NULL);
            LineTo(hdc, px + pw - 16, py + 48);
            SelectObject(hdc, osep);
            DeleteObject(sep);

            /* 内容 */
            SetTextColor(hdc, RGB(0xE0, 0xE0, 0xE0));
            oldF = (HFONT)SelectObject(hdc, g_fontSmall);
            int yy = py + 58;
            int lineH = 19;
            int yLimit = py + ph - 34;
            for (int i = 0; HELP_LINES[i]; i++) {
                if (yy + lineH > yLimit) break;
                RECT lr = { px + 24, yy, px + pw - 24, yy + lineH };
                DrawTextW(hdc, HELP_LINES[i], -1, &lr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                yy += lineH;
            }
            SelectObject(hdc, oldF);

            /* 底部提示 */
            SetTextColor(hdc, C_ACCENT);
            oldF = (HFONT)SelectObject(hdc, g_fontSmall);
            RECT xr = { px + 24, py + ph - 28, px + pw - 24, py + ph - 8 };
            DrawTextW(hdc, L"点击任意处关闭", -1, &xr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, oldF);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lp;
        if (mis->CtlID == IDC_LIST) { mis->itemHeight = CARD_H; return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;

        /* 帮助按钮：画一个圆形 + 问号 */
        if (dis->CtlID == IDC_HELPTIP) {
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            int pressed = (dis->itemState & ODS_SELECTED) != 0;

            HBRUSH bgBrush = CreateSolidBrush(C_BG_CTRL);
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);

            int cx = (rc.left + rc.right) / 2;
            int cy = (rc.top + rc.bottom) / 2;
            int r  = (rc.bottom - rc.top) / 2 - 4;
            if (r > 14) r = 14;
            if (r < 8)  r = 8;

            HBRUSH circle = CreateSolidBrush(pressed ? C_ACCENT_HI : C_ACCENT);
            HBRUSH ob = (HBRUSH)SelectObject(hdc, circle);
            HPEN op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(circle);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0x18, 0x18, 0x18));
            HFONT old = (HFONT)SelectObject(hdc, g_fontBold);
            DrawTextW(hdc, L"?", -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            return TRUE;
        }

        if (dis->CtlID == IDC_PAUSE || dis->CtlID == IDC_OPENFILE) {
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            int pressed = (dis->itemState & ODS_SELECTED) != 0;
            int isPause = (dis->CtlID == IDC_PAUSE);
            HBRUSH bgBrush = CreateSolidBrush(C_BG_CTRL);
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);
            COLORREF bg;
            if (isPause) bg = pressed ? C_ACCENT_HI : C_ACCENT;
            else         bg = pressed ? C_CARD_HOV : C_BG_INPUT;
            HRGN rgn = CreateRoundRectRgn(rc.left, rc.top,
                                          rc.right + 1, rc.bottom + 1, 10, 10);
            HBRUSH br = CreateSolidBrush(bg);
            FillRgn(hdc, rgn, br);
            DeleteObject(br);
            DeleteObject(rgn);
            wchar_t txt[64];
            GetWindowTextW(dis->hwndItem, txt, 64);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0xff, 0xff, 0xff));
            HFONT old = (HFONT)SelectObject(hdc, isPause ? g_fontBold : g_fontSmall);
            DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            return TRUE;
        }

        if (dis->CtlID != IDC_LIST) break;
        int filteredIdx = (int)dis->itemID;

        /* ---------- 列表末尾的链接行 ---------- */
        if (filteredIdx >= g_filteredCount &&
            filteredIdx < g_filteredCount + INFO_ROW_COUNT) {
            int infoIdx = filteredIdx - g_filteredCount;

            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            int isHover = (filteredIdx == g_hoverIdx);

            HBRUSH listbg = CreateSolidBrush(C_BG);
            FillRect(hdc, &rc, listbg);
            DeleteObject(listbg);

            RECT card = rc;
            card.left += 8; card.right -= 8;
            card.top += 4; card.bottom -= 4;

            COLORREF cardbg = isHover ? RGB(0x25, 0x25, 0x1a) : RGB(0x14, 0x14, 0x14);
            HRGN rgn = CreateRoundRectRgn(card.left, card.top,
                                          card.right + 1, card.bottom + 1, 10, 10);
            HBRUSH br = CreateSolidBrush(cardbg);
            FillRgn(hdc, rgn, br);
            DeleteObject(br);
            DeleteObject(rgn);

            /* 左侧外链图标 */
            int icoX = card.left + 14;
            int icoY = (card.top + card.bottom) / 2;
            HPEN ip = CreatePen(PS_SOLID, 2, isHover ? C_ACCENT_HI : C_ACCENT);
            HPEN oip = (HPEN)SelectObject(hdc, ip);
            HBRUSH oib = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, icoX - 5, icoY - 5, icoX + 3, icoY + 3);
            MoveToEx(hdc, icoX - 1, icoY + 1, NULL);
            LineTo(hdc, icoX + 6, icoY - 6);
            MoveToEx(hdc, icoX + 2, icoY - 6, NULL);
            LineTo(hdc, icoX + 6, icoY - 6);
            MoveToEx(hdc, icoX + 6, icoY - 6, NULL);
            LineTo(hdc, icoX + 6, icoY - 2);
            SelectObject(hdc, oib);
            SelectObject(hdc, oip);
            DeleteObject(ip);

            /* 链接文字 */
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, isHover ? C_ACCENT_HI : C_ACCENT);
            HFONT old = (HFONT)SelectObject(hdc, g_fontSmall);
            RECT tr = { card.left + 30, card.top, card.right - 16, card.bottom };
            DrawTextW(hdc, INFO_LABELS[infoIdx], -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
            return TRUE;
        }

        if (filteredIdx < 0 || filteredIdx >= g_filteredCount) return TRUE;
        int realIdx = g_filteredIdx[filteredIdx];

        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        int isHover   = (filteredIdx == g_hoverIdx);
        int isPlaying = (realIdx == g_playingIdx);

        HBRUSH listbg = CreateSolidBrush(C_BG);
        FillRect(hdc, &rc, listbg);
        DeleteObject(listbg);

        RECT card = rc;
        card.left += 8; card.right -= 8;
        card.top += 4; card.bottom -= 4;

        COLORREF bg = isPlaying ? C_CARD_PLAY : (isHover ? C_CARD_HOV : C_CARD);
        HRGN rgn = CreateRoundRectRgn(card.left, card.top,
                                      card.right + 1, card.bottom + 1, 10, 10);
        HBRUSH br = CreateSolidBrush(bg);
        FillRgn(hdc, rgn, br);
        DeleteObject(br);
        DeleteObject(rgn);

        /* 正在播放：呼吸边框 */
        if (isPlaying) {
            DWORD tms = GetTickCount();
            double phase = (double)(tms % 1500) / 1500.0;
            double bright = 0.5 + 0.5 * sin(phase * 2 * 3.14159265);
            int rb = (int)(0x40 + 0xC0 * bright);
            if (rb > 0xff) rb = 0xff;
            COLORREF bcol = RGB(rb, (int)(rb * 0.7), 0x10);
            HPEN bp = CreatePen(PS_SOLID, 2, bcol);
            HPEN obp = (HPEN)SelectObject(hdc, bp);
            HBRUSH obb = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, card.left + 1, card.top + 1,
                      card.right - 1, card.bottom - 1, 10, 10);
            SelectObject(hdc, obb);
            SelectObject(hdc, obp);
            DeleteObject(bp);
        }

        SetBkMode(hdc, TRANSPARENT);
        wchar_t idxbuf[16];
        swprintf(idxbuf, 16, L"%d", realIdx + 1);
        SetTextColor(hdc, isPlaying ? C_TEXT : C_MUTED);
        HFONT old = (HFONT)SelectObject(hdc, g_fontSmall);
        RECT ir = { card.left + 16, card.top, card.left + 60, card.bottom };
        DrawTextW(hdc, idxbuf, -1, &ir, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);

        wchar_t name_w[512];
        u8_to_w_buf(g_songs[realIdx].name, name_w, 512);
        SetTextColor(hdc, C_TEXT);
        old = (HFONT)SelectObject(hdc, isPlaying ? g_fontBold : g_fontNormal);
        RECT nr = { card.left + 70, card.top, card.right - 110, card.bottom };
        DrawTextW(hdc, name_w, -1, &nr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, old);

        wchar_t ext_w[16];
        u8_to_w_buf(g_songs[realIdx].ext, ext_w, 16);
        SetTextColor(hdc, isPlaying ? C_TEXT : C_TEXT_SUB);
        old = (HFONT)SelectObject(hdc, g_fontSmall);
        RECT er = { card.right - 100, card.top, card.right - 16, card.bottom };
        DrawTextW(hdc, ext_w, -1, &er, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == 1) {
            if (!g_progDragging && GetTickCount() - g_lastSeekTime > 300) {
                LONG total = g_totalTicks;
                LONG cur   = g_currentTick;
                double nv = total > 0 ? (double)cur / total : 0.0;
                if (nv < 0) nv = 0; if (nv > 1) nv = 1;
                if (fabs(nv - g_progValue) > 0.0005) {
                    g_progValue = nv;
                    InvalidateRect(g_hProgress, NULL, FALSE);
                }
            }
            LONG cur = g_curSec, tot = g_totalSec;
            if (cur < 0) cur = 0; if (tot < 0) tot = 0;
            wchar_t tb[64];
            swprintf(tb, 64, L"%d:%02d / %d:%02d",
                     cur / 60, cur % 60, tot / 60, tot % 60);
            if (wcscmp(tb, g_timeText) != 0) {
                wcscpy(g_timeText, tb);
                InvalidateRect(hwnd, &g_timeRect, FALSE);
            }
            /* 正在播放卡片微动效刷新 */
            if (g_playingIdx >= 0 && g_hList)
                InvalidateRect(g_hList, NULL, FALSE);
            return 0;
        }
        if (wp == 2) {
            int diff = g_scrollTarget - g_scrollPos;
            if (diff == 0) { KillTimer(hwnd, 2); g_scrollTimerOn = 0; return 0; }
            int step = diff / 4;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            g_scrollPos += step;
            if (g_hList && g_hViewport) {
                RECT vrc; GetClientRect(g_hViewport, &vrc);
                int contentH = g_scrollContentH + LISTVIEW_EXTRA_H;
                if (contentH < vrc.bottom) contentH = vrc.bottom;
                SetWindowPos(g_hList, NULL, 0, -g_scrollPos,
                             vrc.right + LISTVIEW_EXTRA_W, contentH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
                RECT barRc = { vrc.right - SCROLLBAR_W, 0, vrc.right, vrc.bottom };
                InvalidateRect(g_hViewport, &barRc, FALSE);
            }
            return 0;
        }
        if (wp == 3) {
            if ((g_playingState || g_progDragging) && g_hWaterfall)
                RedrawWindow(g_hWaterfall, NULL, NULL,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            return 0;
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == IDC_OPENFILE) {
            open_local_file(hwnd);
            return 0;
        }
        if (id == IDC_HELPTIP) {
            clear_error();
            if (!g_hHelpOverlay) return 0;
            if (g_showHelp) {
                g_showHelp = 0;
                ShowWindow(g_hHelpOverlay, SW_HIDE);
                return 0;
            }
            g_showHelp = 1;

            /* 位置：主窗口客户区居中 */
            RECT mr; GetWindowRect(hwnd, &mr);
            int mw = mr.right - mr.left;
            int mh = mr.bottom - mr.top;

            int pw = 560;
            int ph = 640;
            if (ph > mh - 40) ph = mh - 40;
            int px = mr.left + (mw - pw) / 2;
            int py = mr.top  + (mh - ph) / 2;
            if (px < mr.left + 10) px = mr.left + 10;
            if (py < mr.top  + 10) py = mr.top  + 10;

            SetWindowPos(g_hHelpOverlay, HWND_TOP,
                         px, py, pw, ph,
                         SWP_SHOWWINDOW);
            InvalidateRect(g_hHelpOverlay, NULL, TRUE);
            return 0;
        }
        if (id == IDC_SEARCH && code == EN_CHANGE) {
            GetWindowTextW(g_hSearch, g_searchText, 64);
            rebuild_filter();
            if (g_filteredCount == 0)
                wcscpy(g_statusText, L"没有匹配的歌曲");
            else if (wcslen(g_searchText) > 0)
                swprintf(g_statusText, 256, L"搜索到 %d 首歌曲", g_filteredCount);
            else
                swprintf(g_statusText, 256, L"加载成功，共 %d 首歌曲", g_filteredCount);
            InvalidateRect(hwnd, &g_statusRect, FALSE);
            return 0;
        }
        if (id == IDC_PAUSE) {
            if (!g_audioDevOpen || !g_playingState) {
                SetWindowTextW(g_hPause, L"\u25B6");
                InvalidateRect(g_hPause, NULL, FALSE);
                return 0;
            }
            if (g_paused) {
                waveOutRestart(g_audioDev.hwo);
                InterlockedExchange(&g_paused, 0);
                SetWindowTextW(g_hPause, L"\u275A\u275A");
                swprintf(g_statusText, 256, L"正在播放：%ls", g_currentPlayingName);
            } else {
                waveOutPause(g_audioDev.hwo);
                InterlockedExchange(&g_paused, 1);
                SetWindowTextW(g_hPause, L"\u25B6");
                wcscpy(g_statusText, L"已暂停");
            }
            InvalidateRect(g_hPause, NULL, FALSE);
            InvalidateRect(hwnd, &g_statusRect, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == NM_CLICK) {
            LPNMITEMACTIVATE nia = (LPNMITEMACTIVATE)lp;
            int idx = nia->iItem;

            /* 点击了末尾链接行 → 打开浏览器 */
            if (idx >= g_filteredCount &&
                idx < g_filteredCount + INFO_ROW_COUNT) {
                int i = idx - g_filteredCount;
                ShellExecuteW(NULL, L"open", INFO_URLS[i],
                              NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }

            if (idx >= 0 && idx < g_filteredCount)
                start_play_filtered(hwnd, idx);
        }
        return 0;
    }
    case WM_APP_ERROR: {
        wchar_t *msg = (wchar_t *)lp;
        if (msg) {
            show_error(msg);
            free(msg);
        }
        return 0;
    }
    case WM_APP_SET_WF: {
        size_t n = (size_t)wp;
        Note *arr = (Note *)lp;
        if (g_wfNotes) { free(g_wfNotes); g_wfNotes = NULL; }
        g_wfNotesN = 0;
        if (n > 0 && arr) {
            g_wfNotes = arr;
            g_wfNotesN = n;
            qsort(g_wfNotes, g_wfNotesN, sizeof(Note), cmp_wf_note);
            int mn = 127, mx = 0;
            for (size_t i = 0; i < n; i++) {
                if (arr[i].key < mn) mn = arr[i].key;
                if (arr[i].key > mx) mx = arr[i].key;
            }
            mn -= 3; mx += 3;
            if (mn < 0) mn = 0;
            if (mx > 127) mx = 127;
            if (mx - mn < 6) {
                int mid = (mn + mx) / 2;
                mn = mid - 3; mx = mid + 3;
                if (mn < 0) mn = 0;
                if (mx > 127) mx = 127;
            }
            g_wfMinKey = mn;
            g_wfMaxKey = mx;
        }
        if (g_hWaterfall) InvalidateRect(g_hWaterfall, NULL, FALSE);
        return 0;
    }
    case WM_APP_STATUS_SET: {
        wchar_t *s = (wchar_t *)lp;
        if (wp == 0xFFFF) { free(s); return 0; }
        if (wp == 0xAA) {
            if (s) {
                int i;
                for (i = 0; i < 255 && s[i]; i++) g_currentPlayingName[i] = s[i];
                g_currentPlayingName[i] = 0;
                free(s);
            }
            return 0;
        }
        if (s) {
            int i;
            for (i = 0; i < 255 && s[i]; i++) g_statusText[i] = s[i];
            g_statusText[i] = 0;
            InvalidateRect(hwnd, &g_statusRect, FALSE);
            free(s);
        }
        return 0;
    }
    case WM_APP_PLAYLIST_READY: {
        size_t n = (size_t)wp;
        SongEntry *list = (SongEntry *)lp;
        if (!list || n == 0) {
            wcscpy(g_statusText, L"歌单加载失败");
            InvalidateRect(hwnd, &g_statusRect, FALSE);
            free(g_retry.url);
            g_retry.type = 1;
            g_retry.url = NULL;
            show_error(
                L"无法从 GitHub 加载在线歌单。\n\n"
                L"可能原因：\n"
                L"  • 网络无法访问 github.io\n"
                L"  • 需要代理或 VPN\n"
                L"  • 歌单地址已变更\n\n"
                L"你仍可以用「打开本地文件」按钮播放本地歌曲。");
            free_playlist(list, n);
            return 0;
        }
        g_songs = list;
        g_songCount = n;
        rebuild_filter();
        swprintf(g_statusText, 256, L"加载成功，共 %d 首歌曲", g_filteredCount);
        InvalidateRect(hwnd, &g_statusRect, FALSE);
        return 0;
    }
    case WM_APP_PLAY_DONE: {
        LONG gen = (LONG)wp;
        LONG natural = (LONG)lp;
        if (gen == g_currentGen) {
            InterlockedExchange(&g_stopRequest, 0);
            InterlockedExchange(&g_paused, 0);
            SetWindowTextW(g_hPause, L"\u25B6");
            InvalidateRect(g_hPause, NULL, FALSE);
            if (natural && g_playingIdx >= 0 && g_songCount > 0) {
                int next = pick_next_song(g_playingIdx);
                if (next >= 0) { start_play_idx(hwnd, next); return 0; }
            }
            g_playingIdx = -1;
            wcscpy(g_statusText, L"播放完毕");
            InvalidateRect(hwnd, &g_statusRect, FALSE);
            InvalidateRect(g_hList, NULL, FALSE);
        }
        return 0;
    }
    case WM_DESTROY: {
        InterlockedExchange(&g_stopRequest, 1);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        timeEndPeriod(1);
        free_playlist(g_songs, g_songCount);
        g_songs = NULL; g_songCount = 0;
        free(g_filteredIdx); g_filteredIdx = NULL;
        if (g_wfNotes) { free(g_wfNotes); g_wfNotes = NULL; g_wfNotesN = 0; }
        for (int i = 0; i < 16; i++) {
            if (g_samples[i].pcm) free(g_samples[i].pcm);
            g_samples[i].pcm = NULL;
        }
        if (g_fontTitle)  DeleteObject(g_fontTitle);
        if (g_fontNormal) DeleteObject(g_fontNormal);
        if (g_fontBold)   DeleteObject(g_fontBold);
        if (g_fontSmall)  DeleteObject(g_fontSmall);
        if (g_searchBgBrush) DeleteObject(g_searchBgBrush);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  WinMain
 * ========================================================================== */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow)
{
    (void)hPrev; (void)cmd;
    init_optional_apis();
    srand((unsigned)GetTickCount());
    timeBeginPeriod(1);   /* 提高 WM_TIMER 精度到 1ms */

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW vc = {0};
    vc.cbSize        = sizeof(vc);
    vc.style         = CS_HREDRAW | CS_VREDRAW;
    vc.lpfnWndProc   = ViewportProc;
    vc.hInstance     = hInst;
    vc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    vc.hbrBackground = NULL;
    vc.lpszClassName = L"CbndViewport";
    RegisterClassExW(&vc);

    WNDCLASSEXW wfc = {0};
    wfc.cbSize        = sizeof(wfc);
    wfc.style         = CS_HREDRAW | CS_VREDRAW;
    wfc.lpfnWndProc   = WaterfallProc;
    wfc.hInstance     = hInst;
    wfc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wfc.hbrBackground = NULL;
    wfc.lpszClassName = L"CbndWaterfall";
    RegisterClassExW(&wfc);

    WNDCLASSEXW hoc = {0};
    hoc.cbSize        = sizeof(hoc);
    hoc.style         = CS_HREDRAW | CS_VREDRAW;
    hoc.lpfnWndProc   = HelpOverlayProc;
    hoc.hInstance     = hInst;
    hoc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    hoc.hbrBackground = NULL;
    hoc.lpszClassName = L"CbndHelpOverlay";
    RegisterClassExW(&hoc);

    WNDCLASSEXW mc = {0};
    mc.cbSize        = sizeof(mc);
    mc.style         = CS_HREDRAW | CS_VREDRAW;
    mc.lpfnWndProc   = ModesProc;
    mc.hInstance     = hInst;
    mc.hCursor       = LoadCursor(NULL, IDC_HAND);
    mc.hbrBackground = NULL;
    mc.lpszClassName = L"CbndModes";
    RegisterClassExW(&mc);

    WNDCLASSEXW pc = {0};
    pc.cbSize        = sizeof(pc);
    pc.style         = CS_HREDRAW | CS_VREDRAW;
    pc.lpfnWndProc   = ProgressProc;
    pc.hInstance     = hInst;
    pc.hCursor       = LoadCursor(NULL, IDC_HAND);
    pc.hbrBackground = NULL;
    pc.lpszClassName = L"CbndProgress";
    RegisterClassExW(&pc);

    WNDCLASSEXW sc = {0};
    sc.cbSize        = sizeof(sc);
    sc.style         = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc   = SpeedProc;
    sc.hInstance     = hInst;
    sc.hCursor       = LoadCursor(NULL, IDC_HAND);
    sc.hbrBackground = NULL;
    sc.lpszClassName = L"CbndSpeed";
    RegisterClassExW(&sc);

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"CbndPlayerWnd";
    wc.hIcon         = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 16, 16, 0);
    if (!wc.hIcon)   wc.hIcon   = LoadIcon(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"CbndPlayerWnd",
        L"Minecraft 音乐播放器  by ZeroRaltal",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    g_hMain = hwnd;
    set_dark_titlebar(hwnd);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* 键盘快捷键：把关心的键从子控件转发给主窗口（搜索框除外） */
        if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
            msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP) {
            HWND focus = GetFocus();
            if (focus != g_hSearch && g_hMain) {
                WPARAM k = msg.wParam;
                if (k == VK_SPACE || k == VK_LEFT || k == VK_RIGHT ||
                    k == VK_UP || k == VK_DOWN || k == VK_ESCAPE) {
                    SendMessageW(g_hMain, msg.message, msg.wParam, msg.lParam);
                    continue;
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
