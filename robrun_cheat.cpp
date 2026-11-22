// robrun_cheat.cpp - external read-only overlay: esp, skeleton, legit aim, visuals, config menu
// hotkeys: INSERT menu, END exit, hold RMB = aim (when aim is on)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "offsets.hpp"

static const wchar_t* TARGET_EXE  = L"RobRun.exe";     // FILL: exe name if different
static const wchar_t* GA_NAME     = L"GameAssembly.dll";
static const float    FOV_DEG     = 90.0f;             // FILL: real camera fov
static const float    PI_F        = 3.14159265f;
static const int      HEIGHT_AXIS = 1;                 // FILL: world up axis 0=x 1=y 2=z
static const float    PERSON_H    = 1.7f;              // FILL: real character height
static const int      MENU_W      = 280;
static const int      MENU_H      = 260;
static const char*    CFG_DIR     = "robrun_cfg";
static const int      PARTICLES   = 140;

struct V3 { float x, y, z; };
static V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 addv(V3 a, V3 b, float k) { return { a.x + b.x * k, a.y + b.y * k, a.z + b.z * k }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 up_at(V3 p, float h) {
    if (HEIGHT_AXIS == 0) p.x += h; else if (HEIGHT_AXIS == 1) p.y += h; else p.z += h;
    return p;
}
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

struct Target { V3 pos; float hp; bool local; };
struct Scr    { bool ok; float sx, sy; };
struct View   { V3 cp, fw, rt, up; float focal, cx, cy; };

static Scr project(const View& v, V3 p) {
    Scr s{};
    V3 d = sub(p, v.cp);
    float z = dot(d, v.fw);
    if (z < 0.1f) return s;
    s.sx = v.cx + dot(d, v.rt) / z * v.focal;
    s.sy = v.cy - dot(d, v.up) / z * v.focal;
    s.ok = true;
    return s;
}

// ---- config state ----
struct Cfg {
    bool esp = true, box = true, skel = true, hp = true;
    bool aim = true, fovc = true;
    int  fov = 120, speed = 6;          // legit: fov radius px, max px per frame
    bool cross = true;
    int  cross_sz = 10;
    int  weather = 0;                   // 0 off, 1 snow, 2 rain
    int  atmo = 0;                      // 0..150 alpha of atmosphere layer
};
static Cfg g_cfg;

enum { K_BOOL, K_INT, K_ACT };
struct Item {
    int tab; const char* key; const char* label; int kind;
    bool* b; int* v; int lo, hi, step; int act;
};
static Item ITEMS[] = {
    {0, "esp",     "esp",         K_BOOL, &g_cfg.esp,       nullptr, 0,  0,   0,  0},
    {0, "box",     "boxes",       K_BOOL, &g_cfg.box,       nullptr, 0,  0,   0,  0},
    {0, "skel",    "skeleton",    K_BOOL, &g_cfg.skel,      nullptr, 0,  0,   0,  0},
    {0, "hp",      "hp text",     K_BOOL, &g_cfg.hp,        nullptr, 0,  0,   0,  0},
    {1, "aim",     "aimbot",      K_BOOL, &g_cfg.aim,       nullptr, 0,  0,   0,  0},
    {1, "fovc",    "draw fov",    K_BOOL, &g_cfg.fovc,      nullptr, 0,  0,   0,  0},
    {1, "fov",     "fov",         K_INT,  nullptr, &g_cfg.fov,   20, 400, 10, 0},
    {1, "speed",   "speed",       K_INT,  nullptr, &g_cfg.speed,  1,  30,  1, 0},
    {2, "cross",   "crosshair",   K_BOOL, &g_cfg.cross,     nullptr, 0,  0,   0,  0},
    {2, "cross_sz","cross size",  K_INT,  nullptr, &g_cfg.cross_sz, 4, 40, 2, 0},
    {2, "weather", "weather 0/1/2", K_INT, nullptr, &g_cfg.weather, 0, 2, 1, 0},
    {2, "atmo",    "atmosphere",  K_INT,  nullptr, &g_cfg.atmo,   0, 150, 10, 0},
    {3, "",        "save slot 1", K_ACT,  nullptr, nullptr, 0, 0, 0, 1},
    {3, "",        "save slot 2", K_ACT,  nullptr, nullptr, 0, 0, 0, 2},
    {3, "",        "save slot 3", K_ACT,  nullptr, nullptr, 0, 0, 0, 3},
    {3, "",        "load slot 1", K_ACT,  nullptr, nullptr, 0, 0, 0, 4},
    {3, "",        "load slot 2", K_ACT,  nullptr, nullptr, 0, 0, 0, 5},
    {3, "",        "load slot 3", K_ACT,  nullptr, nullptr, 0, 0, 0, 6},
};
static const int ITEM_N = (int)(sizeof(ITEMS) / sizeof(ITEMS[0]));

static HANDLE    g_proc = nullptr;
static uintptr_t g_ga   = 0;
static HWND      g_game = nullptr;
static HWND      g_ov   = nullptr;
static HWND      g_atm  = nullptr;
static HWND      g_menu = nullptr;
static int       g_tab  = 0;
static bool      g_menu_on = false;

// ---- memory ----
template<typename T>
static bool rd(uintptr_t a, T& out) {
    SIZE_T n = 0;
    return a && ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(a), &out, sizeof(T), &n) && n == sizeof(T);
}
static uintptr_t rp(uintptr_t a) { uintptr_t v = 0; rd(a, v); return v; }

static DWORD find_pid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) do {
        if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

static uintptr_t find_module(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) do {
        if (!_wcsicmp(me.szModule, name)) { base = reinterpret_cast<uintptr_t>(me.modBaseAddr); break; }
    } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    return base;
}

struct FindWin { DWORD pid; HWND hwnd; };
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<FindWin*>(lp);
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p == f->pid && IsWindowVisible(h) && GetWindowTextLengthW(h) > 0) { f->hwnd = h; return FALSE; }
    return TRUE;
}

static bool camera(V3& pos, V3& fw, V3& rt, V3& up) {
    uintptr_t cam = rp(g_ga + RR_SF_BATTLECAMERAMODEL);
    if (!cam) return false;
    return rd(cam + RR_OFF_CAM_POS, pos) && rd(cam + RR_OFF_CAM_FWD, fw)
        && rd(cam + RR_OFF_CAM_RIGHT, rt) && rd(cam + RR_OFF_CAM_UP, up);
}

static std::vector<Target> read_targets();  // defined below

static std::vector<Target> read_targets() {
    std::vector<Target> out;
    uintptr_t wem = rp(g_ga + RR_SF_WORLDENTITIESMODEL);
    if (!wem) return out;
    uintptr_t list = rp(wem + RR_OFF_WEM_BATTLE_CHARS);     // List<BattleCharacterModel>
    if (!list) return out;
    uintptr_t items = rp(list + 0x10);                       // List._items
    int32_t count = 0;
    rd(list + 0x18, count);                                  // List._size
    if (!items || count <= 0 || count > 256) return out;

    for (int32_t i = 0; i < count; ++i) {
        uintptr_t c = rp(items + 0x20 + i * 8);              // T[] data starts at +0x20
        if (!c) continue;

        bool dead = false;
        rd(c + RR_OFF_CHAR_ISDEAD, dead);
        if (dead) continue;

        uintptr_t tr = rp(c + RR_OFF_CHAR_TRANSFORM);
        Target t{};
        if (!tr || !rd(tr + RR_OFF_CTM_X, t.pos)) continue;

        uintptr_t data = rp(c + RR_OFF_CHAR_DATA);
        uintptr_t priv = rp(data + RR_OFF_DATA_PRIVATE);
        uintptr_t hm   = rp(priv + RR_OFF_PRIVATE_HEALTH);
        t.hp = 0.0f;
        if (hm) rd(hm + RR_OFF_HEALTH_VALUE, t.hp);

        t.local = rp(c + RR_OFF_CHAR_PBM) != 0;             // assumption: PBM only on local player
        out.push_back(t);
    }
    return out;
}

// ---- config files ----
static void cfg_path(char* out, size_t n, int slot) {
    snprintf(out, n, "%s\\slot%d.cfg", CFG_DIR, slot);
}

static void cfg_save(int slot) {
    CreateDirectoryA(CFG_DIR, nullptr);
    char path[260];
    cfg_path(path, sizeof path, slot);
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < ITEM_N; ++i) {
        const Item& it = ITEMS[i];
        if (it.kind == K_BOOL) fprintf(f, "%s %d\n", it.key, *it.b ? 1 : 0);
        else if (it.kind == K_INT) fprintf(f, "%s %d\n", it.key, *it.v);
    }
    fclose(f);
}

static void cfg_load(int slot) {
    char path[260];
    cfg_path(path, sizeof path, slot);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char key[32];
    int val = 0;
    while (fscanf(f, "%31s %d", key, &val) == 2) {
        for (int i = 0; i < ITEM_N; ++i) {
            Item& it = ITEMS[i];
            if (!it.key[0] || strcmp(it.key, key) != 0) continue;
            if (it.kind == K_BOOL) *it.b = val != 0;
            else if (it.kind == K_INT) {
                if (val < it.lo) val = it.lo;
                if (val > it.hi) val = it.hi;
                *it.v = val;
            }
        }
    }
    fclose(f);
}

static void run_act(int act) {
    if (act >= 1 && act <= 3) cfg_save(act);
    else if (act >= 4 && act <= 6) cfg_load(act - 3);
}

// ---- skeleton: approximate joints, FILL: replace with real bones when dumped ----
// {height, side} in world units, side along camera right
static const float JOINT[11][2] = {
    {0.90f,  0.00f},   // 0 pelvis
    {1.45f,  0.00f},   // 1 neck
    {1.70f,  0.00f},   // 2 head
    {1.40f, -0.25f},   // 3 l shoulder
    {1.40f,  0.25f},   // 4 r shoulder
    {1.10f, -0.35f},   // 5 l elbow
    {1.10f,  0.35f},   // 6 r elbow
    {0.05f, -0.15f},   // 7 l foot
    {0.05f,  0.15f},   // 8 r foot
    {0.85f, -0.40f},   // 9 l hand
    {0.85f,  0.40f},   // 10 r hand
};
static const int SEG[10][2] = {
    {0,1},{1,2},{1,3},{1,4},{3,5},{5,9},{4,6},{6,10},{0,7},{0,8},
};

static void draw_skel(HDC dc, const View& v, V3 pos) {
    Scr js[11];
    for (int i = 0; i < 11; ++i)
        js[i] = project(v, addv(up_at(pos, JOINT[i][0]), v.rt, JOINT[i][1]));
    for (int i = 0; i < 10; ++i) {
        const Scr& a = js[SEG[i][0]];
        const Scr& b = js[SEG[i][1]];
        if (!a.ok || !b.ok) continue;
        MoveToEx(dc, (int)a.sx, (int)a.sy, nullptr);
        LineTo(dc, (int)b.sx, (int)b.sy);
    }
}

// ---- weather particles ----
struct Pt { float x, y, sp, dx; };
static Pt   g_pt[PARTICLES];
static bool g_pt_init = false;

static void pt_reset(Pt& p, int W, int H, bool anywhere) {
    p.x = (float)(rand() % (W > 0 ? W : 1));
    p.y = anywhere ? (float)(rand() % (H > 0 ? H : 1)) : -(float)(rand() % 40);
    if (g_cfg.weather == 1) {
        p.sp = 1.0f + (rand() % 20) * 0.1f;
        p.dx = ((rand() % 3) - 1) * 0.4f;
    } else {
        p.sp = 10.0f + (float)(rand() % 6);
        p.dx = -1.5f;
    }
}

static void draw_weather(HDC dc, int W, int H) {
    if (W <= 0 || H <= 0) return;
    if (!g_pt_init) {
        for (Pt& p : g_pt) pt_reset(p, W, H, true);
        g_pt_init = true;
    }
    float len = g_cfg.weather == 1 ? 2.0f : 9.0f;
    for (Pt& p : g_pt) {
        p.x += p.dx;
        p.y += p.sp;
        if (p.y > H || p.x < -10.0f || p.x > W + 10.0f) pt_reset(p, W, H, false);
        MoveToEx(dc, (int)p.x, (int)p.y, nullptr);
        LineTo(dc, (int)p.x, (int)(p.y + len));
    }
}

// ---- atmosphere layer ----
static void update_atmo(const POINT& org, int W, int H) {
    if (g_cfg.atmo <= 0) { ShowWindow(g_atm, SW_HIDE); return; }
    SetLayeredWindowAttributes(g_atm, 0, (BYTE)g_cfg.atmo, LWA_ALPHA);
    SetWindowPos(g_atm, HWND_TOPMOST, org.x, org.y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static HWND make_atmo(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hi;
    wc.lpszClassName = L"rratm";
    wc.hbrBackground = CreateSolidBrush(RGB(40, 52, 70));
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"rratm", L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    return h;
}

// ---- drawing helpers ----
static void fillr(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{ l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void draw_cross(HDC dc, const View& v) {
    int s = g_cfg.cross_sz, gap = 3;
    int x = (int)v.cx, y = (int)v.cy;
    MoveToEx(dc, x - s, y, nullptr); LineTo(dc, x - gap, y);
    MoveToEx(dc, x + gap, y, nullptr); LineTo(dc, x + s, y);
    MoveToEx(dc, x, y - s, nullptr); LineTo(dc, x, y - gap);
    MoveToEx(dc, x, y + gap, nullptr); LineTo(dc, x, y + s);
}

// relative mouse move; game may use raw input, calibrate speed
static void aim_at(float dx, float dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)lroundf(dx);
    in.mi.dy = (LONG)lroundf(dy);
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

// legit: eased step toward target, capped per frame, small deadzone and jitter
static void legit_aim(const Scr& s, const View& v) {
    float dx = s.sx - v.cx, dy = s.sy - v.cy;
    if (fabsf(dx) < 2.0f && fabsf(dy) < 2.0f) return;
    float lim = (float)g_cfg.speed;
    float mx = dx * 0.12f + ((rand() % 100) / 100.0f - 0.5f) * 0.6f;
    float my = dy * 0.12f + ((rand() % 100) / 100.0f - 0.5f) * 0.6f;
    mx = clampf(mx, -lim, lim);
    my = clampf(my, -lim, lim);
    aim_at(mx, my);
}

// ---- main frame ----
struct Pick { bool ok = false; float dist = 1e9f; Scr s{}; };

static void frame(int W, int H) {
    HDC dc = GetDC(g_ov);
    RECT all{ 0, 0, W, H };
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));

    View v{};
    if (camera(v.cp, v.fw, v.rt, v.up)) {
        v.cx = W * 0.5f;
        v.cy = H * 0.5f;
        v.focal = v.cy / tanf(FOV_DEG * 0.5f * PI_F / 180.0f);

        HPEN pen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
        HPEN old = (HPEN)SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));

        if (g_cfg.weather) draw_weather(dc, W, H);

        Pick best{};
        for (const auto& t : read_targets()) {
            if (t.local) continue;
            Scr head = project(v, up_at(t.pos, PERSON_H));
            Scr foot = project(v, t.pos);
            if (!head.ok || !foot.ok) continue;

            if (g_cfg.esp && g_cfg.box) {
                float w = fabsf(foot.sy - head.sy) * 0.5f;
                Rectangle(dc, (int)(head.sx - w), (int)head.sy, (int)(head.sx + w), (int)foot.sy);
            }
            if (g_cfg.esp && g_cfg.hp) {
                char buf[16];
                snprintf(buf, sizeof buf, "%.0f", t.hp);
                TextOutA(dc, (int)head.sx - 8, (int)head.sy - 16, buf, (int)strlen(buf));
            }
            if (g_cfg.esp && g_cfg.skel) draw_skel(dc, v, t.pos);
            if (g_cfg.aim) {
                float d = hypotf(head.sx - v.cx, head.sy - v.cy);
                if (d <= (float)g_cfg.fov && d < best.dist) { best.ok = true; best.dist = d; best.s = head; }
            }
        }

        if (g_cfg.aim && g_cfg.fovc) {
            float r = (float)g_cfg.fov;
            Ellipse(dc, (int)(v.cx - r), (int)(v.cy - r), (int)(v.cx + r), (int)(v.cy + r));
        }
        if (g_cfg.cross) draw_cross(dc, v);

        SelectObject(dc, old);
        DeleteObject(pen);

        if (g_cfg.aim && best.ok && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            legit_aim(best.s, v);
    }
    ReleaseDC(g_ov, dc);
}

// ---- menu ----
static void menu_paint(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    fillr(dc, 0, 0, MENU_W, MENU_H, RGB(10, 10, 10));
    fillr(dc, 0, 0, MENU_W, 28, RGB(18, 18, 18));
    SetTextColor(dc, RGB(220, 220, 220));
    TextOutA(dc, 10, 7, "robrun", 6);

    const char* tabs[4] = { "esp", "aim", "visual", "cfg" };
    int tw = MENU_W / 4;
    for (int i = 0; i < 4; ++i) {
        fillr(dc, i * tw, 28, (i + 1) * tw, 54, i == g_tab ? RGB(44, 44, 44) : RGB(22, 22, 22));
        TextOutA(dc, i * tw + 8, 35, tabs[i], (int)strlen(tabs[i]));
    }

    int r = 0;
    for (int i = 0; i < ITEM_N; ++i) {
        const Item& it = ITEMS[i];
        if (it.tab != g_tab) continue;
        int y0 = 62 + r * 26;
        ++r;
        fillr(dc, 8, y0, MENU_W - 8, y0 + 22, RGB(20, 20, 20));
        SetTextColor(dc, RGB(220, 220, 220));
        if (it.kind == K_BOOL) {
            TextOutA(dc, 16, y0 + 4, it.label, (int)strlen(it.label));
            fillr(dc, MENU_W - 38, y0 + 6, MENU_W - 18, y0 + 17, *it.b ? RGB(230, 230, 230) : RGB(55, 55, 55));
        } else if (it.kind == K_INT) {
            char buf[64];
            snprintf(buf, sizeof buf, "%s   %d", it.label, *it.v);
            TextOutA(dc, 16, y0 + 4, buf, (int)strlen(buf));
        } else {
            TextOutA(dc, 16, y0 + 4, it.label, (int)strlen(it.label));
        }
    }

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
    HPEN old = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, MENU_W, MENU_H);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void menu_click(int x, int y, int btn) {
    if (y < 28) return;
    if (y < 54) {
        int t = x / (MENU_W / 4);
        if (t > 3) t = 3;
        if (t < 0) t = 0;
        g_tab = t;
        InvalidateRect(g_menu, nullptr, FALSE);
        return;
    }
    int r = 0;
    for (int i = 0; i < ITEM_N; ++i) {
        const Item& it = ITEMS[i];
        if (it.tab != g_tab) continue;
        int y0 = 62 + r * 26;
        ++r;
        if (y < y0 || y >= y0 + 22 || x < 8 || x > MENU_W - 8) continue;
        switch (it.kind) {
        case K_BOOL: *it.b = !*it.b; break;
        case K_INT: {
            int nv = *it.v + btn * it.step;
            if (nv < it.lo) nv = it.lo;
            if (nv > it.hi) nv = it.hi;
            *it.v = nv;
            break;
        }
        case K_ACT:
            if (btn > 0) run_act(it.act);
            break;
        }
        break;
    }
    InvalidateRect(g_menu, nullptr, FALSE);
}

static LRESULT CALLBACK menu_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        ScreenToClient(h, &p);
        return p.y < 28 ? HTCAPTION : HTCLIENT;             // title bar drags the window
    }
    case WM_LBUTTONDOWN:
        menu_click((short)LOWORD(l), (short)HIWORD(l), 1);
        return 0;
    case WM_RBUTTONDOWN:
        menu_click((short)LOWORD(l), (short)HIWORD(l), -1);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        menu_paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static HWND make_overlay(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = ov_proc;
    wc.hInstance     = hi;
    wc.lpszClassName = L"rrov";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"rrov", L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(h, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

static HWND make_menu(HINSTANCE hi) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = menu_proc;
    wc.hInstance     = hi;
    wc.lpszClassName = L"rrmenu";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"rrmenu", L"robrun", WS_POPUP,
                           40, 40, MENU_W, MENU_H, nullptr, nullptr, hi, nullptr);
}

int main() {
    CreateDirectoryA(CFG_DIR, nullptr);

    DWORD pid = find_pid(TARGET_EXE);
    if (!pid) { puts("game not running"); return 1; }

    g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!g_proc) { puts("OpenProcess failed (run as admin if EAC blocks it)"); return 1; }

    g_ga = find_module(pid, GA_NAME);
    if (!g_ga) { puts("GameAssembly.dll not found"); return 1; }

    FindWin fw{ pid, nullptr };
    EnumWindows(enum_cb, reinterpret_cast<LPARAM>(&fw));
    g_game = fw.hwnd;
    if (!g_game) { puts("game window not found"); return 1; }

    HINSTANCE hi = GetModuleHandleW(nullptr);
    g_atm  = make_atmo(hi);
    g_ov   = make_overlay(hi);
    g_menu = make_menu(hi);
    cfg_load(1);

    bool quit = false;
    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        if (GetAsyncKeyState(VK_INSERT) & 1) {
            g_menu_on = !g_menu_on;
            ShowWindow(g_menu, g_menu_on ? SW_SHOWNOACTIVATE : SW_HIDE);
        }

        if (IsIconic(g_game) || !IsWindow(g_game)) { Sleep(100); continue; }

        RECT cr{};
        GetClientRect(g_game, &cr);
        POINT org{ 0, 0 };
        ClientToScreen(g_game, &org);
        int W = cr.right, H = cr.bottom;

        update_atmo(org, W, H);
        SetWindowPos(g_ov, HWND_TOPMOST, org.x, org.y, W, H, SWP_NOACTIVATE);
        if (g_menu_on)
            SetWindowPos(g_menu, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        frame(W, H);
        Sleep(8);
    }

    CloseHandle(g_proc);
    return 0;
}