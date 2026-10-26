// EspRender.mm —— ImGui 初始化 + Metal 挂点 + 每帧驱动 + 触摸注入
//
// 挂点与旧的 src/Render/Overlay.mm 一致（那是唯一在真机上跑通过的一条链路）：
//   1. -[CAMetalLayer nextDrawable]        ：拿到本帧 drawable，并在首次拿到时
//                                            按 drawable 的【具体类】挂 present
//   2. -[<drawable 具体类> present]        ：在游戏提交呈现之前插一个
//                                            LoadAction = Load 的 render pass，
//                                            保住游戏已画的像素
//   3. -[UIApplication sendEvent:]         ：触摸 -> ImGui，且只在落进我们 UI 时吞事件
//
// 为什么 present 侧要按"具体类"挂：CAMetalDrawable 是 @protocol 不是类，
// objc_getClass("CAMetalDrawable") 拿不到；正解是第一次 nextDrawable 返回真实对象后
// 用 object_getClass() 拿到私有类（_CAMetalDrawable 之类），在它上面换 IMP。
//
// ★ 不要在这里再建一个 ImGui context：一个进程只有一个。若工程里同时存在
//   Render/Overlay.mm，两者必须二选一（见 README 的 patch）。

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>

#include <os/lock.h>
#include <stdint.h>
#include <stdio.h>

#include "EspRender.hpp"
#include "EspBones.hpp"
#include "EspLoot.hpp"
#include "EspMenu.hpp"

#include "../Core/Hook.hpp"          // hook::swizzle_class_method
#include "../Game/Offsets.hpp"
#include "../Game/World.hpp"

#include "imgui.h"
#include "imgui_impl_metal.h"

// ---------------- 最小类型约束 ----------------
// 不同 SDK 版本对 CAMetalDrawable 是否随 <QuartzCore/CAMetalLayer.h> 带进来不一致
// （theos 的 iPhoneOS SDK 上实测不带），所以不依赖 SDK 里的那个协议，自己声明一份。
@protocol EDrawable <NSObject>
- (id<MTLTexture>)texture;
- (id)layer;
- (void)present;
- (void)presentAtTime:(CFTimeInterval)t;
@end

@protocol ELayer <NSObject>
- (CGSize)drawableSize;
- (CGRect)bounds;              // 单位是"点"，用来换算 像素/点
- (id<MTLDevice>)device;
@end

typedef id<EDrawable> EDrawableRef;

// ---------------- 全局状态 ----------------
namespace espx {
static Settings g_set;
Settings& settings() { return g_set; }
}

static espx::Frame g_frame;

struct MetalState {
    id<MTLDevice>       dev  = nil;
    id<MTLCommandQueue> q    = nil;
    bool                init = false;
    bool                objs = false;
};
static MetalState g_m;

// 触摸线程（主线程）与渲染线程都要碰 ImGui 的输入队列，用一把细锁隔开。
// 不隔的后果：菜单偶发"点了没反应"或拖动跳一下 —— 属于偶发、难查的那类问题。
static os_unfair_lock g_ioLock = OS_UNFAIR_LOCK_INIT;

static bool           g_styleScaled = false;
static int64_t        g_frames = 0;
static CFTimeInterval g_lastFrameTime = 0.0;

// ---------------- 安全区 ----------------
// iOS 16 起 [UIScreen mainScreen] 被标 deprecated（多场景），但行为仍在；
// 用 respondsToSelector 兜一层，再用 pragma 关掉 deprecation 警告（不影响功能）。
static bool read_safe_area(UIEdgeInsets& out) {
    out = UIEdgeInsetsZero;
    bool got = false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIScreen* scr = [UIScreen mainScreen];
    if (scr && [scr respondsToSelector:@selector(safeAreaInsets)]) {
        out = [scr safeAreaInsets];
        got = true;
    }
#pragma clang diagnostic pop
    return got;
}

// ---------------- ImGui + Metal 起来 ----------------
static bool bringup(id<MTLDevice> dev) {
    if (g_m.init) return true;
    if (!dev) return false;

    if (!g_m.dev) {
        // 一次性：设备、命令队列、ImGui context、Metal 后端 Init
        g_m.dev = dev;
        g_m.q = [dev newCommandQueue];
        if (!g_m.q) {
            NSLog(@"[espx] newCommandQueue returned nil");
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;      // 不落盘：少一个文件痕迹
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplMetal_Init(dev)) {
            NSLog(@"[espx] ImGui_ImplMetal_Init failed dev=%@", dev);
            return false;
        }
    }

    // 设备对象（字形纹理 + pipeline）。
    // 1.90.9 的签名是 ImGui_ImplMetal_CreateDeviceObjects(id<MTLDevice>) -> bool,
    // 不是无参版本；缺这一步第一帧就会在 nil pipeline / nil font texture 上崩。
    // 失败就留着 init=false 下一帧再试 —— 在没建起来的后端上画必崩，宁可不画。
    if (!g_m.objs) {
        g_m.objs = ImGui_ImplMetal_CreateDeviceObjects(dev);
        if (!g_m.objs) {
            NSLog(@"[espx] CreateDeviceObjects failed, retry next frame");
            return false;
        }
    }

    g_m.init = true;
    NSLog(@"[espx] imgui ready queue=%@", g_m.q);
    return true;
}

// ---------------- 屏幕比例 / 安全区 -> ScreenMap ----------------
static void update_screen(EDrawableRef d, float vpW, float vpH) {
    espm::ScreenMap& s = g_frame.scr;

    float bw = 0.0f, bh = 0.0f;
    id<ELayer> lay = (id<ELayer>)[d layer];
    if (lay) {
        const CGRect r = [lay bounds];
        bw = (float)r.size.width;
        bh = (float)r.size.height;
    }
    s.update(vpW, vpH, bw, bh);

    UIEdgeInsets in = UIEdgeInsetsZero;
    if (read_safe_area(in)) {
        s.insTop    = (float)in.top    * s.scale;
        s.insBottom = (float)in.bottom * s.scale;
        s.insLeft   = (float)in.left   * s.scale;
        s.insRight  = (float)in.right  * s.scale;
    }

    // 首帧把 ImGui 的字号/间距按 retina 比例放大一次：
    // DisplaySize 用的是像素，默认 13px 的字在 3x 屏上只有 4 点多，菜单会小到看不清。
    // ★ 必须判 context：update_screen 在 bringup 之前就会被调（present 里先 update 再 do_draw），
    //   这时 ImGui::GetStyle() 是空指针解引用。
    if (!g_styleScaled && s.scale > 0.0f && ImGui::GetCurrentContext()) {
        g_styleScaled = true;
        ImGui::GetStyle().ScaleAllSizes(s.uiScale);
        ImGui::GetIO().FontGlobalScale = s.uiScale;
    }
}

// ---------------- 世界遍历 + 绘制 ----------------
// 一帧只解一次世界与相机；actor 遍历按 settings().maxActors 截断（性能上限）。
static void frame_draw(float vpW, float vpH) {
    espx::Frame& f = g_frame;
    espx::Settings& S = espx::settings();

    f.scr.pxW = vpW;
    f.scr.pxH = vpH;
    f.visited = f.players = f.loot = f.boneLines = 0;
    f.maxDistCm = (S.maxDistM > 0.0f) ? (S.maxDistM * 100.0f) : 0.0f;

    static world::Context ctx;
    world::refresh(ctx);

    FMinimalViewInfo pov{};
    if (!world::camera(ctx, pov)) return;                 // 相机链没通（偏移待填）
    if (!espx::make_frame_camera(f.cam, pov, f.scr)) return;

    if (!ctx.level) return;
    TArrayRaw arr{};
    if (!mem::rd(ctx.level + off::ULEVEL_ACTORS, arr)) return;
    if (!arr.Data || arr.Num <= 0 || arr.Num > 65536) return;   // 链路失效的典型症状

    int32_t n = arr.Num;
    if (n > S.maxActors) n = S.maxActors;                 // 每帧最多看 N 个 actor
    if (n < 0) return;

    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t a = mem::rdptr(arr.Data + (uintptr_t)i * sizeof(uintptr_t));
        if (!a) continue;
        ++f.visited;
        if (espx::bones::try_actor(a, f)) ++f.players;
        espx::loot::try_actor(a, f);
    }
}

// ---------------- 一帧 ----------------
static void draw_frame(EDrawableRef d, float vpW, float vpH) {
    if (!d || !g_m.init) return;
    if (vpW < 2.0f || vpH < 2.0f) return;
    if (!ImGui::GetCurrentContext()) return;

    id<MTLTexture> tex = [d texture];
    if (!tex) return;                                     // drawable 还没可用的纹理

    id<MTLCommandBuffer> cb = [g_m.q commandBuffer];
    if (!cb) return;

    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!rpd) return;
    rpd.colorAttachments[0].texture     = tex;
    rpd.colorAttachments[0].loadAction  = MTLLoadActionLoad;   // 保住游戏已画的像素
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

    os_unfair_lock_lock(&g_ioLock);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(vpW, vpH);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);           // 我们已经按像素算了

    const CFTimeInterval now = CFAbsoluteTimeGetCurrent();
    if (g_lastFrameTime > 0.0) {
        float dt = (float)(now - g_lastFrameTime);
        if (!(dt > 0.0005f)) dt = 0.0005f;                     // 掉到 0 会让 ImGui 断言
        if (dt > 0.1f) dt = 0.1f;                              // 切后台回来的大跳变
        io.DeltaTime = dt;
    }
    g_lastFrameTime = now;

    // 顺序不能换：NewFrame(后端, rpd) 必须在 ImGui::NewFrame() 之前
    ImGui_ImplMetal_NewFrame(rpd);
    ImGui::NewFrame();

    frame_draw(vpW, vpH);                   // 骨骼 / 装备 / 物资
    espx::menu::draw(g_frame.scr, g_frame); // 菜单 + 常驻按钮

    ImGui::Render();

    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    if (enc) {
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
    }

    os_unfair_lock_unlock(&g_ioLock);

    [cb commit];        // 我们自己一条 CB；游戏那条在 present 里它自己 commit
    ++g_frames;
}

// ---------------- 挂点 ----------------
static EDrawableRef (*orig_nextDrawable)(id, SEL) = nullptr;
static void (*orig_present)(id, SEL) = nullptr;
static void (*orig_presentAtTime)(id, SEL, CFTimeInterval) = nullptr;
static void (*orig_sendEvent)(id, SEL, UIEvent*) = nullptr;

static bool g_presentHooked = false;
static bool g_inDraw = false;

static __unsafe_unretained UITouch* g_capTouch = nil;   // 当前被我们 UI 抓走的触摸

static void do_draw(EDrawableRef d, float vpW, float vpH) {
    if (!d) return;
    if (!g_m.init) {
        id<ELayer> lay = (id<ELayer>)[d layer];
        id<MTLDevice> dev = lay ? [lay device] : nil;
        if (!dev) dev = MTLCreateSystemDefaultDevice();
        if (!bringup(dev)) return;      // 建不起来就不画，下一帧再试
    }
    draw_frame(d, vpW, vpH);
}

static void hk_present(id self, SEL _cmd);
static void hk_presentAtTime(id self, SEL _cmd, CFTimeInterval t);

static void hook_present_on(Class concreteCls) {
    if (g_presentHooked || !concreteCls) return;

    Method mp = class_getInstanceMethod(concreteCls, @selector(present));
    if (mp) {
        orig_present = (void (*)(id, SEL))method_getImplementation(mp);
        method_setImplementation(mp, (IMP)hk_present);
    }
    Method mt = class_getInstanceMethod(concreteCls, @selector(presentAtTime:));
    if (mt) {
        orig_presentAtTime = (void (*)(id, SEL, CFTimeInterval))method_getImplementation(mt);
        method_setImplementation(mt, (IMP)hk_presentAtTime);
    }
    g_presentHooked = (mp != NULL) || (mt != NULL);
}

static EDrawableRef hk_nextDrawable(id self, SEL _cmd) {
    EDrawableRef d = orig_nextDrawable ? orig_nextDrawable(self, _cmd) : nil;
    if (d && !g_presentHooked) hook_present_on(object_getClass(d));
    return d;
}

static void hk_present(id self, SEL _cmd) {
    if (g_inDraw) {                                  // 防重入（有引擎会嵌套 present）
        if (orig_present) orig_present(self, _cmd);
        return;
    }
    g_inDraw = true;
    @autoreleasepool {
        EDrawableRef d = (EDrawableRef)self;
        id<ELayer> lay = (id<ELayer>)[d layer];
        const CGSize sz = lay ? [lay drawableSize] : CGSizeMake(0, 0);
        if (sz.width >= 2.0 && sz.height >= 2.0) {
            update_screen(d, (float)sz.width, (float)sz.height);
            do_draw(d, (float)sz.width, (float)sz.height);
        }
    }
    g_inDraw = false;
    if (orig_present) orig_present(self, _cmd);
}

static void hk_presentAtTime(id self, SEL _cmd, CFTimeInterval t) {
    if (g_inDraw) {
        if (orig_presentAtTime) orig_presentAtTime(self, _cmd, t);
        return;
    }
    g_inDraw = true;
    @autoreleasepool {
        EDrawableRef d = (EDrawableRef)self;
        id<ELayer> lay = (id<ELayer>)[d layer];
        const CGSize sz = lay ? [lay drawableSize] : CGSizeMake(0, 0);
        if (sz.width >= 2.0 && sz.height >= 2.0) {
            update_screen(d, (float)sz.width, (float)sz.height);
            do_draw(d, (float)sz.width, (float)sz.height);
        }
    }
    g_inDraw = false;
    if (orig_presentAtTime) orig_presentAtTime(self, _cmd, t);
}

// 触摸：只认"起点落在我们 UI 上"的那一根手指。
// 起点在 UI 里 -> 抓走、喂 ImGui、按设置决定要不要吞掉事件；
// 起点在别处 -> 一行都不动，游戏正常收到。
static void hk_sendEvent(id self, SEL _cmd, UIEvent* ev) {
    bool swallow = false;

    @autoreleasepool {
        if (ev && ev.type == UIEventTypeTouches) {
            const float sc = (g_frame.scr.scale > 0.0f) ? g_frame.scr.scale : 1.0f;
            NSSet* all = [ev allTouches];
            if (all && all.count > 0) {
                for (UITouch* t in all) {
                    const CGPoint p = [t locationInView:nil];    // 窗口坐标（点）
                    const float px = (float)p.x * sc;
                    const float py = (float)p.y * sc;

                    switch (t.phase) {
                        case UITouchPhaseBegan:
                            if (!g_capTouch && espx::menu::hit_button(px, py)) {
                                g_capTouch = t;
                                espx::menu::toggle();
                                swallow = true;
                            } else if (!g_capTouch && espx::menu::hit_window(px, py)) {
                                g_capTouch = t;
                                swallow = true;
                            }
                            if (t == g_capTouch) espx::on_touch(px, py, 0);
                            break;

                        case UITouchPhaseMoved:
                            if (t == g_capTouch) {
                                swallow = true;
                                espx::on_touch(px, py, 1);
                            }
                            break;

                        case UITouchPhaseEnded:
                        case UITouchPhaseCancelled:
                            if (t == g_capTouch) {
                                g_capTouch = nil;
                                swallow = true;
                                espx::on_touch(px, py, 2);
                            }
                            break;

                        default:
                            break;
                    }
                }
            }
        }
    }

    if (!(swallow && espx::settings().swallowTouch)) {
        if (orig_sendEvent) orig_sendEvent(self, _cmd, ev);
    }
}

// ---------------- 对外接口 ----------------
namespace espx {

void install() {
    // CAMetalLayer 是稳定的类，直接换实例方法（hook::swizzle_class_method 内部就是
    // class_getInstanceMethod + method_setImplementation，不碰 __text，不涉及 PAC）。
    orig_nextDrawable = (EDrawableRef (*)(id, SEL))
        hook::swizzle_class_method("CAMetalLayer", @selector(nextDrawable), (IMP)hk_nextDrawable);

    orig_sendEvent = (void (*)(id, SEL, UIEvent*))
        hook::swizzle_class_method("UIApplication", @selector(sendEvent:), (IMP)hk_sendEvent);

    NSLog(@"[espx] install nextDrawable=%p sendEvent=%p",
          (void*)orig_nextDrawable, (void*)orig_sendEvent);
}

bool ready() { return g_m.init; }

void on_frame(
#ifdef __OBJC__
    id drawable,
#else
    void* drawable,
#endif
    float vpW, float vpH) {
#ifdef __OBJC__
    EDrawableRef d = (EDrawableRef)drawable;
    if (!d || vpW < 2.0f || vpH < 2.0f) return;
    update_screen(d, vpW, vpH);
    do_draw(d, vpW, vpH);
#else
    (void)drawable;
    (void)vpW;
    (void)vpH;
#endif
}

bool on_touch(float px, float py, int phase) {
    if (!ImGui::GetCurrentContext()) return false;
    if (!g_frame.scr.valid()) return false;

    os_unfair_lock_lock(&g_ioLock);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(px, py);
    if (phase == 0)      io.AddMouseButtonEvent(0, true);
    else if (phase == 2) io.AddMouseButtonEvent(0, false);
    os_unfair_lock_unlock(&g_ioLock);
    return true;
}

int64_t frame_count() { return g_frames; }

} // namespace espx
