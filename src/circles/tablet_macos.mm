// The connected tablets' reports, through the pointer listener
// (profiler_macos.mm), and importing the area from OpenTabletDriver.
#include <circles/tablet.h>

#import <AppKit/AppKit.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hid/IOHIDLib.h>
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace circles {

namespace {

// A supported tablet's pen interface, while it's connected.
struct Tablet {
  IOHIDDeviceRef     device;
  const TabletModel* model;
  i32                restore = -1;  // the mode it had, if we changed it
};

std::mutex             mutex;
std::vector<Tablet>    tablets;  // in the order they connected
bool                   reading = false;
std::vector<PenReport> reports;
f64                    last_arrived = -1;

// Sets a tablet's mode, by the feature report its format names: on, the
// value that turns pen reports on; off, back to the one it had. Called with
// the mutex held.
void switch_mode(Tablet& tablet, bool pen) {
  const auto [id, value] = format_setup(tablet.model->format);
  if (pen) {
    u8        mode[2] = {id, 0};
    CFIndex   length = sizeof(mode);
    const i32 got = IOHIDDeviceGetReport(
        tablet.device, kIOHIDReportTypeFeature, id, mode, &length);
    if (got != kIOReturnSuccess) {
      std::fprintf(stderr, "tablet: can't read its mode (0x%x)\n", got);
      return;
    }
    if (mode[1] == value)
      return;
    tablet.restore = mode[1];
    const u8  pen_mode[2] = {id, value};
    const i32 set = IOHIDDeviceSetReport(
        tablet.device, kIOHIDReportTypeFeature, id, pen_mode, sizeof(pen_mode));
    if (set != kIOReturnSuccess)
      std::fprintf(stderr, "tablet: can't switch it to pen reports (0x%x)\n",
                   set);
  } else if (tablet.restore >= 0) {
    const u8 mode[2] = {id, static_cast<u8>(tablet.restore)};
    IOHIDDeviceSetReport(tablet.device, kIOHIDReportTypeFeature, id, mode,
                         sizeof(mode));
    tablet.restore = -1;
  }
}

i32 int_property(IOHIDDeviceRef ref, CFStringRef key) {
  return [(__bridge NSNumber*)IOHIDDeviceGetProperty(ref, key) intValue];
}

}  // namespace

const TabletModel* connected_tablet() {
  std::lock_guard lock(mutex);
  return tablets.empty() ? nullptr : tablets.back().model;
}

bool import_tablet_area(TabletArea& area, f32& tip_threshold) {
  const TabletModel* connected = connected_tablet();
  @autoreleasepool {
    NSString* path = [NSHomeDirectory()
        stringByAppendingPathComponent:
            @"Library/Application Support/OpenTabletDriver/settings.json"];
    NSData*   data = [NSData dataWithContentsOfFile:path];
    if (!data)
      return false;
    NSDictionary* root = [NSJSONSerialization JSONObjectWithData:data
                                                         options:0
                                                           error:nil];
    if (![root isKindOfClass:[NSDictionary class]])
      return false;
    for (NSDictionary* profile in root[@"Profiles"]) {
      NSString* name = profile[@"Tablet"];
      if (![name isKindOfClass:[NSString class]] ||
          ![profile[@"OutputMode"][@"Path"] hasSuffix:@"AbsoluteMode"])
        continue;
      const bool wanted =
          connected
              ? [name isEqualToString:@(connected->driver_name)]
              : std::any_of(std::begin(kTablets), std::end(kTablets),
                            [&](const TabletModel& m) {
                              return [name isEqualToString:@(m.driver_name)];
                            });
      if (!wanted)
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
  for (Tablet& tablet : tablets)
    switch_mode(tablet, on);
  if (!on)
    reports.clear();
}

void take_pen_reports(std::vector<PenReport>& out) {
  std::lock_guard lock(mutex);
  out.insert(out.end(), reports.begin(), reports.end());
  reports.clear();
}

const TabletModel* tablet_device(void* ref) {
  auto               hid = static_cast<IOHIDDeviceRef>(ref);
  const TabletModel* found = find_tablet(
      static_cast<u16>(int_property(hid, CFSTR(kIOHIDVendorIDKey))),
      static_cast<u16>(int_property(hid, CFSTR(kIOHIDProductIDKey))),
      static_cast<u32>(int_property(hid, CFSTR(kIOHIDMaxInputReportSizeKey))));
  if (!found)
    return nullptr;
  std::lock_guard lock(mutex);
  tablets.push_back({hid, found});
  if (reading)
    switch_mode(tablets.back(), true);
  return found;
}

void tablet_removed(void* ref) {
  std::lock_guard lock(mutex);
  std::erase_if(tablets, [&](const Tablet& t) { return t.device == ref; });
}

void tablet_report(const TabletModel&  model,
                   std::span<const u8> report,
                   f64                 arrived) {
  std::lock_guard lock(mutex);
  PenReport       pen;
  // macOS can list the interface twice; each report counts once.
  if (!reading || arrived == last_arrived ||
      !decode_pen_report(model, report, arrived, pen))
    return;
  last_arrived = arrived;
  reports.push_back(pen);
}

}  // namespace circles
