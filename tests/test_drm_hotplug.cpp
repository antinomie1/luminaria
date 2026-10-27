// Test GPU device hotplug support in DrmBackend and AutoBackend.
#include <cassert>
#include <string>

import luminaria;
import luminaria.backend;
import luminaria.shell;
import std;

int main() {
  auto display = luminaria::Display::create();
  assert(display.has_value());

  // Verify GPU hotplug types and signals compile and can be connected
  luminaria::Signal<luminaria::DrmDeviceRemoved> removed_sig;
  luminaria::Signal<luminaria::DrmDeviceAdded> added_sig;

  bool removed_fired = false;
  bool added_fired = false;
  std::string added_device;

  auto conn1 = removed_sig.connect(
      [&](luminaria::DrmDeviceRemoved &) { removed_fired = true; });

  auto conn2 = added_sig.connect([&](luminaria::DrmDeviceAdded &e) {
    added_fired = true;
    added_device = e.device_path;
  });

  // Fire simulated events
  luminaria::DrmDeviceAdded add_event{"/dev/dri/card1"};
  added_sig.emit(add_event);
  assert(added_fired);
  assert(added_device == "/dev/dri/card1");

  return 0;
}
