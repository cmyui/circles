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

}  // namespace circles
