// The tablet's reports, through the pointer listener (profiler_macos.mm),
// and importing its area from OpenTabletDriver.
#include <circles/tablet.h>

#import <AppKit/AppKit.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hid/IOHIDLib.h>
#include <cstdio>
#include <mutex>

namespace circles {

namespace {

// The CTL-472's pen interface, from its USB IDs and report size.
constexpr i32          kVendor = 0x056A, kProduct = 0x037A, kReportSize = 10;

std::mutex             mutex;
IOHIDDeviceRef         device = nullptr;  // the pen's interface, once found
bool                   reading = false;
i32                    restore = -1;  // the mode it had, if we changed it
std::vector<PenReport> reports;
f64                    last_arrived = -1;

// Feature report 2 sets the tablet's mode: 2 sends pen reports, which is
// what OpenTabletDriver sets it to. Called with the mutex held.
void switch_mode(bool pen) {
  if (!device)
    return;
  if (pen) {
    u8        mode[2] = {2, 0};
    CFIndex   length = sizeof(mode);
    const i32 got =
        IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, 2, mode, &length);
    if (got != kIOReturnSuccess) {
      std::fprintf(stderr, "tablet: can't read its mode (0x%x)\n", got);
      return;
    }
    if (mode[1] == 2)
      return;
    restore = mode[1];
    const u8  pen_mode[2] = {2, 2};
    const i32 set = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, 2,
                                         pen_mode, sizeof(pen_mode));
    if (set != kIOReturnSuccess)
      std::fprintf(stderr, "tablet: can't switch it to pen reports (0x%x)\n",
                   set);
  } else if (restore >= 0) {
    const u8 mode[2] = {2, static_cast<u8>(restore)};
    IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature, 2, mode,
                         sizeof(mode));
    restore = -1;
  }
}

i32 int_property(IOHIDDeviceRef ref, CFStringRef key) {
  return [(__bridge NSNumber*)IOHIDDeviceGetProperty(ref, key) intValue];
}

}  // namespace

bool import_tablet_area(TabletArea& area, f32& tip_threshold) {
  @autoreleasepool {
    NSString* path = [NSHomeDirectory()
        stringByAppendingPathComponent:
            @"Library/Application Support/OpenTabletDriver/settings.json"];
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data)
      return false;
    NSDictionary* root = [NSJSONSerialization JSONObjectWithData:data
                                                         options:0
                                                           error:nil];
    if (![root isKindOfClass:[NSDictionary class]])
      return false;
    for (NSDictionary* profile in root[@"Profiles"]) {
      if (![profile[@"Tablet"] isEqual:@"Wacom CTL-472"] ||
          ![profile[@"OutputMode"][@"Path"] hasSuffix:@"AbsoluteMode"])
        continue;
      NSDictionary* tablet = profile[@"AbsoluteModeSettings"][@"Tablet"];
      if ([tablet[@"Width"] floatValue] <= 0 ||
          [tablet[@"Height"] floatValue] <= 0)
        return false;
      area.x = [tablet[@"X"] floatValue];
      area.y = [tablet[@"Y"] floatValue];
      area.width = [tablet[@"Width"] floatValue];
      area.height = [tablet[@"Height"] floatValue];
      area.rotation = [tablet[@"Rotation"] floatValue];
      tip_threshold =
          [profile[@"Bindings"][@"TipActivationThreshold"] floatValue];
      return true;
    }
    return false;
  }
}

void read_tablet(bool on) {
  std::lock_guard lock(mutex);
  reading = on;
  switch_mode(on);
  if (!on)
    reports.clear();
}

void take_pen_reports(std::vector<PenReport>& out) {
  std::lock_guard lock(mutex);
  out.insert(out.end(), reports.begin(), reports.end());
  reports.clear();
}

bool tablet_device(void* ref) {
  auto hid = static_cast<IOHIDDeviceRef>(ref);
  if (int_property(hid, CFSTR(kIOHIDVendorIDKey)) != kVendor ||
      int_property(hid, CFSTR(kIOHIDProductIDKey)) != kProduct ||
      int_property(hid, CFSTR(kIOHIDMaxInputReportSizeKey)) != kReportSize)
    return false;
  std::lock_guard lock(mutex);
  device = hid;
  if (reading)
    switch_mode(true);
  return true;
}

void tablet_report(std::span<const u8> report, f64 arrived) {
  PenReport pen;
  if (!decode_pen_report(report, arrived, pen))
    return;
  std::lock_guard lock(mutex);
  // macOS can list the interface twice; each report counts once.
  if (!reading || arrived == last_arrived)
    return;
  last_arrived = arrived;
  reports.push_back(pen);
}

}  // namespace circles
