// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for OutOfProcessMountHelper.

#include "cryptohome/storage/out_of_process_mount_helper.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/posix/eintr_wrapper.h>
#include <brillo/cryptohome.h>
#include <gtest/gtest.h>

#include "cryptohome/crypto.h"
#include "cryptohome/filesystem_layout.h"
#include "cryptohome/mock_platform.h"
#include "cryptohome/storage/mount_utils.h"

#include "cryptohome/namespace_mounter_ipc.pb.h"

using base::FilePath;
using brillo::cryptohome::home::kGuestUserName;
using ::testing::_;
using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;

namespace {

constexpr char kChromeMountNamespace[] = "/run/namespaces/mnt_chrome";

constexpr pid_t kOOPHelperPid = 2;

constexpr int kInvalidFd = -1;

}  // namespace

namespace cryptohome {

class OutOfProcessMountHelperTest : public ::testing::Test {
 public:
  OutOfProcessMountHelperTest() : crypto_(&platform_) {}
  OutOfProcessMountHelperTest(const OutOfProcessMountHelperTest&) = delete;
  OutOfProcessMountHelperTest& operator=(const OutOfProcessMountHelperTest&) =
      delete;

  virtual ~OutOfProcessMountHelperTest() {}

  void SetUp() {
    // Populate the system salt.
    brillo::SecureBlob system_salt;
    InitializeFilesystemLayout(&platform_, &system_salt);
    platform_.GetFake()->SetSystemSaltForLibbrillo(system_salt);

    out_of_process_mounter_.reset(new OutOfProcessMountHelper(
        true /* legacy_mount */, true /* bind_mount_downloads */, &platform_));
  }

  void TearDown() {
    out_of_process_mounter_ = nullptr;
    platform_.GetFake()->RemoveSystemSaltForLibbrillo();
  }

  bool CreatePipe(base::ScopedFD* read_end, base::ScopedFD* write_end) {
    int pipe[2];
    bool success = base::CreateLocalNonBlockingPipe(pipe);
    if (success) {
      read_end->reset(pipe[0]);
      write_end->reset(pipe[1]);
    }
    return success;
  }

  base::ScopedFD GetDevNullFd() {
    return base::ScopedFD(HANDLE_EINTR(open("/dev/null", O_WRONLY)));
  }

  base::ScopedFD GetDevZeroFd() {
    return base::ScopedFD(HANDLE_EINTR(open("/dev/zero", O_RDONLY)));
  }

 protected:
  NiceMock<MockPlatform> platform_;
  Crypto crypto_;
  std::unique_ptr<OutOfProcessMountHelper> out_of_process_mounter_;
};

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOP) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  EXPECT_CALL(*process, pid()).WillRepeatedly(Return(kOOPHelperPid));

  // Allow reading from cryptohome's perspective.
  base::ScopedFD read_end, write_end;
  ASSERT_TRUE(CreatePipe(&read_end, &write_end));

  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO))
      .WillOnce(Return(read_end.get()));

  // Writing from cryptohome's perspective always succeeds.
  base::ScopedFD dev_null = GetDevNullFd();
  ASSERT_TRUE(dev_null.is_valid());
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO)).WillOnce(Return(dev_null.get()));

  FilePath legacy_home("/home/chronos/user");

  OutOfProcessMountResponse resp;
  resp.add_paths(legacy_home.value());
  ASSERT_TRUE(WriteProtobuf(write_end.get(), resp));
  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_NONE));

  EXPECT_TRUE(out_of_process_mounter_->IsPathMounted(legacy_home));
  EXPECT_FALSE(
      out_of_process_mounter_->IsPathMounted(FilePath("/invalid/path")));

  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(true));
  out_of_process_mounter_->UnmountAll();
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPWriteProtobuf) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  EXPECT_CALL(*process, pid()).WillRepeatedly(Return(kOOPHelperPid));

  // Reading from the helper always succeeds.
  base::ScopedFD dev_zero = GetDevZeroFd();
  ASSERT_TRUE(dev_zero.is_valid());
  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO))
      .WillOnce(Return(dev_zero.get()));

  // Allow writing from cryptohome's perspective.
  base::ScopedFD read_end, write_end;
  ASSERT_TRUE(CreatePipe(&read_end, &write_end));
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO))
      .WillOnce(Return(write_end.get()));

  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_NONE));

  OutOfProcessMountRequest r;
  ASSERT_TRUE(ReadProtobuf(read_end.get(), &r));
  EXPECT_EQ(r.username(), kGuestUserName);
  EXPECT_EQ(r.mount_namespace_path(), kChromeMountNamespace);

  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(true));
  out_of_process_mounter_->UnmountAll();
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPFailsToStart) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(false));
  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_FATAL));
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPNonRootMountNamespace) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  EXPECT_CALL(*process, pid()).WillRepeatedly(Return(kOOPHelperPid));
  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(true));

  out_of_process_mounter_.reset(new OutOfProcessMountHelper(
      true /* legacy_mount */, true /* bind_mount_downloads */, &platform_));

  // Reading from the helper always succeeds.
  base::ScopedFD dev_zero = GetDevZeroFd();
  ASSERT_TRUE(dev_zero.is_valid());
  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO))
      .WillOnce(Return(dev_zero.get()));

  // Allow writing from cryptohome's perspective.
  base::ScopedFD read_end, write_end;
  ASSERT_TRUE(CreatePipe(&read_end, &write_end));
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO))
      .WillOnce(Return(write_end.get()));

  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_NONE));

  OutOfProcessMountRequest r;
  ASSERT_TRUE(ReadProtobuf(read_end.get(), &r));
  EXPECT_EQ(r.username(), kGuestUserName);
  EXPECT_EQ(r.mount_namespace_path(), kChromeMountNamespace);

  out_of_process_mounter_->UnmountAll();
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPFailsToWriteProtobuf) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  // After the PID is checked once and the process is killed, pid() should
  // return 0.
  EXPECT_CALL(*process, pid())
      .WillOnce(Return(kOOPHelperPid))
      .WillRepeatedly(Return(0));

  // Writing the protobuf fails.
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO)).WillOnce(Return(kInvalidFd));

  // Reading from the helper always succeeds.
  base::ScopedFD dev_zero = GetDevZeroFd();
  ASSERT_TRUE(dev_zero.is_valid());
  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO))
      .WillOnce(Return(dev_zero.get()));

  // If writing the protobuf fails, OOP mount helper should be killed.
  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(true));

  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_FATAL));
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPFailsToReadAck) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  // After the PID is checked once and the process is killed, pid() should
  // return 0.
  EXPECT_CALL(*process, pid())
      .WillOnce(Return(kOOPHelperPid))
      .WillRepeatedly(Return(0));

  // Writing the protobuf succeeds.
  base::ScopedFD dev_null = GetDevNullFd();
  ASSERT_TRUE(dev_null.is_valid());
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO)).WillOnce(Return(dev_null.get()));

  // Reading the ack fails.
  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO)).WillOnce(Return(kInvalidFd));

  // If reading the ack fails, OOP mount helper should be killed.
  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(true));

  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_FATAL));
}

TEST_F(OutOfProcessMountHelperTest, MountGuestUserDirOOPFailsToPoke) {
  brillo::ProcessMock* process = platform_.mock_process();
  EXPECT_CALL(*process, Start()).WillOnce(Return(true));
  EXPECT_CALL(*process, pid()).WillRepeatedly(Return(kOOPHelperPid));

  // Writing the protobuf succeeds.
  base::ScopedFD write_to_helper = GetDevNullFd();
  ASSERT_TRUE(write_to_helper.is_valid());
  EXPECT_CALL(*process, GetPipe(STDIN_FILENO))
      .WillOnce(Return(write_to_helper.get()));

  // Reading from the helper always succeeds.
  base::ScopedFD read_from_helper = GetDevZeroFd();
  ASSERT_TRUE(read_from_helper.is_valid());
  EXPECT_CALL(*process, GetPipe(STDOUT_FILENO))
      .WillOnce(Return(read_from_helper.get()));

  ASSERT_THAT(out_of_process_mounter_->PerformEphemeralMount(kGuestUserName,
                                                             base::FilePath()),
              Eq(MOUNT_ERROR_NONE));

  // Poking the helper fails.
  EXPECT_CALL(*process, Kill(SIGTERM, _)).WillOnce(Return(false));
  // If poking fails, OOP mount helper should be killed with SIGKILL.
  EXPECT_CALL(*process, Kill(SIGKILL, _)).WillOnce(Return(true));

  out_of_process_mounter_->UnmountAll();
}

}  // namespace cryptohome
