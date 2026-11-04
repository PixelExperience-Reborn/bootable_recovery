/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "install/fuse_install.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "bootloader_message/bootloader_message.h"
#include "fuse_provider.h"
#include "fuse_sideload.h"
#include "install/install.h"
#include "install/usb_block_device.h"
#include "recovery_utils/roots.h"

static constexpr const char* SDCARD_ROOT = "/sdcard";
static constexpr const char* USB_OTG_ROOT = "/mnt/pe-usbotg";
// How long (in seconds) we wait for the fuse-provided package file to
// appear, before timing out.
static constexpr int SDCARD_INSTALL_TIMEOUT = 10;

// Set the BCB to reboot back into recovery (it won't resume the install from
// sdcard though).
static void SetSdcardUpdateBootloaderMessage() {
  std::vector<std::string> options;
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
}

// Returns the selected filename, or an empty string.
static std::string BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui,
                                   bool allow_block_maps = true, size_t depth = 0) {
  // A USB filesystem is untrusted; bound recursion to avoid exhausting recovery's stack.
  if (depth > 32) {
    LOG(ERROR) << "Update package directory nesting too deep";
    return "";
  }
  // PE USB media is mounted directly, not through a fixed fs_mgr fstab entry.
  if (path != USB_OTG_ROOT &&
      !android::base::StartsWith(path, std::string(USB_OTG_ROOT) + "/")) {
    ensure_path_mounted(path);
  }

  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
  if (!d) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> entries{ "../" };  // "../" is always the first entry.

  dirent* de;
  while ((de = readdir(d.get())) != nullptr) {
    std::string name(de->d_name);

    if (de->d_type == DT_DIR) {
      // Skip "." and ".." entries.
      if (name == "." || name == "..") continue;
      dirs.push_back(name + "/");
    } else if (de->d_type == DT_REG && (android::base::EndsWithIgnoreCase(name, ".zip") ||
                                        (allow_block_maps && android::base::EndsWithIgnoreCase(name, ".map")))) {
      entries.push_back(name);
    }
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(entries.begin(), entries.end());

  // Append dirs to the entries list.
  entries.insert(entries.end(), dirs.begin(), dirs.end());

  std::vector<std::string> headers{ "Choose a package to install:", path };

  size_t chosen_item = 0;
  while (true) {
    chosen_item = ui->ShowMenu(
        headers, entries, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    // A menu can also time out or return an invalid selection. Never index the
    // untrusted-media file list with a KeyError converted to size_t.
    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED) ||
        chosen_item == static_cast<size_t>(RecoveryUI::KeyError::TIMED_OUT) ||
        chosen_item >= entries.size()) {
      return "";
    }

    const std::string& item = entries[chosen_item];
    if (chosen_item == 0) {
      // Go up but continue browsing (if the caller is BrowseDirectory).
      return "";
    }

    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = BrowseDirectory(new_path, device, ui, allow_block_maps, depth + 1);
      if (!result.empty()) return result;
    } else {
      // Selected a zip file: return the path to the caller.
      return new_path;
    }
  }

  // Unreachable.
}

static bool StartInstallPackageFuse(std::string_view path) {
  if (path.empty()) {
    return false;
  }

  constexpr auto FUSE_BLOCK_SIZE = 65536;
  bool is_block_map = android::base::ConsumePrefix(&path, "@");
  auto fuse_data_provider =
      is_block_map ? FuseBlockDataProvider::CreateFromBlockMap(std::string(path), FUSE_BLOCK_SIZE)
                   : FuseFileDataProvider::CreateFromFile(std::string(path), FUSE_BLOCK_SIZE);

  if (!fuse_data_provider || !fuse_data_provider->Valid()) {
    LOG(ERROR) << "Failed to create fuse data provider.";
    return false;
  }

  if (android::base::StartsWith(path, SDCARD_ROOT)) {
    // The installation process expects to find the sdcard unmounted. Unmount it with MNT_DETACH so
    // that our open file continues to work but new references see it as unmounted.
    umount2(SDCARD_ROOT, MNT_DETACH);
  }

  return run_fuse_sideload(std::move(fuse_data_provider)) == 0;
}

InstallResult InstallWithFuseFromPath(std::string_view path, Device* device) {
  // We used to use fuse in a thread as opposed to a process. Since accessing
  // through fuse involves going from kernel to userspace to kernel, it leads
  // to deadlock when a page fault occurs. (Bug: 26313124)
  auto ui = device->GetUI();
  pid_t child;
  if ((child = fork()) == 0) {
    bool status = StartInstallPackageFuse(path);

    _exit(status ? EXIT_SUCCESS : EXIT_FAILURE);
  }

  // FUSE_SIDELOAD_HOST_PATHNAME will start to exist once the fuse in child process is ready.
  InstallResult result = INSTALL_ERROR;
  int status;
  bool waited = false;
  for (int i = 0; i < SDCARD_INSTALL_TIMEOUT; ++i) {
    if (waitpid(child, &status, WNOHANG) == -1) {
      result = INSTALL_ERROR;
      waited = true;
      break;
    }

    struct stat sb;
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &sb) == -1) {
      if (errno == ENOENT && i < SDCARD_INSTALL_TIMEOUT - 1) {
        sleep(1);
        continue;
      } else {
        LOG(ERROR) << "Timed out waiting for the fuse-provided package.";
        result = INSTALL_ERROR;
        kill(child, SIGKILL);
        break;
      }
    }
    auto package =
        Package::CreateFilePackage(FUSE_SIDELOAD_HOST_PATHNAME,
                                   std::bind(&RecoveryUI::SetProgress, ui, std::placeholders::_1));
    result = InstallPackage(package.get(), FUSE_SIDELOAD_HOST_PATHNAME, false, 0 /* retry_count */,
                            device);
    break;
  }

  if (!waited) {
    // Calling stat() on this magic filename signals the fuse
    // filesystem to shut down.
    struct stat sb;
    stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &sb);

    waitpid(child, &status, 0);
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << "Error exit from the fuse process: " << WEXITSTATUS(status);
  }

  return result;
}

// Android17 replacement for PE's legacy external-volume update source.
// Do not parse, write or repartition GPT; USB block partitions come from the kernel.
static bool MountUsbOtgReadOnly(Device* device) {
  RecoveryUI* ui = device->GetUI();
  if (mkdir(USB_OTG_ROOT, 0700) != 0 && errno != EEXIST) {
    PLOG(ERROR) << "Cannot create USB OTG mountpoint";
    return false;
  }
  struct stat mountpoint_stat = {};
  if (lstat(USB_OTG_ROOT, &mountpoint_stat) != 0 || !S_ISDIR(mountpoint_stat.st_mode)) {
    LOG(ERROR) << "USB OTG mountpoint is not a directory";
    return false;
  }
  std::unique_ptr<DIR, decltype(&closedir)> nodes(opendir("/sys/class/block"), closedir);
  if (!nodes) {
    PLOG(ERROR) << "Cannot enumerate USB block devices";
    return false;
  }
  std::vector<std::string> partitions;
  while (dirent* entry = readdir(nodes.get())) {
    const std::string name(entry->d_name);
    char resolved[PATH_MAX] = {};
    const std::string sysfs = "/sys/class/block/" + name;
    if (realpath(sysfs.c_str(), resolved) == nullptr ||
        !pe::recovery::IsUsbBlockPartition(name, resolved)) {
      continue;
    }
    const std::string block = "/dev/block/" + name;
    struct stat st = {};
    if (stat(block.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) partitions.push_back(block);
  }
  std::sort(partitions.begin(), partitions.end());
  partitions.erase(std::unique(partitions.begin(), partitions.end()), partitions.end());
  if (partitions.empty()) {
    ui->Print("No USB OTG block partitions detected.\n");
    return false;
  }

  // The old PE volume manager allowed selection of several external volumes.
  // Preserve that behavior without its obsolete libsgdisk/partition write APIs.
  std::vector<std::string> choices{"Cancel"};
  for (const auto& block : partitions) {
    choices.push_back("USB partition: " + block);
  }
  constexpr unsigned long kMountFlags = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC;
  for (;;) {
    const size_t selected = ui->ShowMenu(
        {"Choose USB OTG partition (read-only)"}, choices, 0, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
    if (selected == 0 || selected > partitions.size()) {
      // Covers Cancel, INTERRUPTED, TIMED_OUT and invalid menu positions.
      return false;
    }
    const std::string& block = partitions[selected - 1];
    for (const char* fs : { "vfat", "exfat", "ext4", "f2fs" }) {
      // Avoid journal replay/metadata writes when probing an ext4 USB disk.
      const char* options = (strcmp(fs, "ext4") == 0) ? "noload" : "";
      if (mount(block.c_str(), USB_OTG_ROOT, fs, kMountFlags, options) == 0) {
        ui->Print("USB OTG mounted read-only: %s (%s)\n", block.c_str(), fs);
        return true;
      }
    }
    ui->Print("Cannot mount %s read-only; select another USB partition.\n", block.c_str());
  }
}

InstallResult ApplyFromUsbOtg(Device* device) {
  RecoveryUI* ui = device->GetUI();
  if (!MountUsbOtgReadOnly(device)) return INSTALL_ERROR;
  std::string path = BrowseDirectory(USB_OTG_ROOT, device, ui, false /* ZIP only */);
  if (path.empty()) {
    ui->Print("No USB OTG package selected.\n");
    umount2(USB_OTG_ROOT, MNT_DETACH);
    return INSTALL_ERROR;
  }
  ui->Print("\n-- Install %s ...\n", path.c_str());
  SetSdcardUpdateBootloaderMessage();
  const InstallResult result = InstallWithFuseFromPath(path, device);
  if (umount2(USB_OTG_ROOT, MNT_DETACH) != 0) {
    PLOG(WARNING) << "Failed to unmount USB OTG";
  }
  return result;
}

InstallResult ApplyFromSdcard(Device* device) {
  auto ui = device->GetUI();
  if (ensure_path_mounted(SDCARD_ROOT) != 0) {
    LOG(ERROR) << "\n-- Couldn't mount " << SDCARD_ROOT << ".\n";
    return INSTALL_ERROR;
  }

  std::string path = BrowseDirectory(SDCARD_ROOT, device, ui);
  if (path.empty()) {
    LOG(ERROR) << "\n-- No package file selected.\n";
    ensure_path_unmounted(SDCARD_ROOT);
    return INSTALL_ERROR;
  }

  // Hint the install function to read from a block map file.
  if (android::base::EndsWithIgnoreCase(path, ".map")) {
    path = "@" + path;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());
  SetSdcardUpdateBootloaderMessage();

  auto result = InstallWithFuseFromPath(path, device);
  ensure_path_unmounted(SDCARD_ROOT);
  return result;
}
