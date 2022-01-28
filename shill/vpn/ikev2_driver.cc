// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/vpn/ikev2_driver.h"

#include <memory>
#include <string>
#include <utility>

#include <base/bind.h>
#include <base/logging.h>
#include <base/time/time.h>
#include <chromeos/dbus/shill/dbus-constants.h>

#include "shill/error.h"
#include "shill/ipconfig.h"
#include "shill/manager.h"
#include "shill/vpn/ipsec_connection.h"
#include "shill/vpn/vpn_service.h"

namespace shill {

namespace {

constexpr base::TimeDelta kConnectTimeout = base::TimeDelta::FromSeconds(30);

std::unique_ptr<IPsecConnection::Config> MakeIPsecConfig(
    const KeyValueStore& args, const EapCredentials& eap_credentials) {
  auto config = std::make_unique<IPsecConnection::Config>();

  config->ike_version = IPsecConnection::Config::IKEVersion::kV2;
  config->remote = args.Lookup<std::string>(kProviderHostProperty, "");
  config->local_id =
      args.GetOptionalValue<std::string>(kIKEv2LocalIdentityProperty);
  config->remote_id =
      args.GetOptionalValue<std::string>(kIKEv2RemoteIdentityProperty);
  config->ca_cert_pem_strings =
      args.GetOptionalValue<Strings>(kIKEv2CaCertPemProperty);

  const std::string auth_type =
      args.Lookup<std::string>(kIKEv2AuthenticationTypeProperty, "");
  if (auth_type == kIKEv2AuthenticationTypePSK) {
    config->psk = args.GetOptionalValue<std::string>(kIKEv2PskProperty);
    if (!config->psk.has_value()) {
      LOG(ERROR) << "Auth type is PSK but no PSK value found.";
      return nullptr;
    }
  } else if (auth_type == kIKEv2AuthenticationTypeCert) {
    config->client_cert_id =
        args.GetOptionalValue<std::string>(kIKEv2ClientCertIdProperty);
    config->client_cert_slot =
        args.GetOptionalValue<std::string>(kIKEv2ClientCertSlotProperty);
    if (!config->client_cert_id.has_value() ||
        !config->client_cert_slot.has_value()) {
      LOG(ERROR) << "Auth type is emtpy but empty cert id or slot found.";
      return nullptr;
    }
  } else if (auth_type == kIKEv2AuthenticationTypeEAP) {
    if (eap_credentials.method() != kEapMethodMSCHAPV2) {
      LOG(ERROR) << "Only MSCHAPv2 is supported for EAP in IKEv2 VPN.";
      return nullptr;
    }

    Error err;
    config->xauth_user = eap_credentials.identity();
    config->xauth_password = eap_credentials.GetEapPassword(&err);
    if (err.IsFailure()) {
      LOG(ERROR) << err;
      return nullptr;
    }
  } else {
    LOG(ERROR) << "Invalid auth type: " << auth_type;
    return nullptr;
  }

  return config;
}

}  // namespace

const VPNDriver::Property IKEv2Driver::kProperties[] = {
    {kIKEv2AuthenticationTypeProperty, 0},
    {kIKEv2CaCertPemProperty, Property::kArray},
    {kIKEv2ClientCertIdProperty, 0},
    {kIKEv2ClientCertSlotProperty, 0},
    {kIKEv2PskProperty, Property::kCredential | Property::kWriteOnly},
    {kIKEv2LocalIdentityProperty, Property::kCredential},
    {kIKEv2RemoteIdentityProperty, Property::kCredential},
    {kProviderHostProperty, 0},
    {kProviderTypeProperty, 0},
};

IKEv2Driver::IKEv2Driver(Manager* manager, ProcessManager* process_manager)
    : VPNDriver(manager,
                process_manager,
                kProperties,
                base::size(kProperties),
                /*use_eap=*/true) {}

IKEv2Driver::~IKEv2Driver() {}

base::TimeDelta IKEv2Driver::ConnectAsync(EventHandler* handler) {
  event_handler_ = handler;

  dispatcher()->PostTask(FROM_HERE,
                         base::BindOnce(&IKEv2Driver::StartIPsecConnection,
                                        weak_factory_.GetWeakPtr()));

  return kConnectTimeout;
}

void IKEv2Driver::StartIPsecConnection() {
  if (ipsec_connection_) {
    LOG(ERROR) << "The previous IPsecConnection is still running.";
    NotifyServiceOfFailure(Service::kFailureInternal);
    return;
  }

  auto callbacks = std::make_unique<IPsecConnection::Callbacks>(
      base::BindRepeating(&IKEv2Driver::OnIPsecConnected,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&IKEv2Driver::OnIPsecFailure, weak_factory_.GetWeakPtr()),
      base::BindOnce(&IKEv2Driver::OnIPsecStopped, weak_factory_.GetWeakPtr()));
  auto ipsec_config = MakeIPsecConfig(*const_args(), *eap_credentials());
  if (!ipsec_config) {
    LOG(ERROR) << "Failed to generate IPsec config";
    NotifyServiceOfFailure(Service::kFailureConnect);
    return;
  }

  ipsec_connection_ = CreateIPsecConnection(
      std::move(ipsec_config), std::move(callbacks), manager()->device_info(),
      manager()->dispatcher(), process_manager());

  ipsec_connection_->Connect();
}

std::unique_ptr<VPNConnection> IKEv2Driver::CreateIPsecConnection(
    std::unique_ptr<IPsecConnection::Config> config,
    std::unique_ptr<VPNConnection::Callbacks> callbacks,
    DeviceInfo* device_info,
    EventDispatcher* dispatcher,
    ProcessManager* process_manager) {
  return std::make_unique<IPsecConnection>(
      std::move(config), std::move(callbacks), /*l2tp_connection=*/nullptr,
      device_info, dispatcher, process_manager);
}

void IKEv2Driver::Disconnect() {
  event_handler_ = nullptr;
  if (!ipsec_connection_) {
    LOG(ERROR) << "Disconnect() called but IPsecConnection is not running";
    return;
  }
  if (!ipsec_connection_->IsConnectingOrConnected()) {
    LOG(ERROR) << "Disconnect() called but IPsecConnection is in "
               << ipsec_connection_->state() << " state";
    return;
  }
  ipsec_connection_->Disconnect();
}

IPConfig::Properties IKEv2Driver::GetIPProperties() const {
  return ip_properties_;
}

std::string IKEv2Driver::GetProviderType() const {
  return kProviderIKEv2;
}

void IKEv2Driver::OnConnectTimeout() {
  LOG(INFO) << "Connect timeout";
  if (!ipsec_connection_) {
    LOG(ERROR)
        << "OnConnectTimeout() called but IPsecConnection is not running";
    return;
  }
  if (!ipsec_connection_->IsConnectingOrConnected()) {
    LOG(ERROR) << "OnConnectTimeout() called but IPsecConnection is in "
               << ipsec_connection_->state() << " state";
    return;
  }
  ipsec_connection_->Disconnect();
  NotifyServiceOfFailure(Service::kFailureConnect);
}

// TODO(b/210064468): Check if charon can handle these events.
void IKEv2Driver::OnBeforeSuspend(const ResultCallback& callback) {
  if (ipsec_connection_ && ipsec_connection_->IsConnectingOrConnected()) {
    ipsec_connection_->Disconnect();
    NotifyServiceOfFailure(Service::kFailureDisconnect);
  }
  callback.Run(Error(Error::kSuccess));
}

// TODO(b/210064468): Check if charon can handle these events.
void IKEv2Driver::OnDefaultPhysicalServiceEvent(
    DefaultPhysicalServiceEvent event) {
  if (!ipsec_connection_ || !ipsec_connection_->IsConnectingOrConnected()) {
    return;
  }
  switch (event) {
    case kDefaultPhysicalServiceUp:
      return;
    case kDefaultPhysicalServiceDown:
      ipsec_connection_->Disconnect();
      NotifyServiceOfFailure(Service::kFailureDisconnect);
      return;
    case kDefaultPhysicalServiceChanged:
      ipsec_connection_->Disconnect();
      NotifyServiceOfFailure(Service::kFailureDisconnect);
      return;
    default:
      NOTREACHED();
  }
}

void IKEv2Driver::NotifyServiceOfFailure(Service::ConnectFailure failure) {
  LOG(ERROR) << "Driver failure due to "
             << Service::ConnectFailureToString(failure);
  if (event_handler_) {
    event_handler_->OnDriverFailure(failure, Service::kErrorDetailsNone);
    event_handler_ = nullptr;
  }
}

void IKEv2Driver::OnIPsecConnected(const std::string& link_name,
                                   int interface_index,
                                   const IPConfig::Properties& ip_properties) {
  if (!event_handler_) {
    LOG(ERROR) << "OnIPsecConnected() triggered in illegal service state";
    return;
  }
  ReportConnectionMetrics();
  ip_properties_ = ip_properties;
  event_handler_->OnDriverConnected(link_name, interface_index);
}

void IKEv2Driver::OnIPsecFailure(Service::ConnectFailure failure) {
  NotifyServiceOfFailure(failure);
}

void IKEv2Driver::OnIPsecStopped() {
  ipsec_connection_ = nullptr;
}

// TODO(b/210064468): Provide information on if credential fields are empty.
KeyValueStore IKEv2Driver::GetProvider(Error* error) {
  KeyValueStore props = VPNDriver::GetProvider(error);
  return props;
}

// TODO(b/210064468): Implement metrics.
void IKEv2Driver::ReportConnectionMetrics() {}

}  // namespace shill
