// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "arc/keymint/conversion.h"

#include <utility>

namespace arc::keymint {

namespace {

keymaster_key_format_t ConvertEnum(arc::mojom::keymint::KeyFormat key_format) {
  return static_cast<keymaster_key_format_t>(key_format);
}

keymaster_purpose_t ConvertEnum(arc::mojom::keymint::KeyPurpose key_purpose) {
  return static_cast<keymaster_purpose_t>(key_purpose);
}

keymaster_tag_t ConvertEnum(arc::mojom::keymint::Tag tag) {
  return static_cast<keymaster_tag_t>(tag);
}

class KmParamSet {
 public:
  explicit KmParamSet(
      const std::vector<arc::mojom::keymint::KeyParameterPtr>& data) {
    param_set_.params = new keymaster_key_param_t[data.size()];
    param_set_.length = data.size();
    for (size_t i = 0; i < data.size(); ++i) {
      keymaster_tag_t tag = ConvertEnum(data[i]->tag);
      switch (keymaster_tag_get_type(tag)) {
        case KM_ENUM:
        case KM_ENUM_REP:
          if (data[i]->value->is_integer()) {
            param_set_.params[i] =
                keymaster_param_enum(tag, data[i]->value->get_integer());
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_UINT:
        case KM_UINT_REP:
          if (data[i]->value->is_integer()) {
            param_set_.params[i] =
                keymaster_param_int(tag, data[i]->value->get_integer());
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_ULONG:
        case KM_ULONG_REP:
          if (data[i]->value->is_long_integer()) {
            param_set_.params[i] =
                keymaster_param_long(tag, data[i]->value->get_long_integer());
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_DATE:
          if (data[i]->value->is_date_time()) {
            param_set_.params[i] =
                keymaster_param_date(tag, data[i]->value->get_date_time());
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_BOOL:
          if (data[i]->value->is_bool_value() &&
              data[i]->value->get_bool_value()) {
            // This function takes a single argument. Default value is TRUE.
            param_set_.params[i] = keymaster_param_bool(tag);
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_BIGNUM:
        case KM_BYTES:
          if (data[i]->value->is_blob()) {
            param_set_.params[i] =
                keymaster_param_blob(tag, data[i]->value->get_blob().data(),
                                     data[i]->value->get_blob().size());
          } else {
            param_set_.params[i].tag = KM_TAG_INVALID;
          }
          break;
        case KM_INVALID:
        default:
          param_set_.params[i].tag = KM_TAG_INVALID;
          // just skip
          break;
      }
    }
  }

  KmParamSet(KmParamSet&& other)
      : param_set_{other.param_set_.params, other.param_set_.length} {
    other.param_set_.length = 0;
    other.param_set_.params = nullptr;
  }
  KmParamSet(const KmParamSet&) = delete;
  KmParamSet& operator=(const KmParamSet&) = delete;

  ~KmParamSet() { delete[] param_set_.params; }

  inline const keymaster_key_param_set_t& param_set() const {
    return param_set_;
  }

 private:
  keymaster_key_param_set_t param_set_;
};

}  // namespace

std::vector<uint8_t> ConvertFromKeymasterMessage(const uint8_t* data,
                                                 const size_t size) {
  return std::vector<uint8_t>(data, data + size);
}

std::vector<std::vector<uint8_t>> ConvertFromKeymasterMessage(
    const keymaster_cert_chain_t& cert) {
  std::vector<std::vector<uint8_t>> out(cert.entry_count);
  for (size_t i = 0; i < cert.entry_count; ++i) {
    const auto& entry = cert.entries[i];
    out[i] = ConvertFromKeymasterMessage(entry.data, entry.data_length);
  }
  return out;
}

std::vector<arc::mojom::keymint::KeyParameterPtr> ConvertFromKeymasterMessage(
    const keymaster_key_param_set_t& param_set) {
  if (param_set.length == 0 || !param_set.params) {
    return std::vector<arc::mojom::keymint::KeyParameterPtr>();
  }

  std::vector<arc::mojom::keymint::KeyParameterPtr> out(param_set.length);
  const keymaster_key_param_t* params = param_set.params;

  for (size_t i = 0; i < param_set.length; ++i) {
    keymaster_tag_t tag = params[i].tag;
    arc::mojom::keymint::KeyParameterValuePtr param;
    switch (keymaster_tag_get_type(tag)) {
      case KM_ENUM:
      case KM_ENUM_REP:
        param = arc::mojom::keymint::KeyParameterValue::NewInteger(
            params[i].enumerated);
        break;
      case KM_UINT:
      case KM_UINT_REP:
        param = arc::mojom::keymint::KeyParameterValue::NewInteger(
            params[i].integer);
        break;
      case KM_ULONG:
      case KM_ULONG_REP:
        param = arc::mojom::keymint::KeyParameterValue::NewLongInteger(
            params[i].long_integer);
        break;
      case KM_DATE:
        param = arc::mojom::keymint::KeyParameterValue::NewDateTime(
            params[i].date_time);
        break;
      case KM_BOOL:
        param = arc::mojom::keymint::KeyParameterValue::NewBoolValue(
            params[i].boolean);
        break;
      case KM_BIGNUM:
      case KM_BYTES:
        param = arc::mojom::keymint::KeyParameterValue::NewBlob(
            ConvertFromKeymasterMessage(params[i].blob.data,
                                        params[i].blob.data_length));
        break;
      case KM_INVALID:
        tag = KM_TAG_INVALID;
        // just skip
        break;
    }

    out[i] = arc::mojom::keymint::KeyParameter::New(
        static_cast<arc::mojom::keymint::Tag>(tag), std::move(param));
  }

  return out;
}

void ConvertToKeymasterMessage(const std::vector<uint8_t>& data,
                               ::keymaster::Buffer* out) {
  out->Reinitialize(data.data(), data.size());
}

void ConvertToKeymasterMessage(const std::vector<uint8_t>& clientId,
                               const std::vector<uint8_t>& appData,
                               ::keymaster::AuthorizationSet* params) {
  params->Clear();
  if (!clientId.empty()) {
    params->push_back(::keymaster::TAG_APPLICATION_ID, clientId.data(),
                      clientId.size());
  }
  if (!appData.empty()) {
    params->push_back(::keymaster::TAG_APPLICATION_DATA, appData.data(),
                      appData.size());
  }
}

void ConvertToKeymasterMessage(
    const std::vector<arc::mojom::keymint::KeyParameterPtr>& data,
    ::keymaster::AuthorizationSet* out) {
  KmParamSet param_set(data);
  out->Reinitialize(param_set.param_set());
}

}  // namespace arc::keymint