// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include <base/json/json_reader.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/stringprintf.h>
#include <brillo/errors/error_codes.h>
#include <build/build_config.h>
#include <build/buildflag.h>
#include <unordered_map>
#include <utility>
#include "debugd/src/error_utils.h"
#include "debugd/src/kernel_feature_tool.h"

namespace debugd {

namespace {
constexpr char kErrorPath[] = "org.chromium.debugd.KernelFeatureError";
constexpr char kKernelFeaturesPath[] = "/etc/init/kernel-features.conf";

// JSON Helper to retrieve a string value given a string key
bool GetStringFromKey(base::Value* obj,
                      const std::string& key,
                      std::string* value) {
  const std::string* val = obj->FindStringKey(key);
  if (!val || val->empty()) {
    return false;
  }

  *value = *val;
  return true;
}

}  // namespace

WriteFileCommand::WriteFileCommand(const std::string& file_name,
                                   const std::string& value)
    : FeatureCommand("WriteFile") {
  file_name_ = file_name;
  value_ = value;
}

bool WriteFileCommand::Execute() {
  if (!base::WriteFile(base::FilePath(file_name_), value_)) {
    PLOG(ERROR) << "Unable to write to " << file_name_;
    return false;
  }
  return true;
}

FileExistsCommand::FileExistsCommand(const std::string& file_name)
    : FeatureCommand("FileExists") {
  file_name_ = file_name;
}

bool FileExistsCommand::Execute() {
  return base::PathExists(base::FilePath(file_name_));
}

void KernelFeature::AddCmd(std::unique_ptr<FeatureCommand> cmd) {
  exec_cmds_.push_back(std::move(cmd));
}

void KernelFeature::AddQueryCmd(std::unique_ptr<FeatureCommand> cmd) {
  support_check_cmds_.push_back(std::move(cmd));
}

bool KernelFeature::Execute() const {
  for (auto& cmd : exec_cmds_) {
    if (!cmd->Execute()) {
      LOG(ERROR) << "Failed to execute command: " << cmd->name();
      return false;
    }
  }
  return true;
}

bool KernelFeature::IsSupported() const {
  for (auto& cmd : support_check_cmds_) {
    if (!cmd->Execute()) {
      return false;
    }
  }
  return true;
}

bool JsonFeatureParser::ParseFile(const base::FilePath& path,
                                  std::string* err_str) {
  std::string input;

  if (features_parsed_)
    return true;

  if (!ReadFileToString(path, &input)) {
    *err_str = "debugd: Failed to read kernel-features config!";
    return false;
  }

  VLOG(1) << "JSON feature parsed result: " << input;

  base::JSONReader::ValueWithError root =
      base::JSONReader::ReadAndReturnValueWithError(input);
  if (!root.value) {
    *err_str = "debugd: Failed to parse features conf file!";
    return false;
  }

  if (!root.value->is_list() || root.value->GetList().size() == 0) {
    *err_str = "debugd: features list should be non-zero size!";
    return false;
  }

  for (unsigned i = 0; i < root.value->GetList().size(); i++) {
    base::Value item = std::move(root.value->GetList()[i]);
    base::Value* feature_json_obj = &item;

    if (!feature_json_obj->is_dict()) {
      *err_str = "debugd: features conf not list of dicts!";
      return false;
    }

    KernelFeature feature_obj;
    if (!MakeFeatureObject(feature_json_obj, err_str, feature_obj)) {
      return false;
    }

    auto got = feature_map_.find(feature_obj.name());
    if (got != feature_map_.end()) {
      *err_str =
          "debugd: Duplicate feature name found! : " + feature_obj.name();
      return false;
    }

    feature_map_.insert(
        std::make_pair(feature_obj.name(), std::move(feature_obj)));
  }

  features_parsed_ = true;
  return true;
}

// KernelFeature implementation (collect and execute commands).
bool JsonFeatureParser::MakeFeatureObject(base::Value* feature_obj,
                                          std::string* err_str,
                                          KernelFeature& kern_feat) {
  std::string feat_name;
  if (!GetStringFromKey(feature_obj, "name", &feat_name)) {
    kern_feat.SetName(feat_name);
    *err_str = "debugd: features conf contains empty names";
    return false;
  }

  // Commands for querying if device is supported
  base::Value* support_cmd_list_obj =
      feature_obj->FindListKey("support_check_commands");

  if (!support_cmd_list_obj) {
    // Feature is assumed to be always supported, such as a kernel parameter
    // that is on all device kernels.
    kern_feat.AddQueryCmd(std::make_unique<AlwaysSupportedCommand>());
  } else {
    // A support check command was provided, add it to the feature object.
    if (!support_cmd_list_obj->is_list() ||
        support_cmd_list_obj->GetList().size() == 0) {
      *err_str = "debugd: Invalid format for support_check_commands commands";
      return false;
    }

    for (unsigned i = 0; i < support_cmd_list_obj->GetList().size(); i++) {
      base::Value item = std::move(support_cmd_list_obj->GetList()[i]);
      base::Value* cmd_obj = &item;
      std::string cmd_name;

      if (!GetStringFromKey(cmd_obj, "name", &cmd_name)) {
        *err_str = "debugd: Invalid/Empty command name in features config.";
        return false;
      }

      if (cmd_name == "FileExists") {
        std::string file_name;

        VLOG(1) << "debugd: command is FileExists";
        if (!GetStringFromKey(cmd_obj, "file", &file_name)) {
          *err_str = "debugd: JSON contains invalid command name";
          return false;
        }

        kern_feat.AddQueryCmd(std::make_unique<FileExistsCommand>(file_name));
      }
    }
  }

  // Commands to execute to enable feature
  base::Value* cmd_list_obj = feature_obj->FindListKey("commands");
  if (!cmd_list_obj || !cmd_list_obj->is_list() ||
      cmd_list_obj->GetList().size() == 0) {
    *err_str = "debugd: Failed to get commands list in feature.";
    return false;
  }

  for (unsigned i = 0; i < cmd_list_obj->GetList().size(); i++) {
    base::Value item = std::move(cmd_list_obj->GetList()[i]);
    base::Value* cmd_obj = &item;
    std::string cmd_name;

    if (!GetStringFromKey(cmd_obj, "name", &cmd_name)) {
      *err_str = "debugd: Invalid command in features config.";
      return false;
    }

    if (cmd_name == "WriteFile") {
      std::string file_name, value;

      VLOG(1) << "debugd: command is WriteFile";
      if (!GetStringFromKey(cmd_obj, "file", &file_name)) {
        *err_str = "debugd: JSON contains invalid command name!";
        return false;
      }

      if (!GetStringFromKey(cmd_obj, "value", &value)) {
        *err_str = "debugd: JSON contains invalid command value!";
        return false;
      }
      kern_feat.AddCmd(std::make_unique<WriteFileCommand>(file_name, value));
    }
  }

  return true;
}

KernelFeatureTool::KernelFeatureTool()
    : parser_(std::make_unique<JsonFeatureParser>()) {}

KernelFeatureTool::~KernelFeatureTool() = default;

bool KernelFeatureTool::ParseFeatureList(std::string* err_str) {
  DCHECK(err_str);

  if (!parser_->ParseFile(base::FilePath(kKernelFeaturesPath), err_str)) {
    return false;
  }

  return true;
}

bool KernelFeatureTool::GetFeatureList(std::string* csv_list,
                                       std::string* err_str) {
  DCHECK(csv_list);
  DCHECK(err_str);

  csv_list->clear();

  if (!ParseFeatureList(err_str)) {
    return false;
  }

  bool first = true;
  for (auto& it : *(parser_->GetFeatureMap())) {
    if (!first)
      csv_list->append(",");
    else
      first = false;

    csv_list->append(it.first);
  }

  return true;
}

bool KernelFeatureTool::KernelFeatureEnable(brillo::ErrorPtr* error,
                                            const std::string& name,
                                            bool* result,
                                            std::string* err_str) {
  DCHECK(error);
  DCHECK(result);
  DCHECK(err_str);

  if (!ParseFeatureList(err_str)) {
    *result = false;
    DEBUGD_ADD_ERROR(error, kErrorPath, *err_str);
    return false;
  }

  auto feature = parser_->GetFeatureMap()->find(name);
  if (feature == parser_->GetFeatureMap()->end()) {
    *err_str = "debugd: Feature not found in features config!";
    *result = false;
    DEBUGD_ADD_ERROR(error, kErrorPath, *err_str);
    return false;
  }

  auto& feature_obj = feature->second;
  if (!feature_obj.IsSupported()) {
    *err_str = "debugd: device does not support feature " + name;
    *result = false;
    DEBUGD_ADD_ERROR(error, kErrorPath, *err_str);
    return false;
  }

  if (!feature_obj.Execute()) {
    *err_str = "debugd: Tried but failed to enable feature " + name;
    *result = false;
    DEBUGD_ADD_ERROR(error, kErrorPath, *err_str);
    return false;
  }

  /* On success, return the feature name to debugd for context. */
  *err_str = name;
  *result = true;
  LOG(INFO) << "debugd: KernelFeatureEnable: Feature " << name << " enabled";
  return true;
}

bool KernelFeatureTool::KernelFeatureList(brillo::ErrorPtr* error,
                                          bool* result,
                                          std::string* out) {
  DCHECK(error);
  DCHECK(result);
  DCHECK(out);
  out->clear();

  std::string csv, err_str;
  *result = GetFeatureList(&csv, &err_str);

  // If failure, assign the output string as the error message
  if (!*result) {
    out->append("error:");
    out->append(err_str);
    DEBUGD_ADD_ERROR(error, kErrorPath, err_str);
  } else {
    LOG(INFO) << "debugd: KernelFeatureList: " << csv;
    out->append("csv:");
    out->append(csv);
  }
  return *result;
}
}  // namespace debugd
