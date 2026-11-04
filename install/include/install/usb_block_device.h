/* Copyright (C) 2026 PixelExperience-Reborn
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string_view>

namespace pe::recovery {
// Qualcomm internal UFS is also named sdX: require a real USB sysfs ancestor.
inline bool IsUsbBlockPartition(std::string_view name, std::string_view sysfs_path) {
  if (name.size() < 4 || name[0] != 's' || name[1] != 'd') return false;
  size_t i = 2;
  while (i < name.size() && name[i] >= 'a' && name[i] <= 'z') ++i;
  if (i == 2 || i == name.size() || name[i] < '1' || name[i] > '9') return false;
  for (; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  constexpr std::string_view prefix = "/sys/devices/";
  if (sysfs_path.substr(0, prefix.size()) != prefix) return false;
  const size_t last = sysfs_path.rfind('/');
  if (last == std::string_view::npos || sysfs_path.substr(last + 1) != name) return false;

  size_t pos = 0;
  while (pos < sysfs_path.size()) {
    size_t end = sysfs_path.find('/', pos);
    if (end == std::string_view::npos) end = sysfs_path.size();
    const auto component = sysfs_path.substr(pos, end - pos);
    if (component.size() >= 4 && component.substr(0, 3) == "usb") {
      bool numeric = true;
      for (size_t j = 3; j < component.size(); ++j) {
        if (component[j] < '0' || component[j] > '9') numeric = false;
      }
      if (numeric) return true;
    }
    pos = end + 1;
  }
  return false;
}
}  // namespace pe::recovery
