// Apple window handle for the Latte renderer. Metal receives the content view; Vulkan (MoltenVK)
// receives the same view and wraps it in a CAMetalLayer inside Cemu's CocoaSurface.
#include "botw_target.h"
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include <SDL3/SDL.h>

#include <stdexcept>

#include "../latte/latte.h"
#include "../log.h"

namespace window {
void window_attach_renderer(SDL_Window* win, int point_w, int point_h) {
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
#if TARGET_OS_IOS
    UIWindow* uiwin = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    void* view = (__bridge void*)uiwin.rootViewController.view;
#else
    NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    void* view = (__bridge void*)nswin.contentView;
#endif
    if (!view) {
        LOG("FATAL: SDL did not expose a Cocoa/UIKit view");
        throw std::runtime_error("SDL did not expose a Cocoa/UIKit view");
    }
    latte::set_host_surface(latte::HostSurface::Cocoa, nullptr, view);
    latte::create_renderer(view, point_w, point_h);
}
}  // namespace window
