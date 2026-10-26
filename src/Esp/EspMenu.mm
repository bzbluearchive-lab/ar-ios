// EspMenu.mm —— 菜单本体
//
// 所有 UI 文本都走 XDECL（XOR 到栈上再解码），二进制里没有 "skeleton" / "loot"
// 这类明文串 —— anogs 扫进程内区域时靠明文 UI 文本识别的成本最低，别白送指纹。
//
// 坐标体系：ImGui 的 DisplaySize 被设成 drawable 像素，所以这里所有几何量都是像素。
// 想要"44 点的按钮"就写 44 * scr.scale；触摸点也乘同一个 scale，两边才对得上。

#include "EspMenu.hpp"

#include <stdio.h>
#include <string.h>

#include "imgui.h"

namespace espx {
namespace menu {

// ---------------------------------------------------------------- 状态
static bool g_open   = true;      // 菜单展开
static bool g_haveBtn = false;    // 按钮矩形是否已经算过（首帧前不参与命中）
static bool g_haveWin = false;    // 窗口矩形是否已经算过

static espm::Rect g_btn{};        // 常驻按钮（像素）
static espm::Rect g_win{};        // 菜单窗口（像素，上一帧的）
static float      g_scale = 1.0f; // 上一帧的 像素/点（命中判定容差用）

bool open()   { return g_open; }
void toggle() { g_open = !g_open; }

// ---------------------------------------------------------------- 命中判定
bool hit_button(float px, float py) {
    if (!g_haveBtn) return false;
    return espm::rect_contains(espm::rect_grow(g_btn, 10.0f * g_scale), px, py);
}

bool hit_window(float px, float py) {
    if (!g_open || !g_haveWin) return false;
    return espm::rect_contains(espm::rect_grow(g_win, 6.0f * g_scale), px, py);
}

bool hit_any(float px, float py) {
    return hit_button(px, py) || hit_window(px, py);
}

// ---------------------------------------------------------------- 绘制
void draw(const espm::ScreenMap& scr, Frame& f) {
    Settings& S = settings();

    const float sc = (scr.scale > 0.0f) ? scr.scale : 1.0f;
    g_scale = sc;

    // ---- 常驻按钮几何：左上角 14,58 点，44x44 点（刘海/状态栏下自动下移）----
    float bxPt = 14.0f;
    float byPt = 58.0f;
    const float safeTopPt = scr.safe_top_pt();
    if (byPt < safeTopPt + 8.0f) byPt = safeTopPt + 8.0f;

    g_btn.x0 = bxPt * sc;
    g_btn.y0 = byPt * sc;
    g_btn.x1 = g_btn.x0 + 44.0f * sc;
    g_btn.y1 = g_btn.y0 + 44.0f * sc;
    g_haveBtn = true;

    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(ImVec2(g_btn.x0, g_btn.y0), ImVec2(g_btn.x1, g_btn.y1),
                          IM_COL32(16, 18, 22, 190), 9.0f * sc);
        bg->AddRect(ImVec2(g_btn.x0, g_btn.y0), ImVec2(g_btn.x1, g_btn.y1),
                    IM_COL32(90, 220, 150, 230), 9.0f * sc, 0, 2.0f * sc);

        // 三段横线（不依赖字体里的特殊字形，避免字体缺字变方块）
        const float lx = g_btn.x0 + 11.0f * sc;
        const float ly = g_btn.y0 + 12.0f * sc;
        const float lw = 22.0f * sc;
        for (int i = 0; i < 3; ++i) {
            bg->AddLine(ImVec2(lx, ly + (float)i * 10.0f * sc),
                        ImVec2(lx + lw, ly + (float)i * 10.0f * sc),
                        IM_COL32(90, 220, 150, 255), 2.0f * sc);
        }
        if (g_open) {
            bg->AddRect(ImVec2(g_btn.x0, g_btn.y0), ImVec2(g_btn.x1, g_btn.y1),
                        IM_COL32(255, 210, 60, 255), 9.0f * sc, 0, 2.0f * sc);
        }
    }

    if (!g_open) {
        g_haveWin = false;
        return;
    }

    // ---- 菜单窗口 ----
    // ImGuiCond_Appearing：只在"出现时"定位一次，之后允许手指拖标题栏移动。
    // 想钉死不动就改成 ImGuiCond_Always。
    XDECL(l_title,  "overlay");
    ImGui::SetNextWindowPos(ImVec2(g_btn.x1 + 10.0f * sc, g_btn.y0), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.92f);

    bool keepOpen = true;
    if (ImGui::Begin(l_title, &keepOpen,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoCollapse)) {

        // ★ 同一窗口里 ImGui 的 ID 由 label 文本算出来，所以 label 必须全局唯一。
        //   下面故意让"开关"和"颜色"用不同措辞（"skeleton" vs "skeleton col"），
        //   否则两个控件会共用一个 ID，点一个动两个。
        XDECL(l_skel,     "skeleton");
        XDECL(l_equip,    "equipment");
        XDECL(l_loot,     "loot");
        XDECL(l_dist,     "distance");
        XDECL(l_box,      "fallback box");
        XDECL(l_swallow,  "ui takes touch");
        XDECL(l_sep1,     "range");
        XDECL(l_radius,   "radius (m)");
        XDECL(l_thick,    "line width");
        XDECL(l_boxth,    "box width");
        XDECL(l_sep2,     "limits");
        XDECL(l_actors,   "actors / frame");
        XDECL(l_bonemax,  "bone lines");
        XDECL(l_lootmax,  "loot items");
        XDECL(l_sep3,     "layout");
        XDECL(l_layout,   "bone layout");
        XDECL(l_lay0,     "compact 0x30");
        XDECL(l_lay1,     "vectorized 0x50");
        XDECL(l_sep4,     "colors");
        XDECL(l_cskel,    "skeleton col");
        XDECL(l_cequip,   "equipment col");
        XDECL(l_cloot,    "loot col");
        XDECL(l_sep5,     "stats");
        XDECL(l_stat,     "actors %d / players %d / loot %d / lines %d");
        XDECL(l_hint,     "drag the title bar to move");

        ImGui::Checkbox(l_skel,  &S.skeleton);
        ImGui::Checkbox(l_equip, &S.equipment);
        ImGui::Checkbox(l_loot,  &S.loot);
        ImGui::Checkbox(l_dist,  &S.distance);
        ImGui::Checkbox(l_box,   &S.box);
        ImGui::Checkbox(l_swallow, &S.swallowTouch);

        ImGui::SeparatorText(l_sep1);
        ImGui::SliderFloat(l_radius, &S.maxDistM, 20.0f, 800.0f, "%.0f m");
        ImGui::SliderFloat(l_thick,  &S.lineThick, 0.5f, 5.0f, "%.1f");
        ImGui::SliderFloat(l_boxth,  &S.boxThick,  0.5f, 5.0f, "%.1f");

        ImGui::SeparatorText(l_sep2);
        ImGui::SliderInt(l_actors,  &S.maxActors, 64, 2048);
        ImGui::SliderInt(l_bonemax, &S.maxBones, 8, 240);
        ImGui::SliderInt(l_lootmax, &S.maxLoot, 8, 256);

        ImGui::SeparatorText(l_sep3);
        const char* layItems[2] = { l_lay0, l_lay1 };
        ImGui::Combo(l_layout, &S.boneLayout, layItems, 2);

        ImGui::SeparatorText(l_sep4);
        // ColorEdit4 带 NoInputs：点色块弹拾色器，省屏幕空间
        ImGui::ColorEdit4(l_cskel,  S.colSkel,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4(l_cequip, S.colEquip,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::ColorEdit4(l_cloot,  S.colLoot,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

        ImGui::SeparatorText(l_sep5);
        ImGui::Text(l_stat, f.visited, f.players, f.loot, f.boneLines);
        ImGui::TextDisabled(l_hint);

        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        g_win.x0 = p.x; g_win.y0 = p.y;
        g_win.x1 = p.x + s.x; g_win.y1 = p.y + s.y;
        g_haveWin = true;
    } else {
        g_haveWin = false;
    }
    ImGui::End();

    if (!keepOpen) g_open = false;      // 标题栏那个 ×
}

} // namespace menu
} // namespace espx
