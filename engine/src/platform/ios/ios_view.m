// Puts SDL's OpenGL ES view into the app's own window (IOS_PORT.md 5.1).
//
// SDL creates its own UIWindow for the game. The app's window (storyboard:
// touch overlay, Game Folder button, vInputKit) stays in front, so the game
// view is moved into the app's root view, below the overlay, as the Go
// engine did with its Metal layer (IkemenAttachMetalLayer). The view keeps
// its CAEAGLLayer and renderbuffer; SDL resizes the renderbuffer in the
// view's layoutSubviews when the constraints change its size.
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include "platform/ios/ios_sys.h"

void cvf_ios_attach_gl_view(void *sdl_window) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!sdl_window || !SDL_GetWindowWMInfo((SDL_Window *)sdl_window, &wm)) {
        fprintf(stderr, "cvf: attach GL view: no window info (%s)\n", SDL_GetError());
        return;
    }
    UIWindow *sdlWindow = wm.info.uikit.window;
    UIView *glView = sdlWindow.rootViewController.view;
    UIWindow *host = nil;
    for (UIWindowScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        for (UIWindow *w in scene.windows)
            if (w != sdlWindow && w.rootViewController && !host) host = w;
    }
    if (!glView || !host) {
        fprintf(stderr, "cvf: attach GL view: %s\n", glView ? "no app window" : "no SDL view");
        return;
    }
    UIView *hostView = host.rootViewController.view;
    [glView removeFromSuperview];
    glView.translatesAutoresizingMaskIntoConstraints = NO;
    glView.userInteractionEnabled = NO;  // touches go to the app's overlay
    [hostView insertSubview:glView atIndex:0];
    [NSLayoutConstraint activateConstraints:@[
        [glView.topAnchor constraintEqualToAnchor:hostView.topAnchor],
        [glView.bottomAnchor constraintEqualToAnchor:hostView.bottomAnchor],
        [glView.leadingAnchor constraintEqualToAnchor:hostView.leadingAnchor],
        [glView.trailingAnchor constraintEqualToAnchor:hostView.trailingAnchor],
    ]];
    [host makeKeyAndVisible];
    sdlWindow.hidden = YES;
    [hostView layoutIfNeeded];
    fprintf(stderr, "cvf: GL view moved into the app window (%.0fx%.0f pt, scale %.1f)\n", hostView.bounds.size.width,
            hostView.bounds.size.height, glView.contentScaleFactor);
}
