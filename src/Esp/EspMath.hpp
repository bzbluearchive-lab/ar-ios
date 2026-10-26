// EspMath.hpp —— 只读数学层：向量 / 旋转 / 4x4 矩阵 / UE4 世界坐标转屏幕 / 屏幕比例
//
// 依赖：<math.h> <string.h> 与 C++17 基本类型。这里不 include ObjC、不 include ImGui，
//       也不 include 任何 game/ 头 —— 好处是这一层可以在桌面上用普通 g++ 直接编一个
//       数值测试（tools 里跑过），W2S 的正确性不靠"看起来对"。
//
// 坐标系约定（UE4）：
//   X 前 / Y 右 / Z 上；FRotator = (Pitch, Yaw, Roll)，单位是度，Pitch 正 = 抬头。
//
// ★ 与 src/Render/ESP.mm、src/Game/Aim.mm 里那套旧 w2s 的差别（重要）：
//   旧实现先算 (x1,y1,z1) = (前, 右, 上)，再做 pitch 旋转得到 (x2,z2)，然后
//        sx = x2 / (z2 * tanHalf * aspect)
//        sy = y1 / (z2 * tanHalf)
//   但 x2 是【前向（深度）】分量、z2 才是【上】分量 —— 两个除法都拿"上"当深度除了。
//   症状：pitch = 0 时 (z2 恒为 0) 直接判负返回 false，整屏什么都不画；
//         pitch ≠ 0 时水平坐标随鼠标抬头低头乱飘。
//   这里按 UE4 FSceneView::WorldToScreen 重写：
//        depth = dot(d, fwd) / right = dot(d, right) / up = dot(d, up)
//        ndcX  = right / (depth * tanHalf * aspect)
//        ndcY  = up    / (depth * tanHalf)
//        sx = (ndcX + 1) * 0.5 * W      sy = (1 - ndcY) * 0.5 * H
//   world_to_screen()（角度式）与 world_to_screen_matrix()（矩阵式，viewProj）两条路
//   在数值上完全一致，桌面上用 4000 组随机样本对拍过（最大误差 < 1e-3 px）。

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

namespace espm {

constexpr float kPi        = 3.14159265358979323846f;
constexpr float kDeg2Rad   = kPi / 180.0f;
constexpr float kRad2Deg   = 180.0f / kPi;

// 相机后方/贴脸的剔除阈值（UE4 世界单位 = cm）。太小会把"贴着镜头"的骨骼拉到无穷远。
constexpr float kMinDepth  = 4.0f;

// FTransformVectorized / FMatrix(3x4) 的平移都在 +0x30（3 个 16 字节旋转块之后）；
// 紧凑 FTransform 的平移在 +0x10。这是引擎结构常量，不是版本偏移，所以放数学层。
// 骨骼数组用哪套布局由 espx::Settings::boneLayout 选（见 EspBones.hpp 的 BoneLayout）。
constexpr uint32_t kMatTrans = 0x30;

// ---------------------------------------------------------------- 基础类型
struct Vec3 { float X, Y, Z; };
struct Vec2 { float X, Y; };
struct Rot  { float Pitch, Yaw, Roll; };

inline Vec3 V3(float x, float y, float z) { Vec3 v; v.X = x; v.Y = y; v.Z = z; return v; }
inline Vec2 V2(float x, float y) { Vec2 v; v.X = x; v.Y = y; return v; }

// 不用 isfinite()：省掉 <cmath> 与宏/命名空间之争，行为等价（NaN != NaN，inf 超界）
inline bool fin(float v)        { return v == v && v < 1e30f && v > -1e30f; }
inline bool fin3(const Vec3& v) { return fin(v.X) && fin(v.Y) && fin(v.Z); }

inline Vec3  add(const Vec3& a, const Vec3& b) { return V3(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
inline Vec3  sub(const Vec3& a, const Vec3& b) { return V3(a.X - b.X, a.Y - b.Y, a.Z - b.Z); }
inline Vec3  mul(const Vec3& a, float s)       { return V3(a.X * s, a.Y * s, a.Z * s); }
inline float dot(const Vec3& a, const Vec3& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline float length(const Vec3& a)             { return sqrtf(dot(a, a)); }
inline float dist(const Vec3& a, const Vec3& b) { return length(sub(a, b)); }
inline Vec3  lerp(const Vec3& a, const Vec3& b, float t) {
    return V3(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t);
}

// ---------------------------------------------------------------- 4x4 矩阵
// 行主序，乘列向量：out[r] = sum_c M[r][c] * v[c]
struct Mat4 { float M[4][4]; };

inline void mat_identity(Mat4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m.M[r][c] = (r == c) ? 1.0f : 0.0f;
}

inline void mat_zero(Mat4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m.M[r][c] = 0.0f;
}

inline Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 o;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.M[r][k] * b.M[k][c];
            o.M[r][c] = s;
        }
    }
    return o;
}

inline void mat_vec4(const Mat4& m, const float in[4], float out[4]) {
    for (int r = 0; r < 4; ++r)
        out[r] = m.M[r][0] * in[0] + m.M[r][1] * in[1] + m.M[r][2] * in[2] + m.M[r][3] * in[3];
}

// ---------------------------------------------------------------- 相机
// 视图空间约定：+X 右 / +Y 上 / +Z 前（深度）。
// focal = (vpH / 2) / tan(fov/2)，即竖直方向"焦距（像素）"，用来按真实尺寸算屏幕大小。
struct CameraView {
    Vec3  loc{};
    Rot   rot{};
    float fov    = 90.0f;
    float aspect = 1.0f;
    float vpW    = 0.0f;
    float vpH    = 0.0f;
    float focal  = 0.0f;

    Vec3 fwd{}, right{}, up{};
    Mat4 view{}, proj{}, viewProj{};
    bool ok = false;
};

// 由 FMinimalViewInfo 的三件（Location / Rotation / FOV）+ 屏幕比例建相机。
// 返回 false = 数据不合理（FOV 越界 / 坐标非有限 / 视口太小），调用方本帧直接不画。
inline bool make_camera(CameraView& c, const Vec3& loc, const Rot& rot,
                        float fovDeg, float aspect, float vpW, float vpH) {
    c.loc = loc;
    c.rot = rot;
    c.fov = fovDeg;
    c.aspect = aspect;
    c.vpW = vpW;
    c.vpH = vpH;
    c.ok = false;

    if (!fin3(loc)) return false;
    if (!(fovDeg > 1.0f && fovDeg < 179.0f)) return false;
    if (!(aspect > 0.01f && aspect < 20.0f)) return false;
    if (!(vpW > 1.0f && vpH > 1.0f)) return false;

    const float p = rot.Pitch * kDeg2Rad;
    const float y = rot.Yaw   * kDeg2Rad;
    const float r = rot.Roll  * kDeg2Rad;
    const float cp = cosf(p), sp = sinf(p);
    const float cy = cosf(y), sy = sinf(y);

    // 前向量：yaw 绕 Z、pitch 绕右轴；右向量取水平面内；上向量 = fwd × right 的反向
    const Vec3 fwd = V3(cp * cy, cp * sy, sp);
    Vec3 rt  = V3(-sy, cy, 0.0f);
    Vec3 upv = V3(-sp * cy, -sp * sy, cp);

    // Roll：绕前向量转（很多 UE 游戏 Roll 恒为 0，但相机抖动/载具会用到）
    if (r != 0.0f) {
        const float cr = cosf(r), sr = sinf(r);
        const Vec3 rt2 = add(mul(rt, cr), mul(upv, sr));
        const Vec3 up2 = add(mul(upv, cr), mul(rt, -sr));
        rt  = rt2;
        upv = up2;
    }
    c.fwd = fwd;
    c.right = rt;
    c.up = upv;

    // 视图矩阵：行 = right / up / fwd，第 4 列 = -dot(row, loc)
    mat_zero(c.view);
    c.view.M[0][0] = rt.X;  c.view.M[0][1] = rt.Y;  c.view.M[0][2] = rt.Z;
    c.view.M[0][3] = -dot(rt, loc);
    c.view.M[1][0] = upv.X; c.view.M[1][1] = upv.Y; c.view.M[1][2] = upv.Z;
    c.view.M[1][3] = -dot(upv, loc);
    c.view.M[2][0] = fwd.X; c.view.M[2][1] = fwd.Y; c.view.M[2][2] = fwd.Z;
    c.view.M[2][3] = -dot(fwd, loc);
    c.view.M[3][3] = 1.0f;

    const float tanHalf = tanf(fovDeg * 0.5f * kDeg2Rad);
    if (!(tanHalf > 1e-4f)) return false;

    // 透视投影：clip.w = 视图空间 z（深度），clip.x / clip.y 由 tanHalf 决定。
    // Z 用 D3D 风格的 zn..zf（iOS Metal 也是 [0,1]），Z 项不参与屏幕 XY，只保持自洽。
    const float zn = 1.0f, zf = 1.0e7f;
    mat_zero(c.proj);
    c.proj.M[0][0] = 1.0f / (tanHalf * aspect);
    c.proj.M[1][1] = 1.0f / tanHalf;
    c.proj.M[2][2] = zf / (zf - zn);
    c.proj.M[2][3] = -(zn * zf) / (zf - zn);
    c.proj.M[3][2] = 1.0f;

    c.viewProj = mat_mul(c.proj, c.view);
    c.focal = (vpH * 0.5f) / tanHalf;
    c.ok = true;
    return true;
}

// 世界 -> 屏幕（角度式，主路径）。返回 false = 在相机后方 / 数据非法。
inline bool world_to_screen(const CameraView& c, const Vec3& w, Vec2& out) {
    if (!c.ok) return false;
    const Vec3 d = sub(w, c.loc);
    const float z = dot(d, c.fwd);
    if (!fin(z) || z < kMinDepth) return false;

    const float tanHalf = tanf(c.fov * 0.5f * kDeg2Rad);
    if (!(tanHalf > 1e-4f)) return false;

    const float ndcX = dot(d, c.right) / (z * tanHalf * c.aspect);
    const float ndcY = dot(d, c.up)    / (z * tanHalf);
    if (!fin(ndcX) || !fin(ndcY)) return false;

    out.X = (ndcX + 1.0f) * 0.5f * c.vpW;
    out.Y = (1.0f - ndcY) * 0.5f * c.vpH;
    return fin(out.X) && fin(out.Y);
}

// 世界 -> 屏幕（矩阵式，等价路径）。用 UE FSceneView::WorldToScreen 的落屏公式：
//   sx = (ndcX + 1) * 0.5 * W        sy = (ndcY - 1) * -0.5 * H
// 留这条是为了"以后要从别处（比如接进来的 ViewMatrices）直接喂矩阵"时不用改调用点。
inline bool world_to_screen_matrix(const CameraView& c, const Vec3& w, Vec2& out) {
    if (!c.ok) return false;
    const float p[4] = { w.X, w.Y, w.Z, 1.0f };
    float clip[4];
    mat_vec4(c.viewProj, p, clip);
    if (!fin(clip[3]) || clip[3] < kMinDepth) return false;

    const float rhw  = 1.0f / clip[3];
    const float ndcX = clip[0] * rhw;
    const float ndcY = clip[1] * rhw;
    if (!fin(ndcX) || !fin(ndcY)) return false;

    out.X = (ndcX + 1.0f) * 0.5f * c.vpW;
    out.Y = (ndcY - 1.0f) * -0.5f * c.vpH;
    return fin(out.X) && fin(out.Y);
}

// 视图空间深度（cm）。<= 0 表示在相机后方。
inline float depth_of(const CameraView& c, const Vec3& w) {
    if (!c.ok) return -1.0f;
    return dot(sub(w, c.loc), c.fwd);
}

// 距离 distCm 处，1 cm 在屏幕上占多少像素（用来按真实尺寸画头/胸/腰框）
inline float pixels_per_cm(const CameraView& c, float distCm) {
    if (!c.ok || !(distCm > 1.0f)) return 0.0f;
    const float px = c.focal / distCm;
    return fin(px) ? px : 0.0f;
}

// 由方向向量反推 FRotator（度）。不是绘制路径要用的东西，主要是自检/对账：
// 拿相机位置和目标位置算出 yaw/pitch，和游戏里读到的 POV.Rotation 比一眼就能看出
// 是不是把 Pitch / Yaw 弄反了（UE 的 Pitch 正 = 抬头，Yaw 正 = 右转）。
inline Rot dir_to_rot(const Vec3& dir) {
    Rot r{};
    const float flat = sqrtf(dir.X * dir.X + dir.Y * dir.Y);
    r.Yaw   = atan2f(dir.Y, dir.X) * kRad2Deg;
    r.Pitch = atan2f(dir.Z, flat) * kRad2Deg;
    r.Roll  = 0.0f;
    return r;
}

// ---------------------------------------------------------------- 旋转块
// 骨骼与组件变换的旋转有两种表示，都读成同一个 12 float 块，用 kind 区分：
//   0 = FQuat（x, y, z, w，4 个 float）
//   1 = 3x4 行主序矩阵（12 个 float，第 4 列是平移，这里只取 3x3）
inline Vec3 quat_rotate(const float q[4], const Vec3& v) {
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.0f * (qy * v.Z - qz * v.Y);
    const float ty = 2.0f * (qz * v.X - qx * v.Z);
    const float tz = 2.0f * (qx * v.Y - qy * v.X);
    return V3(v.X + qw * tx + (qy * tz - qz * ty),
              v.Y + qw * ty + (qz * tx - qx * tz),
              v.Z + qw * tz + (qx * ty - qy * tx));
}

inline Vec3 mat3x4_rotate(const float m[12], const Vec3& v) {
    return V3(m[0] * v.X + m[1] * v.Y + m[2]  * v.Z,
              m[4] * v.X + m[5] * v.Y + m[6]  * v.Z,
              m[8] * v.X + m[9] * v.Y + m[10] * v.Z);
}

// ---------------------------------------------------------------- 屏幕矩形
struct Rect { float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f; };

inline bool rect_valid(const Rect& r) { return r.x1 > r.x0 && r.y1 > r.y0; }
inline bool rect_contains(const Rect& r, float x, float y) {
    return rect_valid(r) && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
}
inline Rect rect_grow(const Rect& r, float m) {
    Rect o; o.x0 = r.x0 - m; o.y0 = r.y0 - m; o.x1 = r.x1 + m; o.y1 = r.y1 + m; return o;
}
inline bool rect_on_screen(const Rect& r, float w, float h) {
    return rect_valid(r) && r.x1 >= 0.0f && r.y0 <= h && r.x0 <= w && r.y1 >= 0.0f;
}

// ---------------------------------------------------------------- 屏幕比例 + 安全区
// iOS 的"点 vs 像素"和刘海安全区都在这里落库：
//   pxW/pxH  = drawable 像素（W2S 与 ImGui DisplaySize 都用这个，游戏是整屏渲染）
//   ptW/ptH  = CAMetalLayer.bounds（点），触摸事件给的是点
//   scale    = 像素 / 点
//   ins*     = 安全区内缩（像素），只有 UI（菜单/按钮）需要避让，W2S 不避让
struct ScreenMap {
    float pxW = 0.0f, pxH = 0.0f;
    float ptW = 0.0f, ptH = 0.0f;
    float scale = 1.0f;
    float insTop = 0.0f, insBottom = 0.0f, insLeft = 0.0f, insRight = 0.0f;
    float uiScale = 1.0f;      // ImGui 字号缩放 = clamp(scale, 1, 3)

    bool  valid()  const { return pxW > 1.0f && pxH > 1.0f; }
    float aspect() const { return (pxH > 1.0f) ? (pxW / pxH) : 1.0f; }

    // 像素 -> 点 / 点 -> 像素
    float to_px(float pt) const { return pt * scale; }
    float to_pt(float px) const { return (scale > 0.0f) ? (px / scale) : px; }

    float safe_top_pt()    const { return to_pt(insTop); }
    float safe_bottom_pt() const { return to_pt(insBottom); }

    // 安全区内的可用区域（像素）
    Rect safe_px() const {
        Rect r;
        r.x0 = insLeft; r.y0 = insTop;
        r.x1 = pxW - insRight; r.y1 = pxH - insBottom;
        return r;
    }

    void update(float drawableW, float drawableH, float boundsW, float boundsH) {
        pxW = drawableW;
        pxH = drawableH;
        ptW = boundsW;
        ptH = boundsH;
        scale = (ptW > 1.0f && pxW > 1.0f) ? (pxW / ptW) : 1.0f;
        if (!fin(scale) || scale < 0.5f || scale > 8.0f) scale = 1.0f;
        uiScale = (scale < 1.0f) ? 1.0f : ((scale > 3.0f) ? 3.0f : scale);
    }
};

} // namespace espm
