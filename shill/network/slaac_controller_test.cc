// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/network/slaac_controller.h"

#include <net-base/byte_utils.h>
#include <net-base/ip_address.h>
#include <net-base/ipv4_address.h>
#include <net-base/ipv6_address.h>

#include "shill/net/mock_rtnl_handler.h"
#include "shill/network/mock_network.h"
#include "shill/network/mock_proc_fs_stub.h"
#include "shill/test_event_dispatcher.h"

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SetArgPointee;

namespace shill {
namespace {

constexpr int kTestIfindex = 123;
constexpr char kTestIfname[] = "eth_test";
constexpr auto kTestTechnology = Technology::kUnknown;

constexpr net_base::IPv4Address kTestIPAddress0(192, 168, 1, 1);
const net_base::IPv6Address kTestIPAddress1 =
    *net_base::IPv6Address::CreateFromString("fe80::1aa9:5ff:abcd:1234");
const net_base::IPv6Address kTestIPAddress2 =
    *net_base::IPv6Address::CreateFromString("fe80::1aa9:5ff:abcd:1235");
const net_base::IPv6Address kTestIPAddress3 =
    *net_base::IPv6Address::CreateFromString("fe80::1aa9:5ff:abcd:1236");
const net_base::IPv6Address kTestIPAddress4 =
    *net_base::IPv6Address::CreateFromString("fe80::1aa9:5ff:abcd:1237");
const net_base::IPv6Address kTestIPAddress7 =
    *net_base::IPv6Address::CreateFromString("fe80::1aa9:5ff:abcd:1238");
}  // namespace

class SLAACControllerTest : public testing::Test {
 public:
  SLAACControllerTest()
      : slaac_controller_(
            kTestIfindex, &proc_fs_, &rtnl_handler_, &dispatcher_),
        proc_fs_(kTestIfname),
        network_(kTestIfindex, kTestIfname, kTestTechnology) {}
  ~SLAACControllerTest() override = default;

  void SetUp() override {
    slaac_controller_.RegisterCallback(base::BindRepeating(
        &SLAACControllerTest::UpdateCallback, base::Unretained(this)));
  }

  void SendRTNLMessage(const RTNLMessage& message);
  std::unique_ptr<RTNLMessage> BuildRdnssMessage(
      RTNLMessage::Mode mode,
      uint32_t lifetime,
      const std::vector<net_base::IPv6Address>& dns_servers);
  std::unique_ptr<RTNLMessage> BuildAddressMessage(
      RTNLMessage::Mode mode,
      const net_base::IPCIDR& address,
      unsigned char flags,
      unsigned char scope);

  MOCK_METHOD(void, UpdateCallback, (SLAACController::UpdateType));

  SLAACController slaac_controller_;
  MockProcFsStub proc_fs_;
  MockRTNLHandler rtnl_handler_;
  MockNetwork network_;
  EventDispatcherForTest dispatcher_;
};

void SLAACControllerTest::SendRTNLMessage(const RTNLMessage& message) {
  if (message.type() == RTNLMessage::kTypeAddress) {
    slaac_controller_.AddressMsgHandler(message);
  } else if (message.type() == RTNLMessage::kTypeRdnss) {
    slaac_controller_.RDNSSMsgHandler(message);
  } else {
    NOTREACHED();
  }
}

std::unique_ptr<RTNLMessage> SLAACControllerTest::BuildRdnssMessage(
    RTNLMessage::Mode mode,
    uint32_t lifetime,
    const std::vector<net_base::IPv6Address>& dns_servers) {
  auto message = std::make_unique<RTNLMessage>(RTNLMessage::kTypeRdnss, mode, 0,
                                               0, 0, kTestIfindex, AF_INET6);
  message->set_rdnss_option(RTNLMessage::RdnssOption(lifetime, dns_servers));
  return message;
}

std::unique_ptr<RTNLMessage> SLAACControllerTest::BuildAddressMessage(
    RTNLMessage::Mode mode,
    const net_base::IPCIDR& cidr,
    unsigned char flags,
    unsigned char scope) {
  auto message = std::make_unique<RTNLMessage>(
      RTNLMessage::kTypeAddress, mode, 0, 0, 0, kTestIfindex,
      net_base::ToSAFamily(cidr.GetFamily()));
  message->SetAttribute(IFA_ADDRESS, cidr.address().ToBytes());
  message->set_address_status(
      RTNLMessage::AddressStatus(cidr.prefix_length(), flags, scope));
  return message;
}

TEST_F(SLAACControllerTest, IPv6DnsServerAddressesChanged) {
  std::vector<net_base::IPv6Address> dns_server_addresses_out;

  // No IPv6 dns server addresses.
  dns_server_addresses_out = slaac_controller_.GetRDNSSAddresses();
  EXPECT_EQ(0, dns_server_addresses_out.size());

  // Setup IPv6 dns server addresses.
  std::vector<net_base::IPv6Address> dns_server_addresses_in = {
      kTestIPAddress1,
      kTestIPAddress2,
  };

  // Infinite lifetime
  const uint32_t kInfiniteLifetime = 0xffffffff;
  auto message = BuildRdnssMessage(RTNLMessage::kModeAdd, kInfiniteLifetime,
                                   dns_server_addresses_in);

  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kRDNSS))
      .Times(1);
  SendRTNLMessage(*message);
  dns_server_addresses_out = slaac_controller_.GetRDNSSAddresses();
  // Verify addresses.
  EXPECT_EQ(dns_server_addresses_in, dns_server_addresses_out);

  // Lifetime of 120
  const uint32_t kLifetime120 = 120;
  auto message1 = BuildRdnssMessage(RTNLMessage::kModeAdd, kLifetime120,
                                    dns_server_addresses_in);
  EXPECT_CALL(*this, UpdateCallback(
                         SLAACController::SLAACController::UpdateType::kRDNSS))
      .Times(1);
  SendRTNLMessage(*message1);

  dns_server_addresses_out = slaac_controller_.GetRDNSSAddresses();
  // Verify addresses.
  EXPECT_EQ(dns_server_addresses_in, dns_server_addresses_out);

  // Lifetime of 0
  const uint32_t kLifetime0 = 0;
  auto message2 = BuildRdnssMessage(RTNLMessage::kModeAdd, kLifetime0,
                                    dns_server_addresses_in);
  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kRDNSS))
      .Times(1);
  SendRTNLMessage(*message2);

  dns_server_addresses_out = slaac_controller_.GetRDNSSAddresses();
  // Verify addresses.
  EXPECT_EQ(0, dns_server_addresses_out.size());
}

TEST_F(SLAACControllerTest, IPv6AddressChanged) {
  // Contains no addresses.
  EXPECT_TRUE(slaac_controller_.GetAddresses().empty());

  auto message = BuildAddressMessage(RTNLMessage::kModeAdd,
                                     net_base::IPCIDR(kTestIPAddress0), 0, 0);

  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kAddress))
      .Times(0);

  // We should ignore IPv4 addresses.
  SendRTNLMessage(*message);
  EXPECT_TRUE(slaac_controller_.GetAddresses().empty());

  message =
      BuildAddressMessage(RTNLMessage::kModeAdd,
                          net_base::IPCIDR(kTestIPAddress1), 0, RT_SCOPE_LINK);

  // We should ignore non-SCOPE_UNIVERSE messages for IPv6.
  SendRTNLMessage(*message);
  EXPECT_TRUE(slaac_controller_.GetAddresses().empty());

  message = BuildAddressMessage(RTNLMessage::kModeAdd,
                                net_base::IPCIDR(kTestIPAddress2),
                                IFA_F_TEMPORARY, RT_SCOPE_UNIVERSE);

  // Add a temporary address.
  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kAddress))
      .Times(1);
  SendRTNLMessage(*message);
  EXPECT_EQ(
      slaac_controller_.GetAddresses(),
      std::vector<net_base::IPv6CIDR>({net_base::IPv6CIDR(kTestIPAddress2)}));

  message = BuildAddressMessage(RTNLMessage::kModeAdd,
                                net_base::IPCIDR(kTestIPAddress3), 0,
                                RT_SCOPE_UNIVERSE);

  // Adding a non-temporary address alerts the Device, but does not override
  // the primary address since the previous one was temporary.
  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kAddress))
      .Times(1);
  SendRTNLMessage(*message);
  EXPECT_EQ(
      slaac_controller_.GetAddresses(),
      std::vector<net_base::IPv6CIDR>({net_base::IPv6CIDR(kTestIPAddress2),
                                       net_base::IPv6CIDR(kTestIPAddress3)}));

  message = BuildAddressMessage(
      RTNLMessage::kModeAdd, net_base::IPCIDR(kTestIPAddress4),
      IFA_F_TEMPORARY | IFA_F_DEPRECATED, RT_SCOPE_UNIVERSE);

  // Adding a temporary deprecated address alerts the Device, but does not
  // override the primary address since the previous one was non-deprecated.
  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kAddress))
      .Times(1);
  SendRTNLMessage(*message);
  EXPECT_EQ(
      slaac_controller_.GetAddresses(),
      std::vector<net_base::IPv6CIDR>({net_base::IPv6CIDR(kTestIPAddress2),
                                       net_base::IPv6CIDR(kTestIPAddress3),
                                       net_base::IPv6CIDR(kTestIPAddress4)}));

  message = BuildAddressMessage(RTNLMessage::kModeAdd,
                                net_base::IPCIDR(kTestIPAddress7),
                                IFA_F_TEMPORARY, RT_SCOPE_UNIVERSE);

  // Another temporary (non-deprecated) address alerts the Device, and will
  // override the previous primary address.
  EXPECT_CALL(*this, UpdateCallback(SLAACController::UpdateType::kAddress))
      .Times(1);
  SendRTNLMessage(*message);
  EXPECT_EQ(
      slaac_controller_.GetAddresses(),
      std::vector<net_base::IPv6CIDR>({net_base::IPv6CIDR(kTestIPAddress7),
                                       net_base::IPv6CIDR(kTestIPAddress2),
                                       net_base::IPv6CIDR(kTestIPAddress3),
                                       net_base::IPv6CIDR(kTestIPAddress4)}));
}

TEST_F(SLAACControllerTest, StartIPv6Flags) {
  EXPECT_CALL(proc_fs_,
              SetIPFlag(net_base::IPFamily::kIPv6, "disable_ipv6", "1"))
      .WillOnce(Return(true));
  EXPECT_CALL(proc_fs_,
              SetIPFlag(net_base::IPFamily::kIPv6, "disable_ipv6", "0"))
      .WillOnce(Return(true));
  EXPECT_CALL(proc_fs_, SetIPFlag(net_base::IPFamily::kIPv6, "accept_dad", "1"))
      .WillOnce(Return(true));
  EXPECT_CALL(proc_fs_, SetIPFlag(net_base::IPFamily::kIPv6, "accept_ra", "2"))
      .WillOnce(Return(true));
  EXPECT_CALL(proc_fs_,
              SetIPFlag(net_base::IPFamily::kIPv6, "use_tempaddr", "2"))
      .WillOnce(Return(true));

  slaac_controller_.Start();
}

}  // namespace shill
