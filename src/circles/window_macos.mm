// Window behaviour GLFW doesn't cover on macOS.
#include <circles/app.h>

#import <AppKit/AppKit.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace circles {

void toggle_fullscreen(App& app) {
  // macOS's own full screen, as the green button enters: the display keeps
  // its resolution, where GLFW's would switch to a lower one.
  NSWindow* window = glfwGetCocoaWindow(app.window);
  [window toggleFullScreen:nil];
}

void window_screen(App& app, Vec2& origin, Vec2& size) {
  NSWindow* window = glfwGetCocoaWindow(app.window);
  NSScreen* screen = window.screen;
  if (!screen) {
    origin = {0, 0};
    size = {app.width, app.height};
    return;
  }
  // Cocoa's coordinates rise upward; the window's run down from its
  // content's top-left.
  const NSRect content = [window contentRectForFrameRect:window.frame];
  const NSRect frame = screen.frame;
  origin = {static_cast<f32>(NSMinX(frame) - NSMinX(content)),
            static_cast<f32>(NSMaxY(content) - NSMaxY(frame))};
  size = {static_cast<f32>(NSWidth(frame)), static_cast<f32>(NSHeight(frame))};
}

}  // namespace circles
