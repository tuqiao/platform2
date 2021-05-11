// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "brillo/blkdev_utils/lvm_device.h"

// lvm2 has multiple options for managing LVM objects:
// - liblvm2app: deprecated.
// - liblvm2cmd: simple interface to directly parse cli commands into functions.
// - lvmdbusd: persistent daemon that can be reached via D-Bus.
//
// Since the logical/physical volume and volume group creation can occur during
// early boot when dbus is not available, the preferred solution is to use
// lvm2cmd.
#include <lvm2cmd.h>

#include <base/json/json_reader.h>
#include <base/posix/eintr_wrapper.h>
#include <base/strings/string_number_conversions.h>
#include <base/values.h>
#include <brillo/process/process.h>

namespace brillo {
namespace {

void LogLvmError(int rc, const std::string& cmd) {
  switch (rc) {
    case LVM2_COMMAND_SUCCEEDED:
      break;
    case LVM2_NO_SUCH_COMMAND:
      LOG(ERROR) << "Failed to run lvm2 command: no such command " << cmd;
      break;
    case LVM2_INVALID_PARAMETERS:
      LOG(ERROR) << "Failed to run lvm2 command: invalid parameters " << cmd;
      break;
    case LVM2_PROCESSING_FAILED:
      LOG(ERROR) << "Failed to run lvm2 command: processing failed " << cmd;
      break;
    default:
      LOG(ERROR) << "Failed to run lvm2 command: invalid return code " << cmd;
      break;
  }
}

}  // namespace

PhysicalVolume::PhysicalVolume(const base::FilePath& device_path,
                               std::shared_ptr<LvmCommandRunner> lvm)
    : device_path_(device_path), lvm_(lvm) {}

bool PhysicalVolume::Check() {
  if (device_path_.empty() || !lvm_)
    return false;

  return lvm_->RunCommand({"pvck", device_path_.value()});
}

bool PhysicalVolume::Repair() {
  if (device_path_.empty() || !lvm_)
    return false;

  return lvm_->RunCommand({"pvck", "--yes", device_path_.value()});
}

bool PhysicalVolume::Remove() {
  if (device_path_.empty() || !lvm_)
    return false;

  bool ret = lvm_->RunCommand({"pvremove", device_path_.value()});
  device_path_ = base::FilePath();
  return ret;
}

VolumeGroup::VolumeGroup(const std::string& volume_group_name,
                         std::shared_ptr<LvmCommandRunner> lvm)
    : volume_group_name_(volume_group_name), lvm_(lvm) {}

bool VolumeGroup::Check() {
  if (volume_group_name_.empty() || !lvm_)
    return false;

  return lvm_->RunCommand({"vgck", GetPath().value()});
}

bool VolumeGroup::Repair() {
  if (volume_group_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"vgck", "--yes", GetPath().value()});
}

base::FilePath VolumeGroup::GetPath() const {
  if (volume_group_name_.empty() || !lvm_)
    return base::FilePath();
  return base::FilePath("/dev").Append(volume_group_name_);
}

bool VolumeGroup::Activate() {
  if (volume_group_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"vgchange", "-ay", volume_group_name_});
}

bool VolumeGroup::Deactivate() {
  if (volume_group_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"vgchange", "-an", volume_group_name_});
}

bool VolumeGroup::Remove() {
  if (volume_group_name_.empty() || !lvm_)
    return false;
  bool ret = lvm_->RunCommand({"vgremove", volume_group_name_});
  volume_group_name_ = "";
  return ret;
}

LogicalVolume::LogicalVolume(const std::string& logical_volume_name,
                             const std::string& volume_group_name,
                             std::shared_ptr<LvmCommandRunner> lvm)
    : logical_volume_name_(logical_volume_name),
      volume_group_name_(volume_group_name),
      lvm_(lvm) {}

base::FilePath LogicalVolume::GetPath() {
  if (logical_volume_name_.empty() || !lvm_)
    return base::FilePath();
  return base::FilePath("/dev")
      .Append(volume_group_name_)
      .Append(logical_volume_name_);
}

bool LogicalVolume::Activate() {
  if (logical_volume_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"lvchange", "-ay", GetName()});
}

bool LogicalVolume::Deactivate() {
  if (logical_volume_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"lvchange", "-an", GetName()});
}

bool LogicalVolume::Remove() {
  if (volume_group_name_.empty() || !lvm_)
    return false;
  bool ret = lvm_->RunCommand({"lvremove", "--force", GetName()});
  logical_volume_name_ = "";
  volume_group_name_ = "";
  return ret;
}

Thinpool::Thinpool(const std::string& thinpool_name,
                   const std::string& volume_group_name,
                   std::shared_ptr<LvmCommandRunner> lvm)
    : thinpool_name_(thinpool_name),
      volume_group_name_(volume_group_name),
      lvm_(lvm) {}

bool Thinpool::Check() {
  if (thinpool_name_.empty() || !lvm_)
    return false;

  return lvm_->RunProcess({"thin_check", GetName()});
}

bool Thinpool::Repair() {
  if (thinpool_name_.empty() || !lvm_)
    return false;
  return lvm_->RunProcess({"lvconvert", "--repair", GetName()});
}

bool Thinpool::Activate() {
  if (thinpool_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"lvchange", "-ay", GetName()});
}

bool Thinpool::Deactivate() {
  if (thinpool_name_.empty() || !lvm_)
    return false;
  return lvm_->RunCommand({"lvchange", "-an", GetName()});
}

bool Thinpool::Remove() {
  if (thinpool_name_.empty() || !lvm_)
    return false;

  bool ret = lvm_->RunCommand({"lvremove", "--force", GetName()});
  volume_group_name_ = "";
  thinpool_name_ = "";
  return ret;
}

uint64_t Thinpool::GetTotalSpace() {
  if (thinpool_name_.empty() || !lvm_)
    return 0;

  std::string output;

  if (!lvm_->RunProcess(
          {"/sbin/lvdisplay", "-S", "pool_lv=\"\"", "-C", "--reportformat",
           "json", "--units", "b", volume_group_name_ + "/" + thinpool_name_},
          &output)) {
    LOG(ERROR) << "Failed to get output from lvdisplay.";
    return 0;
  }

  base::Optional<base::Value> report_contents =
      lvm_->UnwrapReportContents(output, "lv");
  base::DictionaryValue* lv_dictionary;

  if (!report_contents || !report_contents->GetAsDictionary(&lv_dictionary)) {
    LOG(ERROR) << "Failed to get report contents.";
    return 0;
  }

  // Get the thinpool size.
  std::string thinpool_size;
  if (!lv_dictionary->GetString("lv_size", &thinpool_size)) {
    LOG(ERROR) << "Failed to get thinpool size.";
    return 0;
  }

  // Use base::StringToUint64 to validate the returned thinpool size.
  // Last character for size is always "B".
  thinpool_size.pop_back();
  uint64_t size;

  if (!base::StringToUint64(thinpool_size, &size)) {
    LOG(ERROR) << "Failed to convert thinpool size to a numeric value";
    return 0;
  }

  return size;
}

uint64_t Thinpool::GetFreeSpace() {
  if (thinpool_name_.empty() || !lvm_)
    return 0;

  std::string output;

  if (!lvm_->RunProcess(
          {"/sbin/lvdisplay", "-S", "pool_lv=\"\"", "-C", "--reportformat",
           "json", "--units", "b", volume_group_name_ + "/" + thinpool_name_},
          &output)) {
    LOG(ERROR) << "Failed to get output from lvdisplay.";
    return 0;
  }

  base::Optional<base::Value> report_contents =
      lvm_->UnwrapReportContents(output, "lv");
  base::DictionaryValue* lv_dictionary;

  if (!report_contents || !report_contents->GetAsDictionary(&lv_dictionary)) {
    LOG(ERROR) << "Failed to get report contents.";
    return 0;
  }

  // Get the percentage of used data from the thinpool. The value is stored as a
  // string in the json.
  std::string data_used_percent;
  if (!lv_dictionary->GetString("data_percent", &data_used_percent)) {
    LOG(ERROR) << "Failed to get percentage size of thinpool used.";
    return 0;
  }

  double used_percent;
  if (!base::StringToDouble(data_used_percent, &used_percent)) {
    LOG(ERROR) << "Failed to convert used percentage string to double.";
    return 0;
  }

  return static_cast<uint64_t>((100.0 - used_percent) / 100.0 *
                               GetTotalSpace());
}

LvmCommandRunner::LvmCommandRunner() {}

LvmCommandRunner::~LvmCommandRunner() {}

bool LvmCommandRunner::RunCommand(const std::vector<std::string>& cmd) {
  // lvm2_run() does not exec/fork a separate process, instead it parses the
  // command line and calls the relevant functions within liblvm2cmd directly.
  std::string lvm_cmd = base::JoinString(cmd, " ");
  int rc = lvm2_run(nullptr, lvm_cmd.c_str());
  LogLvmError(rc, lvm_cmd);

  return rc == LVM2_COMMAND_SUCCEEDED;
}

bool LvmCommandRunner::RunProcess(const std::vector<std::string>& cmd,
                                  std::string* output) {
  brillo::ProcessImpl lvm_process;
  for (auto arg : cmd)
    lvm_process.AddArg(arg);
  lvm_process.SetCloseUnusedFileDescriptors(true);

  if (output) {
    lvm_process.RedirectUsingMemory(STDOUT_FILENO);
  }

  if (lvm_process.Run() != 0) {
    PLOG(ERROR) << "Failed to run command";
    return false;
  }

  if (output) {
    *output = lvm_process.GetOutputString(STDOUT_FILENO);
  }

  return true;
}

// LVM reports are structured as:
//  {
//      "report": [
//          {
//              "lv": [
//                  {"lv_name":"foo", "vg_name":"bar", ...},
//                  {...}
//              ]
//          }
//      ]
//  }
//
// Common function to fetch the underlying dictionary (assume for now
// that the reports will be reporting just a single type (lv/vg/pv) for now).

base::Optional<base::Value> LvmCommandRunner::UnwrapReportContents(
    const std::string& output, const std::string& key) {
  auto report = base::JSONReader::Read(output);
  base::DictionaryValue* dictionary_report;
  if (!report || !report->is_dict() ||
      !report->GetAsDictionary(&dictionary_report)) {
    LOG(ERROR) << "Failed to get report as dictionary";
    return base::nullopt;
  }

  base::ListValue* report_list;
  if (!dictionary_report->GetList("report", &report_list)) {
    LOG(ERROR) << "Failed to find 'report' list";
    return base::nullopt;
  }

  if (report_list->GetSize() != 1) {
    LOG(ERROR) << "Unexpected size: " << report_list->GetSize();
    return base::nullopt;
  }

  base::DictionaryValue* report_dictionary;
  if (!report_list->GetDictionary(0, &report_dictionary)) {
    LOG(ERROR) << "Failed to find 'report' dictionary";
    return base::nullopt;
  }

  base::ListValue* key_list;
  if (!report_dictionary->GetList(key, &key_list)) {
    LOG(ERROR) << "Failed to find " << key << " list";
    return base::nullopt;
  }

  // If the list has just a single dictionary element, return it directly.
  if (key_list && key_list->GetSize() == 1) {
    base::DictionaryValue* key_dictionary;
    if (!key_list->GetDictionary(0, &key_dictionary)) {
      LOG(ERROR) << "Failed to get " << key << " dictionary";
      return base::nullopt;
    }
    return key_dictionary->Clone();
  }

  return key_list->Clone();
}

}  // namespace brillo
