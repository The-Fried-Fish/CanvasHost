/**
 * @file
 * @brief Owns a virtual HID touchscreen and submits native input reports.
 */
#include "native_touch.h"

#include "src/logging.h"

#include <algorithm>
#include <cmath>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <moonlight-common-c/src/Limelight.h>
#include <unistd.h>

namespace platf::macos {
  namespace {
    /** @brief Creates a digitizer report descriptor with one finger collection per slot. */
    CFDataRef create_descriptor() {
      const uint8_t header[] = {0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01};
      const uint8_t finger[] = {
        0x05,
        0x0d,
        0x09,
        0x22,
        0xa1,
        0x02,
        0x09,
        0x42,
        0x09,
        0x32,
        0x09,
        0x47,
        0x15,
        0x00,
        0x25,
        0x01,
        0x75,
        0x01,
        0x95,
        0x03,
        0x81,
        0x02,
        0x75,
        0x05,
        0x95,
        0x01,
        0x81,
        0x03,
        0x09,
        0x51,
        0x25,
        0x7f,
        0x75,
        0x08,
        0x95,
        0x01,
        0x81,
        0x02,
        0x05,
        0x01,
        0x09,
        0x30,
        0x09,
        0x31,
        0x27,
        0xff,
        0xff,
        0x00,
        0x00,
        0x75,
        0x10,
        0x95,
        0x02,
        0x81,
        0x02,
        0xc0
      };
      const uint8_t end[] = {0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, mac_touch::max_contacts, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xc0};
      auto descriptor = CFDataCreateMutable(kCFAllocatorDefault, 0);
      CFDataAppendBytes(descriptor, header, sizeof(header));
      for (unsigned i = 0; i < mac_touch::max_contacts; ++i) {
        CFDataAppendBytes(descriptor, finger, sizeof(finger));
      }
      CFDataAppendBytes(descriptor, end, sizeof(end));
      return descriptor;
    }

    /**
     * @brief Checks whether the system digitizer service is registered for this device.
     * @param service Virtual device registry entry.
     * @return Whether the driver's event service is ready.
     */
    bool ready(io_service_t service) {
      io_iterator_t entries = IO_OBJECT_NULL;
      if (IORegistryEntryCreateIterator(service, kIOServicePlane, kIORegistryIterateRecursively, &entries) != KERN_SUCCESS) {
        return false;
      }
      bool found = false;
      while (auto entry = IOIteratorNext(entries)) {
        uint64_t id = 0;
        if (IOObjectConformsTo(entry, "AppleUserHIDEventService") &&
            IORegistryEntryGetRegistryEntryID(entry, &id) == KERN_SUCCESS) {
          auto registered = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(id));
          if (registered) {
            IOObjectRelease(registered);
            found = true;
          }
        }
        IOObjectRelease(entry);
        if (found) {
          break;
        }
      }
      IOObjectRelease(entries);
      return found;
    }

  }  // namespace

  bool touch_contacts_t::active() const {
    return std::ranges::any_of(slots, [](const auto &slot) {
      return slot.contact.id != 0;
    });
  }

  std::optional<mac_touch::report_t> touch_contacts_t::update(uint8_t type, uint32_t id, std::optional<std::array<float, 2>> position) {
    const auto valid_position = position && std::isfinite((*position)[0]) && std::isfinite((*position)[1]);
    const auto set_position = [&](auto &contact) {
      auto encode = [](float value, uint8_t (&bytes)[2]) {
        const auto fixed = static_cast<uint16_t>(std::lround(std::clamp(value, 0.f, 1.f) * 65535.f));
        bytes[0] = fixed & 0xff;
        bytes[1] = fixed >> 8;
      };
      encode((*position)[0], contact.x);
      encode((*position)[1], contact.y);
    };

    if (type == LI_TOUCH_EVENT_CANCEL_ALL) {
      if (!active()) {
        return std::nullopt;
      }
      for (auto &slot : slots) {
        if (slot.contact.id) {
          slot.contact.flags = mac_touch::changed;
        }
      }
    } else {
      auto slot = std::ranges::find_if(slots, [&](const auto &slot) {
        return slot.contact.id && slot.client_id == id;
      });
      if (type == LI_TOUCH_EVENT_DOWN || (type == LI_TOUCH_EVENT_MOVE && slot == slots.end())) {
        if (slot != slots.end() || !valid_position) {
          return std::nullopt;
        }
        slot = std::ranges::find_if(slots, [](const auto &slot) {
          return !slot.contact.id;
        });
        if (slot == slots.end()) {
          return std::nullopt;
        }
        slot->client_id = id;
        slot->contact.id = static_cast<uint8_t>(slot - slots.begin() + 1);
        slot->contact.flags = mac_touch::touching | mac_touch::changed | mac_touch::moved;
        set_position(slot->contact);
      } else if (slot == slots.end()) {
        return std::nullopt;
      } else if (type == LI_TOUCH_EVENT_MOVE && valid_position) {
        slot->contact.flags = mac_touch::touching | mac_touch::moved;
        set_position(slot->contact);
      } else if (type == LI_TOUCH_EVENT_UP || type == LI_TOUCH_EVENT_CANCEL) {
        slot->contact.flags = mac_touch::changed;
      } else {
        return std::nullopt;
      }
    }

    mac_touch::report_t report {};
    for (std::size_t i = 0; i < slots.size(); ++i) {
      auto &slot = slots[i];
      report.contacts[i] = slot.contact;
      if (slot.contact.flags & mac_touch::touching) {
        slot.contact.flags = mac_touch::touching;
      } else {
        slot = {};
      }
    }
    return report;
  }

  native_touch_t::~native_touch_t() {
    reset();
  }

  bool native_touch_t::connected() const {
    return device_ != nullptr;
  }

  bool native_touch_t::connect(uint32_t display) {
    if (!__builtin_available(macOS 27.0, *)) {
      return false;
    }
    display = display ? display : CGMainDisplayID();
    if (connected()) {
      return display_ == display;
    }

    static const auto create = reinterpret_cast<CFTypeRef (*)(CFAllocatorRef, CFDictionaryRef, uint32_t)>(
      dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCreateWithProperties")
    );
    static const auto get_service = reinterpret_cast<io_service_t (*)(CFTypeRef)>(
      dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCopyService")
    );
    static const auto get_uuid = reinterpret_cast<CFUUIDRef (*)(CGDirectDisplayID)>(
      dlsym(RTLD_DEFAULT, "CGDisplayCreateUUIDFromDisplayID")
    );
    if (!create || !get_service || !get_uuid || !CGDisplayIsOnline(display)) {
      return false;
    }

    auto uuid = get_uuid(display);
    if (!uuid) {
      return false;
    }
    auto uuid_string = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    CFRelease(uuid);
    const int64_t vendor_id = 0x1209;
    const int64_t product_id = 0x4c53;
    const int64_t display_id = display;
    auto vendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &vendor_id);
    auto product = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &product_id);
    auto display_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &display_id);
    auto descriptor = create_descriptor();
    const void *keys[] {
      CFSTR("Product"), CFSTR("VendorID"), CFSTR("ProductID"), CFSTR("Transport"), CFSTR("ReportDescriptor"),
      CFSTR("displayUUID"), CFSTR("CGDirectDisplayID"), CFSTR("DisplayIntegrated"), CFSTR("DigitizerCollectionDispatch")
    };
    const void *values[] {
      CFSTR("VoidLink HID Touchscreen"), vendor, product, CFSTR("Virtual"), descriptor,
      uuid_string, display_number, kCFBooleanTrue, kCFBooleanTrue
    };
    auto properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, std::size(keys),
                                         &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    device_ = create(kCFAllocatorDefault, properties, 0);
    for (auto value : {static_cast<CFTypeRef>(properties), static_cast<CFTypeRef>(descriptor),
                       static_cast<CFTypeRef>(vendor), static_cast<CFTypeRef>(product),
                       static_cast<CFTypeRef>(uuid_string), static_cast<CFTypeRef>(display_number)}) {
      CFRelease(value);
    }
    if (!device_) {
      return false;
    }

    auto service = get_service(device_);
    bool available = false;
    for (unsigned i = 0; service && i < 100 && !available; ++i) {
      available = ready(service);
      if (!available) {
        usleep(10000);
      }
    }
    if (service) {
      IOObjectRelease(service);
    }
    if (!available) {
      BOOST_LOG(debug) << "macOS native touch device did not become ready";
      disconnect();
      return false;
    }
    // Allow WindowServer to register the newly matched digitizer before the first contact.
    usleep(150000);
    display_ = display;
    return true;
  }

  bool native_touch_t::send(const mac_touch::report_t &report) {
    static const auto post = reinterpret_cast<IOReturn (*)(CFTypeRef, uint64_t, const uint8_t *, CFIndex)>(
      dlsym(RTLD_DEFAULT, "IOHIDUserDeviceHandleReportWithTimeStamp")
    );
    const auto hid = mac_touch::encode_hid_report(report);
    return post && post(device_, mach_absolute_time(), reinterpret_cast<const uint8_t *>(&hid), sizeof(hid)) == kIOReturnSuccess;
  }

  void native_touch_t::update(uint32_t display, uint8_t type, uint32_t id, std::optional<std::array<float, 2>> position) {
    if (display && display != display_) {
      reset();
      if (type != LI_TOUCH_EVENT_DOWN || !connect(display)) {
        return;
      }
    }
    if (auto report = contacts_.update(type, id, position); report && !send(*report)) {
      BOOST_LOG(warning) << "macOS native touch report failed";
      disconnect();
    }
  }

  void native_touch_t::disconnect() {
    if (device_) {
      CFRelease(device_);
      device_ = nullptr;
    }
    contacts_ = {};
    display_ = 0;
  }

  void native_touch_t::reset() {
    if (connected()) {
      if (const auto report = contacts_.update(LI_TOUCH_EVENT_CANCEL_ALL, 0, std::nullopt)) {
        send(*report);
      }
      disconnect();
    }
  }
}  // namespace platf::macos
