// macOS's input events, for their own timestamps: when the device's report
// reached the system, which GLFW's callbacks don't carry.
#include <circles/profiler.h>

#import <AppKit/AppKit.h>

namespace circles {

namespace {

f64 last_event_time = -1;

bool classify(NSEvent* event, InputKind& kind, InputDevice& device) {
  device = InputDevice::Mouse;
  switch (event.type) {
    case NSEventTypeKeyDown:
      if (event.isARepeat)
        return false;
      kind = InputKind::KeyDown;
      device = InputDevice::Keyboard;
      return true;
    case NSEventTypeKeyUp:
      kind = InputKind::KeyUp;
      device = InputDevice::Keyboard;
      return true;
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
      kind = InputKind::ButtonDown;
      break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
      kind = InputKind::ButtonUp;
      break;
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
      kind = InputKind::Move;
      break;
    case NSEventTypeTabletPoint:
      kind = InputKind::Move;
      device = InputDevice::Tablet;
      return true;
    default:
      return false;
  }
  // A pen's movement and taps arrive as mouse events marked as tablet ones.
  if (event.subtype == NSEventSubtypeTabletPoint)
    device = InputDevice::Tablet;
  return true;
}

}  // namespace

void install_input_monitor(Profiler& profiler) {
  Profiler* const     target = &profiler;
  const NSEventMask   mask =
      NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskLeftMouseDown |
      NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown |
      NSEventMaskRightMouseUp | NSEventMaskMouseMoved |
      NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
      NSEventMaskTabletPoint;
  [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                        handler:^NSEvent*(NSEvent* event) {
                                          InputKind   kind;
                                          InputDevice device;
                                          if (!classify(event, kind, device))
                                            return event;
                                          // Seen before GLFW's callback for
                                          // the same event.
                                          if (kind != InputKind::Move)
                                            last_event_time = event.timestamp;
                                          if (target->active &&
                                              target->in_frame) {
                                            target->inputs.push_back(
                                                {event.timestamp, uptime(),
                                                 static_cast<u32>(
                                                     target->frames.size()),
                                                 kind, device});
                                            ++target->current.inputs;
                                          }
                                          return event;
                                        }];
}

f64 last_input_event_time() {
  return last_event_time;
}

void set_mouse_coalescing(bool enabled) {
  NSEvent.mouseCoalescingEnabled = enabled;
}

}  // namespace circles
