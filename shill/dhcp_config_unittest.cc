// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/dhcp_config.h"

#include <base/bind.h>
#include <base/file_util.h>
#include <base/scoped_temp_dir.h>
#include <base/stringprintf.h>
#include <chromeos/dbus/service_constants.h>
#include <gtest/gtest.h>

#include "shill/dbus_adaptor.h"
#include "shill/dhcp_provider.h"
#include "shill/event_dispatcher.h"
#include "shill/mock_control.h"
#include "shill/mock_dhcp_proxy.h"
#include "shill/mock_glib.h"
#include "shill/property_store_unittest.h"
#include "shill/proxy_factory.h"

using base::Bind;
using base::Unretained;
using std::string;
using std::vector;
using testing::_;
using testing::Return;
using testing::SetArgumentPointee;
using testing::Test;

namespace shill {

namespace {
const char kDeviceName[] = "eth0";
const char kHostName[] = "hostname";
}  // namespace {}

class DHCPConfigTest : public PropertyStoreTest {
 public:
  DHCPConfigTest()
      : proxy_(new MockDHCPProxy()),
        proxy_factory_(this),
        config_(new DHCPConfig(&control_,
                               dispatcher(),
                               DHCPProvider::GetInstance(),
                               kDeviceName,
                               kHostName,
                               glib())) {}

  virtual void SetUp() {
    config_->proxy_factory_ = &proxy_factory_;
  }

  virtual void TearDown() {
    config_->proxy_factory_ = NULL;
  }

 protected:
  class TestProxyFactory : public ProxyFactory {
   public:
    explicit TestProxyFactory(DHCPConfigTest *test) : test_(test) {}

    virtual DHCPProxyInterface *CreateDHCPProxy(const string &/*service*/) {
      return test_->proxy_.release();
    }

   private:
    DHCPConfigTest *test_;
  };

  scoped_ptr<MockDHCPProxy> proxy_;
  TestProxyFactory proxy_factory_;
  MockControl control_;
  DHCPConfigRefPtr config_;
};

TEST_F(DHCPConfigTest, GetIPv4AddressString) {
  EXPECT_EQ("255.255.255.255", config_->GetIPv4AddressString(0xffffffff));
  EXPECT_EQ("0.0.0.0", config_->GetIPv4AddressString(0));
  EXPECT_EQ("1.2.3.4", config_->GetIPv4AddressString(0x04030201));
}

TEST_F(DHCPConfigTest, InitProxy) {
  static const char kService[] = ":1.200";
  EXPECT_TRUE(proxy_.get());
  EXPECT_FALSE(config_->proxy_.get());
  config_->InitProxy(kService);
  EXPECT_FALSE(proxy_.get());
  EXPECT_TRUE(config_->proxy_.get());

  config_->InitProxy(kService);
}

TEST_F(DHCPConfigTest, ParseConfiguration) {
  DHCPConfig::Configuration conf;
  conf[DHCPConfig::kConfigurationKeyIPAddress].writer().append_uint32(
      0x01020304);
  conf[DHCPConfig::kConfigurationKeySubnetCIDR].writer().append_byte(
      16);
  conf[DHCPConfig::kConfigurationKeyBroadcastAddress].writer().append_uint32(
      0x10203040);
  {
    vector<unsigned int> routers;
    routers.push_back(0x02040608);
    routers.push_back(0x03050709);
    DBus::MessageIter writer =
        conf[DHCPConfig::kConfigurationKeyRouters].writer();
    writer << routers;
  }
  {
    vector<unsigned int> dns;
    dns.push_back(0x09070503);
    dns.push_back(0x08060402);
    DBus::MessageIter writer = conf[DHCPConfig::kConfigurationKeyDNS].writer();
    writer << dns;
  }
  conf[DHCPConfig::kConfigurationKeyDomainName].writer().append_string(
      "domain-name");
  {
    vector<string> search;
    search.push_back("foo.com");
    search.push_back("bar.com");
    DBus::MessageIter writer =
        conf[DHCPConfig::kConfigurationKeyDomainSearch].writer();
    writer << search;
  }
  conf[DHCPConfig::kConfigurationKeyMTU].writer().append_uint16(600);
  conf["UnknownKey"] = DBus::Variant();

  IPConfig::Properties properties;
  ASSERT_TRUE(config_->ParseConfiguration(conf, &properties));
  EXPECT_EQ("4.3.2.1", properties.address);
  EXPECT_EQ(16, properties.subnet_prefix);
  EXPECT_EQ("64.48.32.16", properties.broadcast_address);
  EXPECT_EQ("8.6.4.2", properties.gateway);
  ASSERT_EQ(2, properties.dns_servers.size());
  EXPECT_EQ("3.5.7.9", properties.dns_servers[0]);
  EXPECT_EQ("2.4.6.8", properties.dns_servers[1]);
  EXPECT_EQ("domain-name", properties.domain_name);
  ASSERT_EQ(2, properties.domain_search.size());
  EXPECT_EQ("foo.com", properties.domain_search[0]);
  EXPECT_EQ("bar.com", properties.domain_search[1]);
  EXPECT_EQ(600, properties.mtu);
}

TEST_F(DHCPConfigTest, StartFail) {
  EXPECT_CALL(*glib(), SpawnAsync(_, _, _, _, _, _, _, _))
      .WillOnce(Return(false));
  EXPECT_CALL(*glib(), ChildWatchAdd(_, _, _)).Times(0);
  EXPECT_FALSE(config_->Start());
  EXPECT_EQ(0, config_->pid_);
}

MATCHER_P(IsDHCPCDArgs, has_hostname, "") {
  if (string(arg[0]) != "/sbin/dhcpcd" ||
      string(arg[1]) != "-B" ||
      string(arg[2]) != kDeviceName) {
    return false;
  }

  if (has_hostname) {
    if (string(arg[3]) != "-h" ||
        string(arg[4]) != kHostName ||
        arg[5] != NULL) {
      return false;
    }
  } else {
      if (arg[3] != NULL) {
        return false;
      }
  }

  return true;
}

TEST_F(DHCPConfigTest, StartWithHostname) {
  EXPECT_CALL(*glib(), SpawnAsync(_, IsDHCPCDArgs(true), _, _, _, _, _, _))
      .WillOnce(Return(false));
  EXPECT_FALSE(config_->Start());
}

TEST_F(DHCPConfigTest, StartWithoutHostname) {
  DHCPConfigRefPtr config(new DHCPConfig(&control_,
                                         dispatcher(),
                                         DHCPProvider::GetInstance(),
                                         kDeviceName,
                                         "",
                                         glib()));

  EXPECT_CALL(*glib(), SpawnAsync(_, IsDHCPCDArgs(false), _, _, _, _, _, _))
      .WillOnce(Return(false));
  EXPECT_FALSE(config->Start());
}

namespace {

class UpdateCallbackTest {
 public:
  UpdateCallbackTest(const string &message,
                     const IPConfigRefPtr &ipconfig,
                     bool success)
      : message_(message),
        ipconfig_(ipconfig),
        success_(success),
        called_(false) {}

  void Callback(const IPConfigRefPtr &ipconfig, bool success) {
    called_ = true;
    EXPECT_EQ(ipconfig_.get(), ipconfig.get()) << message_;
    EXPECT_EQ(success_, success) << message_;
  }

  bool called() const { return called_; }

 private:
  const string message_;
  IPConfigRefPtr ipconfig_;
  bool success_;
  bool called_;
};

}  // namespace {}

TEST_F(DHCPConfigTest, ProcessEventSignalFail) {
  DHCPConfig::Configuration conf;
  conf[DHCPConfig::kConfigurationKeyIPAddress].writer().append_uint32(
      0x01020304);
  UpdateCallbackTest callback_test(DHCPConfig::kReasonFail, config_, false);
  config_->RegisterUpdateCallback(
      Bind(&UpdateCallbackTest::Callback, Unretained(&callback_test)));
  config_->ProcessEventSignal(DHCPConfig::kReasonFail, conf);
  EXPECT_TRUE(callback_test.called());
  EXPECT_TRUE(config_->properties().address.empty());
}

TEST_F(DHCPConfigTest, ProcessEventSignalSuccess) {
  static const char * const kReasons[] =  {
    DHCPConfig::kReasonBound,
    DHCPConfig::kReasonRebind,
    DHCPConfig::kReasonReboot,
    DHCPConfig::kReasonRenew
  };
  for (size_t r = 0; r < arraysize(kReasons); r++) {
    DHCPConfig::Configuration conf;
    string message = string(kReasons[r]) + " failed";
    conf[DHCPConfig::kConfigurationKeyIPAddress].writer().append_uint32(r);
    UpdateCallbackTest callback_test(message, config_, true);
    config_->RegisterUpdateCallback(
        Bind(&UpdateCallbackTest::Callback, Unretained(&callback_test)));
    config_->ProcessEventSignal(kReasons[r], conf);
    EXPECT_TRUE(callback_test.called()) << message;
    EXPECT_EQ(base::StringPrintf("%zu.0.0.0", r), config_->properties().address)
        << message;
  }
}

TEST_F(DHCPConfigTest, ProcessEventSignalUnknown) {
  DHCPConfig::Configuration conf;
  conf[DHCPConfig::kConfigurationKeyIPAddress].writer().append_uint32(
      0x01020304);
  static const char kReasonUnknown[] = "UNKNOWN_REASON";
  UpdateCallbackTest callback_test(kReasonUnknown, config_, false);
  config_->RegisterUpdateCallback(
      Bind(&UpdateCallbackTest::Callback, Unretained(&callback_test)));
  config_->ProcessEventSignal(kReasonUnknown, conf);
  EXPECT_FALSE(callback_test.called());
  EXPECT_TRUE(config_->properties().address.empty());
}


TEST_F(DHCPConfigTest, ReleaseIP) {
  config_->pid_ = 1 << 18;  // Ensure unknown positive PID.
  EXPECT_CALL(*proxy_, Release(kDeviceName)).Times(1);
  config_->proxy_.reset(proxy_.release());
  EXPECT_TRUE(config_->ReleaseIP());
  config_->pid_ = 0;
}

TEST_F(DHCPConfigTest, RenewIP) {
  config_->pid_ = 456;
  EXPECT_CALL(*proxy_, Rebind(kDeviceName)).Times(1);
  config_->proxy_.reset(proxy_.release());
  EXPECT_TRUE(config_->RenewIP());
  config_->pid_ = 0;
}

TEST_F(DHCPConfigTest, RequestIP) {
  config_->pid_ = 567;
  EXPECT_CALL(*proxy_, Rebind(kDeviceName)).Times(1);
  config_->proxy_.reset(proxy_.release());
  EXPECT_TRUE(config_->RenewIP());
  config_->pid_ = 0;
}

TEST_F(DHCPConfigTest, Restart) {
  const int kPID1 = 1 << 17;  // Ensure unknown positive PID.
  const int kPID2 = 987;
  const unsigned int kTag1 = 11;
  const unsigned int kTag2 = 22;
  config_->pid_ = kPID1;
  config_->child_watch_tag_ = kTag1;
  DHCPProvider::GetInstance()->BindPID(kPID1, config_);
  EXPECT_CALL(*glib(), SourceRemove(kTag1)).WillOnce(Return(true));
  EXPECT_CALL(*glib(), SpawnClosePID(kPID1)).Times(1);
  EXPECT_CALL(*glib(), SpawnAsync(_, _, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArgumentPointee<6>(kPID2), Return(true)));
  EXPECT_CALL(*glib(), ChildWatchAdd(kPID2, _, _)).WillOnce(Return(kTag2));
  EXPECT_TRUE(config_->Restart());
  EXPECT_EQ(kPID2, config_->pid_);
  EXPECT_EQ(config_.get(), DHCPProvider::GetInstance()->GetConfig(kPID2).get());
  EXPECT_EQ(kTag2, config_->child_watch_tag_);
  DHCPProvider::GetInstance()->UnbindPID(kPID2);
  config_->pid_ = 0;
  config_->child_watch_tag_ = 0;
}

TEST_F(DHCPConfigTest, RestartNoClient) {
  const int kPID = 777;
  const unsigned int kTag = 66;
  EXPECT_CALL(*glib(), SourceRemove(_)).Times(0);
  EXPECT_CALL(*glib(), SpawnClosePID(_)).Times(0);
  EXPECT_CALL(*glib(), SpawnAsync(_, _, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArgumentPointee<6>(kPID), Return(true)));
  EXPECT_CALL(*glib(), ChildWatchAdd(kPID, _, _)).WillOnce(Return(kTag));
  EXPECT_TRUE(config_->Restart());
  EXPECT_EQ(kPID, config_->pid_);
  EXPECT_EQ(config_.get(), DHCPProvider::GetInstance()->GetConfig(kPID).get());
  EXPECT_EQ(kTag, config_->child_watch_tag_);
  DHCPProvider::GetInstance()->UnbindPID(kPID);
  config_->pid_ = 0;
  config_->child_watch_tag_ = 0;
}

TEST_F(DHCPConfigTest, StartSuccess) {
  const int kPID = 123456;
  const unsigned int kTag = 55;
  EXPECT_CALL(*glib(), SpawnAsync(_, _, _, _, _, _, _, _))
      .WillOnce(DoAll(SetArgumentPointee<6>(kPID), Return(true)));
  EXPECT_CALL(*glib(), ChildWatchAdd(kPID, _, _)).WillOnce(Return(kTag));
  EXPECT_TRUE(config_->Start());
  EXPECT_EQ(kPID, config_->pid_);
  EXPECT_EQ(config_.get(), DHCPProvider::GetInstance()->GetConfig(kPID).get());
  EXPECT_EQ(kTag, config_->child_watch_tag_);

  ScopedTempDir temp_dir;
  config_->root_ = temp_dir.path();
  FilePath varrun = temp_dir.path().Append("var/run");
  EXPECT_TRUE(file_util::CreateDirectory(varrun));
  FilePath pid_file =
      varrun.Append(base::StringPrintf("dhcpcd-%s.pid", kDeviceName));
  FilePath lease_file =
      varrun.Append(base::StringPrintf("dhcpcd-%s.lease", kDeviceName));
  EXPECT_EQ(0, file_util::WriteFile(pid_file, "", 0));
  EXPECT_EQ(0, file_util::WriteFile(lease_file, "", 0));
  ASSERT_TRUE(file_util::PathExists(pid_file));
  ASSERT_TRUE(file_util::PathExists(lease_file));

  EXPECT_CALL(*glib(), SpawnClosePID(kPID)).Times(1);
  DHCPConfig::ChildWatchCallback(kPID, 0, config_.get());
  EXPECT_EQ(NULL, DHCPProvider::GetInstance()->GetConfig(kPID).get());
  EXPECT_FALSE(file_util::PathExists(pid_file));
  EXPECT_FALSE(file_util::PathExists(lease_file));
}

TEST_F(DHCPConfigTest, Stop) {
  // Ensure no crashes.
  const int kPID = 1 << 17;  // Ensure unknown positive PID.
  config_->Stop();
  config_->pid_ = kPID;
  config_->Stop();
  EXPECT_CALL(*glib(), SpawnClosePID(kPID)).Times(1);  // Invoked by destructor.
}

TEST_F(DHCPConfigTest, SetProperty) {
  ::DBus::Error error;
  // Ensure that an attempt to write a R/O property returns InvalidArgs error.
  EXPECT_FALSE(DBusAdaptor::SetProperty(config_->mutable_store(),
                                        flimflam::kAddressProperty,
                                        PropertyStoreTest::kStringV,
                                        &error));
  EXPECT_EQ(invalid_args(), error.name());
}

}  // namespace shill
