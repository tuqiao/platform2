// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/net/netlink_socket.h"

#include <linux/netlink.h>

#include <algorithm>
#include <vector>

#include <base/containers/span.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <net-base/byte_utils.h>

#include "shill/net/mock_sockets.h"
#include "shill/net/netlink_fd.h"
#include "shill/net/netlink_message.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::Test;

namespace shill {

const int kFakeFd = 99;

class NetlinkSocketTest : public Test {
 public:
  NetlinkSocketTest() = default;
  ~NetlinkSocketTest() override = default;

  void SetUp() override {
    mock_sockets_ = new MockSockets();
    netlink_socket_.sockets_.reset(mock_sockets_);
  }

  virtual void InitializeSocket(int fd) {
    EXPECT_CALL(*mock_sockets_,
                Socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_GENERIC))
        .WillOnce(Return(fd));
    EXPECT_CALL(*mock_sockets_, SetReceiveBuffer(fd, kNetlinkReceiveBufferSize))
        .WillOnce(Return(0));
    EXPECT_CALL(*mock_sockets_, Bind(fd, _, sizeof(struct sockaddr_nl)))
        .WillOnce(Return(0));
    EXPECT_TRUE(netlink_socket_.Init());
  }

 protected:
  MockSockets* mock_sockets_;  // Owned by netlink_socket_.
  NetlinkSocket netlink_socket_;
};

class FakeSocketRead {
 public:
  explicit FakeSocketRead(base::span<const uint8_t> next_read_string)
      : next_read_string_(next_read_string.begin(), next_read_string.end()) {}
  // Copies |len| bytes of |next_read_string_| into |buf| and clears
  // |next_read_string_|.
  ssize_t FakeSuccessfulRead(int sockfd,
                             void* buf,
                             size_t len,
                             int flags,
                             struct sockaddr* src_addr,
                             socklen_t* addrlen) {
    if (!buf) {
      return -1;
    }
    int read_bytes = std::min(len, next_read_string_.size());
    memcpy(buf, next_read_string_.data(), read_bytes);
    next_read_string_.clear();
    return read_bytes;
  }

 private:
  std::vector<uint8_t> next_read_string_;
};

TEST_F(NetlinkSocketTest, InitWorkingTest) {
  SetUp();
  InitializeSocket(kFakeFd);
  EXPECT_CALL(*mock_sockets_, Close(kFakeFd));
}

TEST_F(NetlinkSocketTest, InitBrokenSocketTest) {
  SetUp();

  const int kBadFd = -1;
  EXPECT_CALL(*mock_sockets_, Socket(PF_NETLINK, _, NETLINK_GENERIC))
      .WillOnce(Return(kBadFd));
  EXPECT_CALL(*mock_sockets_, SetReceiveBuffer(_, _)).Times(0);
  EXPECT_CALL(*mock_sockets_, Bind(_, _, _)).Times(0);
  EXPECT_FALSE(netlink_socket_.Init());
}

TEST_F(NetlinkSocketTest, InitBrokenBufferTest) {
  SetUp();

  EXPECT_CALL(*mock_sockets_, Socket(PF_NETLINK, _, NETLINK_GENERIC))
      .WillOnce(Return(kFakeFd));
  EXPECT_CALL(*mock_sockets_,
              SetReceiveBuffer(kFakeFd, kNetlinkReceiveBufferSize))
      .WillOnce(Return(-1));
  EXPECT_CALL(*mock_sockets_, Bind(kFakeFd, _, sizeof(struct sockaddr_nl)))
      .WillOnce(Return(0));
  EXPECT_TRUE(netlink_socket_.Init());

  // Destructor.
  EXPECT_CALL(*mock_sockets_, Close(kFakeFd));
}

TEST_F(NetlinkSocketTest, InitBrokenBindTest) {
  SetUp();

  EXPECT_CALL(*mock_sockets_, Socket(PF_NETLINK, _, NETLINK_GENERIC))
      .WillOnce(Return(kFakeFd));
  EXPECT_CALL(*mock_sockets_,
              SetReceiveBuffer(kFakeFd, kNetlinkReceiveBufferSize))
      .WillOnce(Return(0));
  EXPECT_CALL(*mock_sockets_, Bind(kFakeFd, _, sizeof(struct sockaddr_nl)))
      .WillOnce(Return(-1));
  EXPECT_CALL(*mock_sockets_, Close(kFakeFd)).WillOnce(Return(0));
  EXPECT_FALSE(netlink_socket_.Init());
}

TEST_F(NetlinkSocketTest, SendMessageTest) {
  SetUp();
  InitializeSocket(kFakeFd);

  const std::vector<uint8_t> message =
      net_base::byte_utils::ByteStringToBytes("This text is really arbitrary");

  // Good Send.
  EXPECT_CALL(*mock_sockets_, Send(kFakeFd, message.data(), message.size(), 0))
      .WillOnce(Return(message.size()));
  EXPECT_TRUE(netlink_socket_.SendMessage(message));

  // Short Send.
  EXPECT_CALL(*mock_sockets_, Send(kFakeFd, message.data(), message.size(), 0))
      .WillOnce(Return(message.size() - 3));
  EXPECT_FALSE(netlink_socket_.SendMessage(message));

  // Bad Send.
  EXPECT_CALL(*mock_sockets_, Send(kFakeFd, message.data(), message.size(), 0))
      .WillOnce(Return(-1));
  EXPECT_FALSE(netlink_socket_.SendMessage(message));

  // Destructor.
  EXPECT_CALL(*mock_sockets_, Close(kFakeFd));
}

TEST_F(NetlinkSocketTest, SequenceNumberTest) {
  SetUp();

  // Just a sequence number.
  const uint32_t arbitrary_number = 42;
  netlink_socket_.sequence_number_ = arbitrary_number;
  EXPECT_EQ(arbitrary_number + 1, netlink_socket_.GetSequenceNumber());

  // Make sure we don't go to |NetlinkMessage::kBroadcastSequenceNumber|.
  netlink_socket_.sequence_number_ = NetlinkMessage::kBroadcastSequenceNumber;
  EXPECT_NE(NetlinkMessage::kBroadcastSequenceNumber,
            netlink_socket_.GetSequenceNumber());
}

TEST_F(NetlinkSocketTest, GoodRecvMessageTest) {
  SetUp();
  InitializeSocket(kFakeFd);

  static const std::vector<uint8_t> expected_results =
      net_base::byte_utils::ByteStringToBytes(
          "Random text may include things like 'freaking fracking foo'.");

  FakeSocketRead fake_socket_read(expected_results);

  // Expect one call to get the size...
  EXPECT_CALL(*mock_sockets_,
              RecvFrom(kFakeFd, _, _, MSG_TRUNC | MSG_PEEK, _, _))
      .WillOnce(Return(expected_results.size()));

  // ...and expect a second call to get the data.
  EXPECT_CALL(*mock_sockets_,
              RecvFrom(kFakeFd, _, expected_results.size(), 0, _, _))
      .WillOnce(Invoke(&fake_socket_read, &FakeSocketRead::FakeSuccessfulRead));

  std::vector<uint8_t> message;
  EXPECT_TRUE(netlink_socket_.RecvMessage(&message));
  EXPECT_EQ(message, expected_results);

  // Destructor.
  EXPECT_CALL(*mock_sockets_, Close(kFakeFd));
}

TEST_F(NetlinkSocketTest, BadRecvMessageTest) {
  SetUp();
  InitializeSocket(kFakeFd);

  std::vector<uint8_t> message;
  EXPECT_CALL(*mock_sockets_, RecvFrom(kFakeFd, _, _, _, _, _))
      .WillOnce(Return(-1));
  EXPECT_FALSE(netlink_socket_.RecvMessage(&message));

  EXPECT_CALL(*mock_sockets_, Close(kFakeFd));
}

}  // namespace shill.
