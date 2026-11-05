// macOS's input events, for their own timestamps: when the device's report
// reached the system, which GLFW's callbacks don't carry.
#include <circles/profiler.h>
#include <circles/tablet.h>

#import <AppKit/AppKit.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hid/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hid/IOHIDUsageTables.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace circles {

namespace {

f64                      last_event_time = -1;
std::vector<InputTiming> presses, moves;
NSEventModifierFlags     modifiers = 0;

// Pointing devices' raw reports, gathered on a thread of their own.
struct PointerListener {
  std::atomic<PointerAccess>        access{PointerAccess::Asked};
  std::mutex                        mutex;
  std::vector<f64>                  times;
  // Reports' buffers, one per device. Kept after it leaves, in case a
  // report is still on its way.
  std::unordered_map<IOHIDDeviceRef, std::vector<uint8_t>> buffers;
  f64                               tick = 0;  // seconds per mach unit
};
PointerListener* pointers = nullptr;

// Each report's arrival, for timing the driver; the tablet's (`tablet` set)
// also go to be read.
void on_pointer_report(void* tablet,
                       IOReturn,
                       void*,
                       IOHIDReportType,
                       uint32_t,
                       uint8_t* report,
                       CFIndex  length,
                       uint64_t timestamp) {
  const f64 arrived = timestamp * pointers->tick;
  {
    std::lock_guard lock(pointers->mutex);
    pointers->times.push_back(arrived);
  }
  if (tablet)
    tablet_report({report, static_cast<size_t>(length)}, arrived);
}

// Each device as it's matched, those present at the start included: its
// reports are read, and a tablet's are decoded too.
void on_device_matched(void*, IOReturn, void*, IOHIDDeviceRef ref) {
  const i32 size = [(__bridge NSNumber*)IOHIDDeviceGetProperty(
      ref, CFSTR(kIOHIDMaxInputReportSizeKey)) intValue];
  if (size <= 0)
    return;
  auto& buffer = pointers->buffers[ref];
  buffer.resize(size);
  void* tablet = tablet_device(ref) ? ref : nullptr;
  IOHIDDeviceRegisterInputReportWithTimeStampCallback(
      ref, buffer.data(), size, on_pointer_report, tablet);
}

void on_device_removed(void*, IOReturn, void*, IOHIDDeviceRef ref) {
  tablet_removed(ref);
}

// Waits for the permission, then reads reports for good, from devices as
// they come and go. On a thread of its own: each check of the permission
// keeps macOS busy for about 11 ms.
void listen() {
  for (bool asked = false;;) {
    const IOHIDAccessType access =
        IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (access == kIOHIDAccessTypeGranted)
      break;
    if (access == kIOHIDAccessTypeUnknown && !asked) {
      IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);  // macOS's prompt
      asked = true;
    }
    pointers->access = access == kIOHIDAccessTypeDenied ? PointerAccess::Denied
                                                        : PointerAccess::Asked;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  @autoreleasepool {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, 0);
    NSArray*        matching = @[
      @{
        @kIOHIDDeviceUsagePageKey : @(kHIDPage_GenericDesktop),
        @kIOHIDDeviceUsageKey : @(kHIDUsage_GD_Mouse)
      },
      @{
        @kIOHIDDeviceUsagePageKey : @(kHIDPage_GenericDesktop),
        @kIOHIDDeviceUsageKey : @(kHIDUsage_GD_Pointer)
      },
      @{@kIOHIDDeviceUsagePageKey : @(kHIDPage_Digitizer)},
    ];
    IOHIDManagerSetDeviceMatchingMultiple(manager,
                                          (__bridge CFArrayRef)matching);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, on_device_matched,
                                               nullptr);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, on_device_removed,
                                              nullptr);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(),
                                    kCFRunLoopDefaultMode);
    // Shared, so drivers such as OpenTabletDriver keep reading too.
    IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
  }
  pointers->access = PointerAccess::Listening;
  CFRunLoopRun();
}

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
    case NSEventTypeFlagsChanged: {
      // A modifier key: down if it set a flag that wasn't.
      const NSEventModifierFlags flags =
          event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
      kind = flags & ~modifiers ? InputKind::KeyDown : InputKind::KeyUp;
      modifiers = flags;
      device = InputDevice::Keyboard;
      return true;
    }
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
      kind = InputKind::ButtonDown;
      break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
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
      NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
      NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
      NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
      NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp |
      NSEventMaskMouseMoved |
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
                                          if (kind == InputKind::KeyDown ||
                                              kind == InputKind::ButtonDown)
                                            presses.push_back(
                                                {event.timestamp, uptime()});
                                          else if (kind == InputKind::Move)
                                            moves.push_back(
                                                {event.timestamp, uptime()});
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

void take_input_presses(std::vector<InputTiming>& out) {
  out.insert(out.end(), presses.begin(), presses.end());
  presses.clear();
}

void take_input_moves(std::vector<InputTiming>& out) {
  out.insert(out.end(), moves.begin(), moves.end());
  moves.clear();
}

PointerAccess listen_to_pointers() {
  if (!pointers) {
    pointers = new PointerListener;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    pointers->tick = static_cast<f64>(timebase.numer) / timebase.denom * 1e-9;
    std::thread(listen).detach();
  }
  return pointers->access;
}

void take_pointer_reports(std::vector<f64>& times) {
  if (!pointers)
    return;
  std::lock_guard lock(pointers->mutex);
  times.insert(times.end(), pointers->times.begin(), pointers->times.end());
  pointers->times.clear();
}

f64 keyboard_report_interval() {
  @autoreleasepool {
    f64           slowest = 0;
    io_iterator_t  devices = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOHIDDevice"),
                                     &devices) != KERN_SUCCESS)
      return slowest;
    for (io_object_t device; (device = IOIteratorNext(devices));
         IOObjectRelease(device)) {
      const auto property = [&](const char* key) {
        return CFBridgingRelease(IORegistryEntryCreateCFProperty(
            device, (__bridge CFStringRef) @(key), kCFAllocatorDefault, 0));
      };
      bool keyboard = false;
      for (NSDictionary* pair in property(kIOHIDDeviceUsagePairsKey))
        keyboard |= [pair[@kIOHIDDeviceUsagePageKey] intValue] ==
                        kHIDPage_GenericDesktop &&
                    [pair[@kIOHIDDeviceUsageKey] intValue] ==
                        kHIDUsage_GD_Keyboard;
      const f64 interval = [property(kIOHIDReportIntervalKey) intValue] / 1000.0;
      if (keyboard)
        slowest = std::max(slowest, interval);
    }
    IOObjectRelease(devices);
    return slowest;
  }
}

void set_mouse_coalescing(bool enabled) {
  NSEvent.mouseCoalescingEnabled = enabled;
}

}  // namespace circles
