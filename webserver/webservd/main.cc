// Copyright 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <signal.h>
#include <sysexits.h>

#include <string>

#include <base/command_line.h>
#include <base/files/file_util.h>
#ifdef __ANDROID__
#include <binderwrapper/binder_wrapper.h>
#include <brillo/binder_watcher.h>
#include <brillo/daemons/daemon.h>
#else
#include <brillo/dbus/async_event_sequencer.h>
#include <brillo/dbus/exported_object_manager.h>
#include <brillo/daemons/dbus_daemon.h>
#endif  // __ANDROID__
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>

#ifdef __ANDROID__
#include "webservd/binder_server.h"
#include "webserv_common/binder_constants.h"
#else
#include "webservd/server.h"
#endif
#include "webservd/config.h"
#include "webservd/log_manager.h"
#include "webservd/utils.h"

#if defined(__ANDROID__)
#include <firewalld/firewall.h>
#else
#include "webservd/permission_broker_firewall.h"
using FirewallImpl = webservd::PermissionBrokerFirewall;
#endif  // defined(__ANDROID__)

#ifdef __ANDROID__
using BaseDaemon = brillo::Daemon;
#else
using brillo::dbus_utils::AsyncEventSequencer;
using BaseDaemon = brillo::DBusServiceDaemon;
#endif  // __ANDROID__

namespace {

const char kDefaultConfigFilePath[] = "/etc/webservd/config";

#ifndef __ANDROID__
const char kServiceName[] = "org.chromium.WebServer";
const char kRootServicePath[] = "/org/chromium/WebServer";
#endif  // !defined(__ANDROID__)

class Daemon final : public BaseDaemon {
 public:
#ifdef __ANDROID__
  explicit Daemon(webservd::Config config)
      : config_{std::move(config)} {}
#else
  explicit Daemon(webservd::Config config)
      : DBusServiceDaemon{kServiceName, kRootServicePath},
        config_{std::move(config)} {}
#endif  // __ANDROID__

 protected:
#ifndef __ANDROID__
  void RegisterDBusObjectsAsync(AsyncEventSequencer* sequencer) override {
    webservd::LogManager::Init(base::FilePath{config_.log_directory});
    server_.reset(new webservd::Server{
        object_manager_.get(), config_,
        std::unique_ptr<webservd::FirewallInterface>{new FirewallImpl()}});
    server_->RegisterAsync(
        sequencer->GetHandler("Server.RegisterAsync() failed.", true));
  }

  void OnShutdown(int* /* return_code */) override {
    server_.reset();
  }
#endif  // !__ANDROID__

 private:
#ifdef __ANDROID__
  int OnInit() override {
    int result = brillo::Daemon::OnInit();
    if (result != EX_OK) {
      return result;
    }

    webservd::LogManager::Init(base::FilePath{config_.log_directory});

    android::BinderWrapper::Create();
    if (!binder_watcher_.Init()) {
        return EX_OSERR;
    }

    server_.reset(new webservd::BinderServer(config_,
                  android::BinderWrapper::Get()));

    if (!android::BinderWrapper::Get()->RegisterService(
            webservd::kWebserverBinderServiceName,
            server_.get())) {
      return EX_OSERR;
    }


    return EX_OK;
  }
#endif  // __ANDROID__

  webservd::Config config_;
#ifdef __ANDROID__
  std::unique_ptr<webservd::BinderServer> server_;
  brillo::BinderWatcher binder_watcher_;
#else
  std::unique_ptr<webservd::Server> server_;
#endif  // __ANDROID__

  DISALLOW_COPY_AND_ASSIGN(Daemon);
};

}  // namespace

int main(int argc, char* argv[]) {
  DEFINE_bool(log_to_stderr, false, "log trace messages to stderr as well");
  DEFINE_string(config_path, "",
                "path to a file containing server configuration");
  DEFINE_bool(debug, false,
              "return debug error information in web requests");
  DEFINE_bool(ipv6, true, "enable IPv6 support");
  brillo::FlagHelper::Init(argc, argv, "Brillo web server daemon");

  // From libmicrohttpd documentation, section 1.5 SIGPIPE:
  // ... portable code using MHD must install a SIGPIPE handler or explicitly
  // block the SIGPIPE signal.
  // This also applies to using pipes over D-Bus to pass request/response data
  // to/from remote request handlers. We handle errors from write operations on
  // sockets/pipes correctly, so SIGPIPE is just a pest.
  signal(SIGPIPE, SIG_IGN);

  int flags = brillo::kLogToSyslog;
  if (FLAGS_log_to_stderr)
    flags |= brillo::kLogToStderr;
  brillo::InitLog(flags | brillo::kLogHeader);

  webservd::Config config;
  config.use_ipv6 = FLAGS_ipv6;
  base::FilePath default_file_path{kDefaultConfigFilePath};
  if (!FLAGS_config_path.empty()) {
    // In tests, we'll override the board specific and default configurations
    // with a test specific configuration.
    webservd::LoadConfigFromFile(base::FilePath{FLAGS_config_path}, &config);
  } else if (base::PathExists(default_file_path)) {
    // Some boards have a configuration they will want to use to override
    // our defaults.  Part of our interface is to look for this in a
    // standard location.
    CHECK(webservd::LoadConfigFromFile(default_file_path, &config));
  } else {
    webservd::LoadDefaultConfig(&config);
  }

  // For protocol handlers bound to specific network interfaces, we need root
  // access to create those bound sockets.
  for (auto& handler_config : config.protocol_handlers) {
    if (!handler_config.interface_name.empty()) {
      int socket_fd =
          webservd::CreateNetworkInterfaceSocket(handler_config.interface_name);
      if (socket_fd < 0) {
        LOG(ERROR) << "Failed to create a socket for network interface "
                   << handler_config.interface_name;
        return EX_SOFTWARE;
      }
      handler_config.socket_fd = socket_fd;
    }
  }

  config.use_debug = FLAGS_debug;
  Daemon daemon{std::move(config)};

  return daemon.Run();
}
