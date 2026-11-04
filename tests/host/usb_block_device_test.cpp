/* Copyright (C) 2026 PixelExperience-Reborn
 * SPDX-License-Identifier: Apache-2.0
 */
#include <cassert>
#include "install/usb_block_device.h"

int main() {
  using pe::recovery::IsUsbBlockPartition;
  constexpr auto usb =
      "/sys/devices/platform/soc/8804000.ssusb/xhci-hcd.0.auto/usb1/1-1/"
      "1-1:1.0/host6/target6:0:0/6:0:0:0/block/sda/sda1";
  assert(IsUsbBlockPartition("sda1", usb));
  assert(IsUsbBlockPartition("sdaa12",
      "/sys/devices/pci0000:00/usb2/2-1/host7/target7:0:0/block/sdaa/sdaa12"));
  assert(!IsUsbBlockPartition("sda1",
      "/sys/devices/platform/soc/1d84000.ufshc/host0/target0:0:0/block/sda/sda1"));
  assert(!IsUsbBlockPartition("sda1", "/sys/devices/platform/soc/dummy-usb1/sda/sda1"));
  assert(!IsUsbBlockPartition("sda", "/sys/devices/platform/soc/usb1/block/sda"));
  assert(!IsUsbBlockPartition("sda0", usb));
  assert(!IsUsbBlockPartition("sda1x", usb));
  assert(!IsUsbBlockPartition("sda1", "/sys/devices/platform/soc/usb1/block/sdb/sdb1"));
  assert(!IsUsbBlockPartition("sda1", "/sys/class/block/sda1"));
  assert(!IsUsbBlockPartition("mmcblk0p1",
      "/sys/devices/platform/soc/usb1/block/mmcblk0/mmcblk0p1"));
  return 0;
}
