// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/service.h"

#include <time.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include <base/memory/scoped_ptr.h>
#include <base/string_number_conversions.h>
#include <base/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/connection.h"
#include "shill/control_interface.h"
#include "shill/diagnostics_reporter.h"
#include "shill/error.h"
#include "shill/http_proxy.h"
#include "shill/logging.h"
#include "shill/manager.h"
#include "shill/metrics.h"
#include "shill/profile.h"
#include "shill/property_accessor.h"
#include "shill/refptr_types.h"
#include "shill/service_dbus_adaptor.h"
#include "shill/sockets.h"
#include "shill/store_interface.h"

using base::Bind;
using std::deque;
using std::map;
using std::string;
using std::vector;

namespace shill {

const char Service::kAutoConnBusy[] = "busy";
const char Service::kAutoConnConnected[] = "connected";
const char Service::kAutoConnConnecting[] = "connecting";
const char Service::kAutoConnExplicitDisconnect[] = "explicitly disconnected";
const char Service::kAutoConnNotConnectable[] = "not connectable";
const char Service::kAutoConnOffline[] = "offline";
const char Service::kAutoConnThrottled[] = "throttled";

const size_t Service::kEAPMaxCertificationElements = 10;

const char Service::kCheckPortalAuto[] = "auto";
const char Service::kCheckPortalFalse[] = "false";
const char Service::kCheckPortalTrue[] = "true";

const int Service::kPriorityNone = 0;

const char Service::kServiceSortAutoConnect[] = "AutoConnect";
const char Service::kServiceSortConnectable[] = "Connectable";
const char Service::kServiceSortDependency[] = "Dependency";
const char Service::kServiceSortFavorite[] = "Favorite";
const char Service::kServiceSortIsConnected[] = "IsConnected";
const char Service::kServiceSortIsConnecting[] = "IsConnecting";
const char Service::kServiceSortIsFailed[] = "IsFailed";
const char Service::kServiceSortIsPortalled[] = "IsPortal";
const char Service::kServiceSortPriority[] = "Priority";
const char Service::kServiceSortSecurityEtc[] = "SecurityEtc";
const char Service::kServiceSortTechnology[] = "Technology";
const char Service::kServiceSortUniqueName[] = "UniqueName";

const char Service::kStorageAutoConnect[] = "AutoConnect";
const char Service::kStorageCheckPortal[] = "CheckPortal";
const char Service::kStorageEapAnonymousIdentity[] = "EAP.AnonymousIdentity";
const char Service::kStorageEapCACert[] = "EAP.CACert";
const char Service::kStorageEapCACertID[] = "EAP.CACertID";
const char Service::kStorageEapCACertNSS[] = "EAP.CACertNSS";
const char Service::kStorageEapCertID[] = "EAP.CertID";
const char Service::kStorageEapClientCert[] = "EAP.ClientCert";
const char Service::kStorageEapEap[] = "EAP.EAP";
const char Service::kStorageEapIdentity[] = "EAP.Identity";
const char Service::kStorageEapInnerEap[] = "EAP.InnerEAP";
const char Service::kStorageEapKeyID[] = "EAP.KeyID";
const char Service::kStorageEapKeyManagement[] = "EAP.KeyMgmt";
const char Service::kStorageEapPIN[] = "EAP.PIN";
const char Service::kStorageEapPassword[] = "EAP.Password";
const char Service::kStorageEapPrivateKey[] = "EAP.PrivateKey";
const char Service::kStorageEapPrivateKeyPassword[] = "EAP.PrivateKeyPassword";
const char Service::kStorageEapUseSystemCAs[] = "EAP.UseSystemCAs";
const char Service::kStorageError[] = "Error";
const char Service::kStorageFavorite[] = "Favorite";
const char Service::kStorageGUID[] = "GUID";
const char Service::kStorageHasEverConnected[] = "HasEverConnected";
const char Service::kStorageName[] = "Name";
const char Service::kStoragePriority[] = "Priority";
const char Service::kStorageProxyConfig[] = "ProxyConfig";
const char Service::kStorageSaveCredentials[] = "SaveCredentials";
const char Service::kStorageType[] = "Type";
const char Service::kStorageUIData[] = "UIData";

const uint8 Service::kStrengthMax = 100;
const uint8 Service::kStrengthMin = 0;

const uint64 Service::kMaxAutoConnectCooldownTimeMilliseconds = 30 * 60 * 1000;
const uint64 Service::kMinAutoConnectCooldownTimeMilliseconds = 1000;
const uint64 Service::kAutoConnectCooldownBackoffFactor = 2;

const int Service::kDisconnectsMonitorSeconds = 5 * 60;
const int Service::kMisconnectsMonitorSeconds = 5 * 60;
const int Service::kReportDisconnectsThreshold = 2;
const int Service::kReportMisconnectsThreshold = 3;
const int Service::kMaxDisconnectEventHistory = 20;

// static
unsigned int Service::serial_number_ = 0;

Service::Service(ControlInterface *control_interface,
                 EventDispatcher *dispatcher,
                 Metrics *metrics,
                 Manager *manager,
                 Technology::Identifier technology)
    : state_(kStateIdle),
      previous_state_(kStateIdle),
      failure_(kFailureUnknown),
      auto_connect_(false),
      check_portal_(kCheckPortalAuto),
      connectable_(false),
      error_(ConnectFailureToString(failure_)),
      explicitly_disconnected_(false),
      favorite_(false),
      priority_(kPriorityNone),
      security_level_(0),
      strength_(0),
      save_credentials_(true),
      technology_(technology),
      failed_time_(0),
      has_ever_connected_(false),
      auto_connect_cooldown_milliseconds_(0),
      dispatcher_(dispatcher),
      unique_name_(base::UintToString(serial_number_++)),
      friendly_name_(unique_name_),
      adaptor_(control_interface->CreateServiceAdaptor(this)),
      metrics_(metrics),
      manager_(manager),
      sockets_(new Sockets()),
      weak_ptr_factory_(this),
      time_(Time::GetInstance()),
      diagnostics_reporter_(DiagnosticsReporter::GetInstance()) {
  HelpRegisterDerivedBool(flimflam::kAutoConnectProperty,
                          &Service::GetAutoConnect,
                          &Service::SetAutoConnect);

  // flimflam::kActivationStateProperty: Registered in CellularService
  // flimflam::kCellularApnProperty: Registered in CellularService
  // flimflam::kCellularLastGoodApnProperty: Registered in CellularService
  // flimflam::kNetworkTechnologyProperty: Registered in CellularService
  // flimflam::kOperatorNameProperty: DEPRECATED
  // flimflam::kOperatorCodeProperty: DEPRECATED
  // flimflam::kRoamingStateProperty: Registered in CellularService
  // flimflam::kServingOperatorProperty: Registered in CellularService
  // flimflam::kPaymentURLProperty: Registered in CellularService

  HelpRegisterDerivedString(flimflam::kCheckPortalProperty,
                            &Service::GetCheckPortal,
                            &Service::SetCheckPortal);
  store_.RegisterConstBool(flimflam::kConnectableProperty, &connectable_);
  HelpRegisterDerivedRpcIdentifier(flimflam::kDeviceProperty,
                                   &Service::GetDeviceRpcId,
                                   NULL);
  store_.RegisterString(flimflam::kGuidProperty, &guid_);

  store_.RegisterString(flimflam::kEapIdentityProperty, &eap_.identity);
  store_.RegisterString(flimflam::kEAPEAPProperty, &eap_.eap);
  store_.RegisterString(flimflam::kEapPhase2AuthProperty, &eap_.inner_eap);
  store_.RegisterString(flimflam::kEapAnonymousIdentityProperty,
                        &eap_.anonymous_identity);
  store_.RegisterString(flimflam::kEAPClientCertProperty, &eap_.client_cert);
  store_.RegisterString(flimflam::kEAPCertIDProperty, &eap_.cert_id);
  store_.RegisterString(flimflam::kEapPrivateKeyProperty, &eap_.private_key);
  HelpRegisterWriteOnlyDerivedString(flimflam::kEapPrivateKeyPasswordProperty,
                                     &Service::SetEAPPrivateKeyPassword,
                                     NULL,
                                     &eap_.private_key_password);
  store_.RegisterString(flimflam::kEAPKeyIDProperty, &eap_.key_id);
  store_.RegisterString(flimflam::kEapCaCertProperty, &eap_.ca_cert);
  store_.RegisterString(flimflam::kEapCaCertIDProperty, &eap_.ca_cert_id);
  store_.RegisterString(flimflam::kEapCaCertNssProperty, &eap_.ca_cert_nss);
  store_.RegisterString(flimflam::kEAPPINProperty, &eap_.pin);
  HelpRegisterWriteOnlyDerivedString(flimflam::kEapPasswordProperty,
                                     &Service::SetEAPPassword,
                                     NULL,
                                     &eap_.password);
  store_.RegisterString(flimflam::kEapKeyMgmtProperty, &eap_.key_management);
  store_.RegisterBool(flimflam::kEapUseSystemCAsProperty, &eap_.use_system_cas);
  store_.RegisterConstStrings(shill::kEapRemoteCertificationProperty,
                              &eap_.remote_certification);
  store_.RegisterString(shill::kEapSubjectMatchProperty,
                        &eap_.subject_match);

  // TODO(ers): in flimflam clearing Error has the side-effect of
  // setting the service state to IDLE. Is this important? I could
  // see an autotest depending on it.
  store_.RegisterConstString(flimflam::kErrorProperty, &error_);
  store_.RegisterConstBool(flimflam::kFavoriteProperty, &favorite_);
  HelpRegisterDerivedUint16(shill::kHTTPProxyPortProperty,
                            &Service::GetHTTPProxyPort,
                            NULL);
  HelpRegisterDerivedRpcIdentifier(shill::kIPConfigProperty,
                                   &Service::GetIPConfigRpcIdentifier,
                                   NULL);
  HelpRegisterDerivedBool(flimflam::kIsActiveProperty,
                          &Service::IsActive,
                          NULL);
  // flimflam::kModeProperty: Registered in WiFiService

  // Although this is a read-only property, some callers want to blindly
  // set this value to its current value.
  HelpRegisterDerivedString(flimflam::kNameProperty,
                            &Service::GetNameProperty,
                            &Service::AssertTrivialSetNameProperty);
  // flimflam::kPassphraseProperty: Registered in WiFiService
  // flimflam::kPassphraseRequiredProperty: Registered in WiFiService
  store_.RegisterInt32(flimflam::kPriorityProperty, &priority_);
  HelpRegisterDerivedString(flimflam::kProfileProperty,
                            &Service::GetProfileRpcId,
                            &Service::SetProfileRpcId);
  HelpRegisterDerivedString(flimflam::kProxyConfigProperty,
                            &Service::GetProxyConfig,
                            &Service::SetProxyConfig);
  store_.RegisterBool(flimflam::kSaveCredentialsProperty, &save_credentials_);
  HelpRegisterDerivedString(flimflam::kTypeProperty,
                            &Service::CalculateTechnology,
                            NULL);
  // flimflam::kSecurityProperty: Registered in WiFiService
  HelpRegisterDerivedString(flimflam::kStateProperty,
                            &Service::CalculateState,
                            NULL);
  store_.RegisterConstUint8(flimflam::kSignalStrengthProperty, &strength_);
  store_.RegisterString(flimflam::kUIDataProperty, &ui_data_);
  HelpRegisterConstDerivedStrings(shill::kDiagnosticsDisconnectsProperty,
                                  &Service::GetDisconnectsProperty);
  HelpRegisterConstDerivedStrings(shill::kDiagnosticsMisconnectsProperty,
                                  &Service::GetMisconnectsProperty);
  metrics_->RegisterService(this);

  static_ip_parameters_.PlumbPropertyStore(&store_);

  IgnoreParameterForConfigure(flimflam::kTypeProperty);
  IgnoreParameterForConfigure(flimflam::kProfileProperty);

  LOG(INFO) << Technology::NameFromIdentifier(technology) << " service "
            << unique_name_ << " constructed.";
}

Service::~Service() {
  LOG(INFO) << "Service " << unique_name_ << " destroyed.";
  metrics_->DeregisterService(this);
}

void Service::AutoConnect() {
  const char *reason = NULL;
  if (IsAutoConnectable(&reason)) {
    LOG(INFO) << "Auto-connecting to service " << unique_name_;
    ThrottleFutureAutoConnects();
    Error error;
    Connect(&error);
  } else {
    if (reason == kAutoConnConnected || reason == kAutoConnBusy) {
      SLOG(Service, 1)
          << "Suppressed autoconnect to service " << unique_name_ << " "
          << "(" << reason << ")";
    } else {
      LOG(INFO) << "Suppressed autoconnect to service " << unique_name_ << " "
                << "(" << reason << ")";
    }
  }
}

void Service::Connect(Error */*error*/) {
  explicitly_disconnected_ = false;
  // clear any failure state from a previous connect attempt
  SetState(kStateIdle);
}

void Service::Disconnect(Error */*error*/) {
  MemoryLog::GetInstance()->FlushToDisk();
}

void Service::DisconnectWithFailure(ConnectFailure failure, Error *error) {
  Disconnect(error);
  SetFailure(failure);
}

void Service::UserInitiatedDisconnect(Error *error) {
  Disconnect(error);
  explicitly_disconnected_ = true;
}

void Service::ActivateCellularModem(const string &/*carrier*/,
                                    Error *error,
                                    const ResultCallback &/*callback*/) {
  Error::PopulateAndLog(error, Error::kNotSupported,
                        "Service doesn't support cellular modem activation.");
}

bool Service::IsActive(Error */*error*/) {
  return state() != kStateUnknown &&
    state() != kStateIdle &&
    state() != kStateFailure;
}

// static
bool Service::IsConnectedState(ConnectState state) {
  return (state == kStateConnected ||
          state == kStatePortal ||
          state == kStateOnline);
}

// static
bool Service::IsConnectingState(ConnectState state) {
  return (state == kStateAssociating ||
          state == kStateConfiguring);
}

bool Service::IsConnected() const {
  return IsConnectedState(state());
}

bool Service::IsConnecting() const {
  return IsConnectingState(state());
}

void Service::SetState(ConnectState state) {
  if (state == state_) {
    return;
  }

  LOG(INFO) << "Service " << unique_name_ << ": state "
            << ConnectStateToString(state_) << " -> "
            << ConnectStateToString(state);

  if (state == kStateFailure) {
    NoteDisconnectEvent();
  }

  previous_state_ = state_;
  state_ = state;
  if (state != kStateFailure) {
    failure_ = kFailureUnknown;
  }
  if (state == kStateConnected) {
    failed_time_ = 0;
    has_ever_connected_ = true;
    SaveToProfile();
    // When we succeed in connecting, forget that connects failed in the past.
    // Give services one chance at a fast autoconnect retry by resetting the
    // cooldown to 0 to indicate that the last connect was successful.
    auto_connect_cooldown_milliseconds_  = 0;
    reenable_auto_connect_task_.Cancel();
  }
  UpdateErrorProperty();
  manager_->UpdateService(this);
  metrics_->NotifyServiceStateChanged(this, state);
  adaptor_->EmitStringChanged(flimflam::kStateProperty, GetStateString());
}

void Service::ReEnableAutoConnectTask() {
  // Kill the thing blocking AutoConnect().
  reenable_auto_connect_task_.Cancel();
  // Post to the manager, giving it an opportunity to AutoConnect again.
  manager_->UpdateService(this);
}

void Service::ThrottleFutureAutoConnects() {
  if (auto_connect_cooldown_milliseconds_ > 0) {
    LOG(INFO) << "Throttling autoconnect to service " << unique_name_ << " for "
              << auto_connect_cooldown_milliseconds_ << " milliseconds.";
    reenable_auto_connect_task_.Reset(Bind(&Service::ReEnableAutoConnectTask,
                                           weak_ptr_factory_.GetWeakPtr()));
    dispatcher_->PostDelayedTask(reenable_auto_connect_task_.callback(),
                                 auto_connect_cooldown_milliseconds_);
  }
  auto_connect_cooldown_milliseconds_ =
      std::min(kMaxAutoConnectCooldownTimeMilliseconds,
               std::max(kMinAutoConnectCooldownTimeMilliseconds,
                        auto_connect_cooldown_milliseconds_ *
                        kAutoConnectCooldownBackoffFactor));
}

void Service::SetFailure(ConnectFailure failure) {
  failure_ = failure;
  failed_time_ = time(NULL);
  UpdateErrorProperty();
  SetState(kStateFailure);
}

void Service::SetFailureSilent(ConnectFailure failure) {
  NoteDisconnectEvent();
  // Note that order matters here, since SetState modifies |failure_| and
  // |failed_time_|.
  SetState(kStateIdle);
  failure_ = failure;
  UpdateErrorProperty();
  failed_time_ = time(NULL);
}

string Service::GetRpcIdentifier() const {
  return adaptor_->GetRpcIdentifier();
}

bool Service::IsLoadableFrom(StoreInterface *storage) const {
  return storage->ContainsGroup(GetStorageIdentifier());
}

bool Service::Load(StoreInterface *storage) {
  const string id = GetStorageIdentifier();
  if (!storage->ContainsGroup(id)) {
    LOG(WARNING) << "Service is not available in the persistent store: " << id;
    return false;
  }
  storage->GetBool(id, kStorageAutoConnect, &auto_connect_);
  storage->GetString(id, kStorageCheckPortal, &check_portal_);
  storage->GetBool(id, kStorageFavorite, &favorite_);
  storage->GetString(id, kStorageGUID, &guid_);
  storage->GetBool(id, kStorageHasEverConnected, &has_ever_connected_);
  storage->GetInt(id, kStoragePriority, &priority_);
  storage->GetString(id, kStorageProxyConfig, &proxy_config_);
  storage->GetBool(id, kStorageSaveCredentials, &save_credentials_);
  storage->GetString(id, kStorageUIData, &ui_data_);
  LoadEapCredentials(storage, id);
  static_ip_parameters_.Load(storage, id);

  explicitly_disconnected_ = false;
  favorite_ = true;

  return true;
}

bool Service::Unload() {
  auto_connect_ = IsAutoConnectByDefault();
  check_portal_ = kCheckPortalAuto;
  explicitly_disconnected_ = false;
  favorite_ = false;
  guid_ = "";
  has_ever_connected_ = false;
  priority_ = kPriorityNone;
  proxy_config_ = "";
  save_credentials_ = true;
  ui_data_ = "";

  UnloadEapCredentials();
  Error error;  // Ignored.
  Disconnect(&error);
  return false;
}

bool Service::Save(StoreInterface *storage) {
  const string id = GetStorageIdentifier();

  storage->SetString(id, kStorageType, GetTechnologyString());

  storage->SetBool(id, kStorageAutoConnect, auto_connect_);
  if (check_portal_ == kCheckPortalAuto) {
    storage->DeleteKey(id, kStorageCheckPortal);
  } else {
    storage->SetString(id, kStorageCheckPortal, check_portal_);
  }
  storage->SetBool(id, kStorageFavorite, favorite_);
  SaveString(storage, id, kStorageGUID, guid_, false, true);
  storage->SetBool(id, kStorageHasEverConnected, has_ever_connected_);
  storage->SetString(id, kStorageName, friendly_name_);
  if (priority_ != kPriorityNone) {
    storage->SetInt(id, kStoragePriority, priority_);
  } else {
    storage->DeleteKey(id, kStoragePriority);
  }
  SaveString(storage, id, kStorageProxyConfig, proxy_config_, false, true);
  storage->SetBool(id, kStorageSaveCredentials, save_credentials_);
  SaveString(storage, id, kStorageUIData, ui_data_, false, true);

  SaveEapCredentials(storage, id);
  static_ip_parameters_.Save(storage, id);
  return true;
}

void Service::SaveToCurrentProfile() {
  // Some unittests do not specify a manager.
  if (manager()) {
    manager()->SaveServiceToProfile(this);
  }
}

void Service::Configure(const KeyValueStore &args, Error *error) {
  map<string, bool>::const_iterator bool_it;
  SLOG(Service, 5) << "Configuring bool properties:";
  for (bool_it = args.bool_properties().begin();
       bool_it != args.bool_properties().end();
       ++bool_it) {
    if (ContainsKey(parameters_ignored_for_configure_, bool_it->first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << bool_it->first;
    Error set_error;
    store_.SetBoolProperty(bool_it->first, bool_it->second, &set_error);
    OnPropertyChanged(bool_it->first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string properties:";
  map<string, string>::const_iterator string_it;
  for (string_it = args.string_properties().begin();
       string_it != args.string_properties().end();
       ++string_it) {
    if (ContainsKey(parameters_ignored_for_configure_, string_it->first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << string_it->first;
    Error set_error;
    store_.SetStringProperty(string_it->first, string_it->second, &set_error);
    OnPropertyChanged(string_it->first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring int32 properties:";
  map<string, int32>::const_iterator int_it;
  for (int_it = args.int_properties().begin();
       int_it != args.int_properties().end();
       ++int_it) {
    if (ContainsKey(parameters_ignored_for_configure_, int_it->first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << int_it->first;
    Error set_error;
    store_.SetInt32Property(int_it->first, int_it->second, &set_error);
    OnPropertyChanged(int_it->first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
}

bool Service::DoPropertiesMatch(const KeyValueStore &args) const {
  map<string, bool>::const_iterator bool_it;
  SLOG(Service, 5) << "Checking bool properties:";
  for (bool_it = args.bool_properties().begin();
       bool_it != args.bool_properties().end();
       ++bool_it) {
    SLOG(Service, 5) << "   " << bool_it->first;
    Error get_error;
    bool value;
    if (!store_.GetBoolProperty(bool_it->first, &value, &get_error) ||
        value != bool_it->second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking string properties:";
  map<string, string>::const_iterator string_it;
  for (string_it = args.string_properties().begin();
       string_it != args.string_properties().end();
       ++string_it) {
    SLOG(Service, 5) << "   " << string_it->first;
    Error get_error;
    string value;
    if (!store_.GetStringProperty(string_it->first, &value, &get_error) ||
        value != string_it->second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking int32 properties:";
  map<string, int32>::const_iterator int_it;
  for (int_it = args.int_properties().begin();
       int_it != args.int_properties().end();
       ++int_it) {
    SLOG(Service, 5) << "   " << int_it->first;
    Error get_error;
    int32 value;
    if (!store_.GetInt32Property(int_it->first, &value, &get_error) ||
        value != int_it->second) {
      return false;
    }
  }
  return true;
}

bool Service::IsRemembered() const {
  return profile_ && !manager_->IsServiceEphemeral(this);
}

bool Service::IsDependentOn(const ServiceRefPtr &b) const {
  if (!connection_ || !b) {
    return false;
  }
  return connection_->GetLowerConnection() == b->connection();
}

void Service::MakeFavorite() {
  if (favorite_) {
    // We do not want to clobber the value of auto_connect_ (it may
    // be user-set). So return early.
    return;
  }

  auto_connect_ = true;
  favorite_ = true;
}

void Service::SetConnection(const ConnectionRefPtr &connection) {
  if (connection.get()) {
    // TODO(pstew): Make this function testable by using a factory here.
    // http://crosbug.com/34528
    http_proxy_.reset(new HTTPProxy(connection));
    http_proxy_->Start(dispatcher_, sockets_.get());
  } else {
    http_proxy_.reset();
    static_ip_parameters_.ClearSavedParameters();
  }
  connection_ = connection;
  Error error;
  string ipconfig = GetIPConfigRpcIdentifier(&error);
  if (error.IsSuccess()) {
    adaptor_->EmitRpcIdentifierChanged(shill::kIPConfigProperty, ipconfig);
  }
}

bool Service::Is8021xConnectable() const {
  // We mirror all the flimflam checks (see service.c:is_connectable()).

  // Identity is required.
  if (eap_.identity.empty()) {
    SLOG(Service, 2) << "Not connectable: Identity is empty.";
    return false;
  }

  if (!eap_.client_cert.empty() || !eap_.cert_id.empty()) {
    // If a client certificate is being used, we must have a private key.
    if (eap_.private_key.empty() && eap_.key_id.empty()) {
      SLOG(Service, 2)
          << "Not connectable. Client certificate but no private key.";
      return false;
    }
  }
  if (!eap_.cert_id.empty() || !eap_.key_id.empty() ||
      !eap_.ca_cert_id.empty()) {
    // If PKCS#11 data is needed, a PIN is required.
    if (eap_.pin.empty()) {
      SLOG(Service, 2) << "Not connectable. PKCS#11 data but no PIN.";
      return false;
    }
  }

  // For EAP-TLS, a client certificate is required.
  if (eap_.eap.empty() || eap_.eap == "TLS") {
    if ((!eap_.client_cert.empty() || !eap_.cert_id.empty()) &&
        (!eap_.private_key.empty() || !eap_.key_id.empty())) {
      SLOG(Service, 2) << "Connectable. EAP-TLS with a client cert and key.";
      return true;
    }
  }

  // For EAP types other than TLS (e.g. EAP-TTLS or EAP-PEAP, password is the
  // minimum requirement), at least an identity + password is required.
  if (eap_.eap.empty() || eap_.eap != "TLS") {
    if (!eap_.password.empty()) {
      SLOG(Service, 2) << "Connectable. !EAP-TLS and has a password.";
      return true;
    }
  }

  SLOG(Service, 2)
      << "Not connectable. No suitable EAP configuration was found.";
  return false;
}

bool Service::AddEAPCertification(const string &name, size_t depth) {
  if (depth >= kEAPMaxCertificationElements) {
    LOG(WARNING) << "Ignoring certification " << name
                 << " because depth " << depth
                 << " exceeds our maximum of "
                 << kEAPMaxCertificationElements;
    return false;
  }

  if (depth >= eap_.remote_certification.size()) {
    eap_.remote_certification.resize(depth + 1);
  } else if (name == eap_.remote_certification[depth]) {
    return true;
  }

  eap_.remote_certification[depth] = name;
  LOG(INFO) << "Received certification for "
            << name
            << " at depth "
            << depth;
  return true;
}

void Service::ClearEAPCertification() {
  eap_.remote_certification.clear();
}

void Service::set_eap(const EapCredentials &eap) {
  eap_ = eap;
  // Note: Connectability can only be updated by a subclass of Service
  // with knowledge of whether the service actually uses 802.1x credentials.
}

// static
const char *Service::ConnectFailureToString(const ConnectFailure &state) {
  switch (state) {
    case kFailureUnknown:
      return "Unknown";
    case kFailureAAA:
      return flimflam::kErrorAaaFailed;
    case kFailureActivation:
      return flimflam::kErrorActivationFailed;
    case kFailureBadPassphrase:
      return flimflam::kErrorBadPassphrase;
    case kFailureBadWEPKey:
      return flimflam::kErrorBadWEPKey;
    case kFailureConnect:
      return flimflam::kErrorConnectFailed;
    case kFailureDNSLookup:
      return flimflam::kErrorDNSLookupFailed;
    case kFailureDHCP:
      return flimflam::kErrorDhcpFailed;
    case kFailureHTTPGet:
      return flimflam::kErrorHTTPGetFailed;
    case kFailureNeedEVDO:
      return flimflam::kErrorNeedEvdo;
    case kFailureNeedHomeNetwork:
      return flimflam::kErrorNeedHomeNetwork;
    case kFailureOTASP:
      return flimflam::kErrorOtaspFailed;
    case kFailureOutOfRange:
      return flimflam::kErrorOutOfRange;
    case kFailurePinMissing:
      return flimflam::kErrorPinMissing;
    case kFailurePPPAuth:
      return flimflam::kErrorPppAuthFailed;
    case kFailureEAPAuthentication:
      return shill::kErrorEapAuthenticationFailed;
    case kFailureEAPLocalTLS:
      return shill::kErrorEapLocalTlsFailed;
    case kFailureEAPRemoteTLS:
      return shill::kErrorEapRemoteTlsFailed;
    case kFailureMax:
      return "Max failure error code";
  }
  return "Invalid";
}

// static
const char *Service::ConnectStateToString(const ConnectState &state) {
  switch (state) {
    case kStateUnknown:
      return "Unknown";
    case kStateIdle:
      return "Idle";
    case kStateAssociating:
      return "Associating";
    case kStateConfiguring:
      return "Configuring";
    case kStateConnected:
      return "Connected";
    case kStateDisconnected:
      return "Disconnected";
    case kStatePortal:
      return "Portal";
    case kStateFailure:
      return "Failure";
    case kStateOnline:
      return "Online";
  }
  return "Invalid";
}

string Service::GetTechnologyString() const {
  return Technology::NameFromIdentifier(technology());
}

string Service::CalculateTechnology(Error */*error*/) {
  return GetTechnologyString();
}

// static
void Service::ExpireEventsBefore(
  int seconds_ago, const Timestamp &now, std::deque<Timestamp> *events) {
  struct timeval period = (const struct timeval){ seconds_ago };
  while (!events->empty()) {
    if (events->size() < static_cast<size_t>(kMaxDisconnectEventHistory)) {
      struct timeval elapsed = (const struct timeval){ 0 };
      timersub(&now.monotonic, &events->front().monotonic, &elapsed);
      if (timercmp(&elapsed, &period, <)) {
        break;
      }
    }
    events->pop_front();
  }
}

void Service::NoteDisconnectEvent() {
  SLOG(Service, 2) << __func__;
  // Ignore the event if it's user-initiated explicit disconnect.
  if (explicitly_disconnected_) {
    SLOG(Service, 2) << "Explicit disconnect ignored.";
    return;
  }
  // Ignore the event if manager is not running (e.g., service disconnects on
  // shutdown).
  if (!manager_->running()) {
    SLOG(Service, 2) << "Disconnect while manager stopped ignored.";
    return;
  }
  // Ignore the event if the power state is not on (e.g., when suspending).
  PowerManager *power_manager = manager_->power_manager();
  if (!power_manager ||
      (power_manager->power_state() != PowerManager::kOn &&
       power_manager->power_state() != PowerManager::kUnknown)) {
    SLOG(Service, 2) << "Disconnect in transitional power state ignored.";
    return;
  }
  int period = 0;
  size_t threshold = 0;
  deque<Timestamp> *events = NULL;
  // Sometimes services transition to Idle before going into a failed state so
  // take into account the last non-idle state.
  ConnectState state = state_ == kStateIdle ? previous_state_ : state_;
  if (IsConnectedState(state)) {
    LOG(INFO) << "Noting an unexpected connection drop.";
    period = kDisconnectsMonitorSeconds;
    threshold = kReportDisconnectsThreshold;
    events = &disconnects_;
  } else if (IsConnectingState(state)) {
    LOG(INFO) << "Noting an unexpected failure to connect.";
    period = kMisconnectsMonitorSeconds;
    threshold = kReportMisconnectsThreshold;
    events = &misconnects_;
  } else {
    SLOG(Service, 2)
        << "Not connected or connecting, state transition ignored.";
    return;
  }
  Timestamp now = time_->GetNow();
  // Discard old events first.
  ExpireEventsBefore(period, now, events);
  events->push_back(now);
  if (events->size() >= threshold) {
    diagnostics_reporter_->OnConnectivityEvent();
  }
}

bool Service::HasRecentConnectionIssues() {
  Timestamp now = time_->GetNow();
  ExpireEventsBefore(kDisconnectsMonitorSeconds, now, &disconnects_);
  ExpireEventsBefore(kMisconnectsMonitorSeconds, now, &misconnects_);
  return !disconnects_.empty() || !misconnects_.empty();
}

// static
bool Service::DecideBetween(int a, int b, bool *decision) {
  if (a == b)
    return false;
  *decision = (a > b);
  return true;
}

// static
bool Service::Compare(ServiceRefPtr a,
                      ServiceRefPtr b,
                      const vector<Technology::Identifier> &tech_order,
                      const char **reason) {
  bool ret;

  if (a->state() != b->state()) {
    if (DecideBetween(a->IsConnected(), b->IsConnected(), &ret)) {
      *reason = kServiceSortIsConnected;
      return ret;
    }

    if (DecideBetween(!a->IsPortalled(), !b->IsPortalled(), &ret)) {
      *reason = kServiceSortIsPortalled;
      return ret;
    }

    if (DecideBetween(a->IsConnecting(), b->IsConnecting(), &ret)) {
      *reason = kServiceSortIsConnecting;
      return ret;
    }

    if (DecideBetween(!a->IsFailed(), !b->IsFailed(), &ret)) {
      *reason = kServiceSortIsFailed;
      return ret;
    }
  }

  if (DecideBetween(a->connectable(), b->connectable(), &ret)) {
    *reason = kServiceSortConnectable;
    return ret;
  }

  if (DecideBetween(a->IsDependentOn(b), b->IsDependentOn(a), &ret)) {
    *reason = kServiceSortDependency;
    return ret;
  }

  // Ignore the auto-connect property if both services are connected
  // already. This allows connected non-autoconnectable VPN services to be
  // sorted higher than other connected services based on technology order.
  if (!a->IsConnected() &&
      DecideBetween(a->auto_connect(), b->auto_connect(), &ret)) {
    *reason = kServiceSortAutoConnect;
    return ret;
  }

  if (DecideBetween(a->favorite(), b->favorite(), &ret)) {
    *reason = kServiceSortFavorite;
    return ret;
  }

  if (DecideBetween(a->priority(), b->priority(), &ret)) {
    *reason = kServiceSortPriority;
    return ret;
  }

  // TODO(pstew): Below this point we are making value judgements on
  // services that are not related to anything intrinsic or
  // user-specified.  These heuristics should be richer (contain
  // historical information, for example) and be subject to user
  // customization.
  for (vector<Technology::Identifier>::const_iterator it = tech_order.begin();
       it != tech_order.end();
       ++it) {
    if (DecideBetween(a->technology() == *it, b->technology() == *it, &ret)) {
      *reason = kServiceSortTechnology;
      return ret;
    }
  }

  if (DecideBetween(a->security_level(), b->security_level(), &ret) ||
      DecideBetween(a->strength(), b->strength(), &ret)) {
    *reason = kServiceSortSecurityEtc;
    return ret;
  }

  *reason = kServiceSortUniqueName;
  return a->unique_name() < b->unique_name();
}

const ProfileRefPtr &Service::profile() const { return profile_; }

void Service::set_profile(const ProfileRefPtr &p) { profile_ = p; }

void Service::SetProfile(const ProfileRefPtr &p) {
  SLOG(Service, 2) << "SetProfile from "
                   << (profile_ ? profile_->GetFriendlyName() : "")
                   << " to " << (p ? p->GetFriendlyName() : "");
  profile_ = p;
  Error error;
  string profile_rpc_id = GetProfileRpcId(&error);
  if (!error.IsSuccess()) {
    return;
  }
  adaptor_->EmitStringChanged(flimflam::kProfileProperty, profile_rpc_id);
}

void Service::OnPropertyChanged(const string &property) {
  if (Is8021x() &&
      (property == flimflam::kEAPCertIDProperty ||
       property == flimflam::kEAPClientCertProperty ||
       property == flimflam::kEAPKeyIDProperty ||
       property == flimflam::kEAPPINProperty ||
       property == flimflam::kEapCaCertIDProperty ||
       property == flimflam::kEapIdentityProperty ||
       property == flimflam::kEapKeyMgmtProperty ||
       property == flimflam::kEapPasswordProperty ||
       property == flimflam::kEapPrivateKeyProperty)) {
    // This notifies subclassess that EAP parameters have been changed.
    set_eap(eap_);
  }
  SaveToProfile();
  if ((property == flimflam::kCheckPortalProperty ||
       property == flimflam::kProxyConfigProperty) &&
      (state_ == kStateConnected ||
       state_ == kStatePortal ||
       state_ == kStateOnline)) {
    manager_->RecheckPortalOnService(this);
  }
}

void Service::OnAfterResume() {
  // Forget old autoconnect failures across suspend/resume.
  auto_connect_cooldown_milliseconds_  = 0;
  reenable_auto_connect_task_.Cancel();
  // Forget if the user disconnected us, we might be able to connect now.
  explicitly_disconnected_ = false;
}

string Service::GetIPConfigRpcIdentifier(Error *error) {
  if (!connection_) {
    error->Populate(Error::kNotFound);
    return "/";
  }

  string id = connection_->ipconfig_rpc_identifier();

  if (id.empty()) {
    // Do not return an empty IPConfig.
    error->Populate(Error::kNotFound);
    return "/";
  }

  return id;
}

void Service::set_connectable(bool connectable) {
  connectable_ = connectable;
  adaptor_->EmitBoolChanged(flimflam::kConnectableProperty, connectable_);
}

void Service::SetConnectable(bool connectable) {
  if (connectable_ == connectable) {
    return;
  }
  connectable_ = connectable;
  adaptor_->EmitBoolChanged(flimflam::kConnectableProperty, connectable_);
  if (manager_->HasService(this)) {
    manager_->UpdateService(this);
  }
}

string Service::GetStateString() const {
  switch (state_) {
    case kStateIdle:
      return flimflam::kStateIdle;
    case kStateAssociating:
      return flimflam::kStateAssociation;
    case kStateConfiguring:
      return flimflam::kStateConfiguration;
    case kStateConnected:
      return flimflam::kStateReady;
    case kStateDisconnected:
      return flimflam::kStateDisconnect;
    case kStateFailure:
      return flimflam::kStateFailure;
    case kStatePortal:
      return flimflam::kStatePortal;
    case kStateOnline:
      return flimflam::kStateOnline;
    case kStateUnknown:
    default:
      return "";
  }
}

string Service::CalculateState(Error */*error*/) {
  return GetStateString();
}

bool Service::IsAutoConnectable(const char **reason) const {
  if (!connectable()) {
    *reason = kAutoConnNotConnectable;
    return false;
  }

  if (IsConnected()) {
    *reason = kAutoConnConnected;
    return false;
  }

  if (IsConnecting()) {
    *reason = kAutoConnConnecting;
    return false;
  }

  if (explicitly_disconnected_) {
    *reason = kAutoConnExplicitDisconnect;
    return false;
  }

  if (!reenable_auto_connect_task_.IsCancelled()) {
    *reason = kAutoConnThrottled;
    return false;
  }

  if (!Technology::IsPrimaryConnectivityTechnology(technology_) &&
      !manager_->IsOnline()) {
    *reason = kAutoConnOffline;
    return false;
  }

  return true;
}

bool Service::IsPortalDetectionDisabled() const {
  return check_portal_ == kCheckPortalFalse;
}

bool Service::IsPortalDetectionAuto() const {
  return check_portal_ == kCheckPortalAuto;
}

void Service::HelpRegisterDerivedBool(
    const string &name,
    bool(Service::*get)(Error *),
    void(Service::*set)(const bool&, Error *)) {
  store_.RegisterDerivedBool(
      name,
      BoolAccessor(new CustomAccessor<Service, bool>(this, get, set)));
}

void Service::HelpRegisterDerivedString(
    const string &name,
    string(Service::*get)(Error *),
    void(Service::*set)(const string&, Error *)) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(new CustomAccessor<Service, string>(this, get, set)));
}

void Service::HelpRegisterDerivedRpcIdentifier(
    const string &name,
    RpcIdentifier(Service::*get)(Error *),
    void(Service::*set)(const RpcIdentifier&, Error *)) {
  store_.RegisterDerivedRpcIdentifier(
      name,
      RpcIdentifierAccessor(new CustomAccessor<Service, RpcIdentifier>(
          this, get, set)));
}

void Service::HelpRegisterDerivedUint16(
    const string &name,
    uint16(Service::*get)(Error *),
    void(Service::*set)(const uint16&, Error *)) {
  store_.RegisterDerivedUint16(
      name,
      Uint16Accessor(new CustomAccessor<Service, uint16>(this, get, set)));
}

void Service::HelpRegisterConstDerivedStrings(
    const string &name, Strings(Service::*get)(Error *error)) {
  store_.RegisterDerivedStrings(
      name,
      StringsAccessor(new CustomAccessor<Service, Strings>(this, get, NULL)));
}

void Service::HelpRegisterWriteOnlyDerivedString(
    const string &name,
    void(Service::*set)(const string &, Error *),
    void(Service::*clear)(Error *),
    const string *default_value) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(
          new CustomWriteOnlyAccessor<Service, string>(
              this, set, clear, default_value)));
}

void Service::SaveString(StoreInterface *storage,
                         const string &id,
                         const string &key,
                         const string &value,
                         bool crypted,
                         bool save) {
  if (value.empty() || !save) {
    storage->DeleteKey(id, key);
    return;
  }
  if (crypted) {
    storage->SetCryptedString(id, key, value);
    return;
  }
  storage->SetString(id, key, value);
}

void Service::LoadEapCredentials(StoreInterface *storage, const string &id) {
  EapCredentials eap;
  storage->GetCryptedString(id, kStorageEapIdentity, &eap.identity);
  storage->GetString(id, kStorageEapEap, &eap.eap);
  storage->GetString(id, kStorageEapInnerEap, &eap.inner_eap);
  storage->GetCryptedString(id,
                            kStorageEapAnonymousIdentity,
                            &eap.anonymous_identity);
  storage->GetString(id, kStorageEapClientCert, &eap.client_cert);
  storage->GetString(id, kStorageEapCertID, &eap.cert_id);
  storage->GetString(id, kStorageEapPrivateKey, &eap.private_key);
  storage->GetCryptedString(id,
                            kStorageEapPrivateKeyPassword,
                            &eap.private_key_password);
  storage->GetString(id, kStorageEapKeyID, &eap.key_id);
  storage->GetString(id, kStorageEapCACert, &eap.ca_cert);
  storage->GetString(id, kStorageEapCACertID, &eap.ca_cert_id);
  storage->GetString(id, kStorageEapCACertNSS, &eap.ca_cert_nss);
  storage->GetBool(id, kStorageEapUseSystemCAs, &eap.use_system_cas);
  storage->GetString(id, kStorageEapPIN, &eap.pin);
  storage->GetCryptedString(id, kStorageEapPassword, &eap.password);
  storage->GetString(id, kStorageEapKeyManagement, &eap.key_management);
  set_eap(eap);
}

void Service::SaveEapCredentials(StoreInterface *storage, const string &id) {
  bool save = save_credentials_;
  SaveString(storage, id, kStorageEapIdentity, eap_.identity, true, save);
  SaveString(storage, id, kStorageEapEap, eap_.eap, false, true);
  SaveString(storage, id, kStorageEapInnerEap, eap_.inner_eap, false, true);
  SaveString(storage,
             id,
             kStorageEapAnonymousIdentity,
             eap_.anonymous_identity,
             true,
             save);
  SaveString(storage, id, kStorageEapClientCert, eap_.client_cert, false, save);
  SaveString(storage, id, kStorageEapCertID, eap_.cert_id, false, save);
  SaveString(storage, id, kStorageEapPrivateKey, eap_.private_key, false, save);
  SaveString(storage,
             id,
             kStorageEapPrivateKeyPassword,
             eap_.private_key_password,
             true,
             save);
  SaveString(storage, id, kStorageEapKeyID, eap_.key_id, false, save);
  SaveString(storage, id, kStorageEapCACert, eap_.ca_cert, false, true);
  SaveString(storage, id, kStorageEapCACertID, eap_.ca_cert_id, false, true);
  SaveString(storage, id, kStorageEapCACertNSS, eap_.ca_cert_nss, false, true);
  storage->SetBool(id, kStorageEapUseSystemCAs, eap_.use_system_cas);
  SaveString(storage, id, kStorageEapPIN, eap_.pin, false, save);
  SaveString(storage, id, kStorageEapPassword, eap_.password, true, save);
  SaveString(storage,
             id,
             kStorageEapKeyManagement,
             eap_.key_management,
             false,
             true);
}

void Service::UnloadEapCredentials() {
  eap_.identity = "";
  eap_.eap = "";
  eap_.inner_eap = "";
  eap_.anonymous_identity = "";
  eap_.client_cert = "";
  eap_.cert_id = "";
  eap_.private_key = "";
  eap_.private_key_password = "";
  eap_.key_id = "";
  eap_.ca_cert = "";
  eap_.ca_cert_id = "";
  eap_.use_system_cas = true;
  eap_.pin = "";
  eap_.password = "";
}

void Service::IgnoreParameterForConfigure(const string &parameter) {
  parameters_ignored_for_configure_.insert(parameter);
}

const string &Service::GetEAPKeyManagement() const {
  return eap_.key_management;
}

void Service::SetEAPKeyManagement(const string &key_management) {
  eap_.key_management = key_management;
}

bool Service::GetAutoConnect(Error */*error*/) {
  return auto_connect();
}

void Service::SetAutoConnect(const bool &connect, Error *error) {
  LOG(INFO) << __func__ << "(" << connect << ")";
  set_auto_connect(connect);
}

string Service::GetCheckPortal(Error *error) {
  return check_portal_;
}

void Service::SetCheckPortal(const string &check_portal, Error *error) {
  if (check_portal == check_portal_) {
    return;
  }
  if (check_portal != kCheckPortalFalse &&
      check_portal != kCheckPortalTrue &&
      check_portal != kCheckPortalAuto) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Invalid Service CheckPortal property value: %s",
                              check_portal.c_str()));
    return;
  }
  check_portal_ = check_portal;
}

void Service::SetEAPPassword(const string &password, Error */*error*/) {
  eap_.password = password;
}

void Service::SetEAPPrivateKeyPassword(const string &password,
                                       Error */*error*/) {
  eap_.private_key_password = password;
}

string Service::GetNameProperty(Error *error) {
  return friendly_name_;
}

void Service::AssertTrivialSetNameProperty(const string &name, Error *error) {
  if (name != friendly_name_) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Service %s Name property cannot be modified.",
                              unique_name_.c_str()));
  }
}

string Service::GetProfileRpcId(Error *error) {
  if (!profile_) {
    // This happens in some unit tests where profile_ is not set.
    error->Populate(Error::kNotFound);
    return "";
  }
  return profile_->GetRpcIdentifier();
}

void Service::SetProfileRpcId(const string &profile, Error *error) {
  manager_->SetProfileForService(this, profile, error);
}

uint16 Service::GetHTTPProxyPort(Error */*error*/) {
  if (http_proxy_.get()) {
    return static_cast<uint16>(http_proxy_->proxy_port());
  }
  return 0;
}

string Service::GetProxyConfig(Error *error) {
  return proxy_config_;
}

void Service::SetProxyConfig(const string &proxy_config, Error *error) {
  proxy_config_ = proxy_config;
  adaptor_->EmitStringChanged(flimflam::kProxyConfigProperty, proxy_config_);
}

// static
Strings Service::ExtractWallClockToStrings(
    const deque<Timestamp> &timestamps) {
  Strings strings;
  for (deque<Timestamp>::const_iterator it = timestamps.begin();
       it != timestamps.end(); ++it) {
    strings.push_back(it->wall_clock);
  }
  return strings;
}

Strings Service::GetDisconnectsProperty(Error */*error*/) {
  return ExtractWallClockToStrings(disconnects_);
}

Strings Service::GetMisconnectsProperty(Error */*error*/) {
  return ExtractWallClockToStrings(misconnects_);
}

void Service::SaveToProfile() {
  if (profile_.get() && profile_->GetConstStorage()) {
    profile_->UpdateService(this);
  }
}

void Service::SetStrength(uint8 strength) {
  if (strength == strength_) {
    return;
  }
  strength_ = strength;
  adaptor_->EmitUint8Changed(flimflam::kSignalStrengthProperty, strength);
}

void Service::UpdateErrorProperty() {
  const string error(ConnectFailureToString(failure_));
  if (error == error_) {
    return;
  }
  error_ = error;
  adaptor_->EmitStringChanged(flimflam::kErrorProperty, error);
}

}  // namespace shill
