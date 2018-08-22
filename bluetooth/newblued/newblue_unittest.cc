// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bluetooth/newblued/newblue.h"

#include <memory>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/message_loop/message_loop.h>
#include <gtest/gtest.h>

#include "bluetooth/newblued/mock_libnewblue.h"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Pair;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::UnorderedElementsAre;

namespace bluetooth {

namespace {

constexpr uniq_t kDiscoveryHandle = 11;

}  // namespace

class NewblueTest : public ::testing::Test {
 public:
  // A dummy struct that hosts the device information from discovery callback.
  struct MockDevice {
    std::string address;
    std::string name;
    int16_t rssi;
    uint32_t eir_class;
  };

  void SetUp() override {
    auto libnewblue = std::make_unique<MockLibNewblue>();
    libnewblue_ = libnewblue.get();
    newblue_ = std::make_unique<Newblue>(std::move(libnewblue));
  }

  bool StubHciUp(const uint8_t* address,
                 hciReadyForUpCbk callback,
                 void* callback_data) {
    callback(callback_data);
    return true;
  }

  void OnReadyForUp() { is_ready_for_up_ = true; }

  void OnDeviceDiscovered(const Device& device) {
    discovered_devices_.push_back({device.address, device.name.value(),
                                   device.rssi.value(),
                                   device.eir_class.value()});
  }

 protected:
  base::MessageLoop message_loop_;
  bool is_ready_for_up_ = false;
  std::unique_ptr<Newblue> newblue_;
  MockLibNewblue* libnewblue_;
  std::vector<MockDevice> discovered_devices_;
};

TEST_F(NewblueTest, ListenReadyForUp) {
  newblue_->Init();

  hciReadyForUpCbk up_callback;
  EXPECT_CALL(*libnewblue_, HciUp(_, _, _))
      .WillOnce(DoAll(SaveArg<1>(&up_callback),
                      Invoke(this, &NewblueTest::StubHciUp)));
  bool success = newblue_->ListenReadyForUp(
      base::Bind(&NewblueTest::OnReadyForUp, base::Unretained(this)));
  EXPECT_TRUE(success);
  message_loop_.RunUntilIdle();
  EXPECT_TRUE(is_ready_for_up_);

  // If libnewblue says the stack is ready for up again, ignore that.
  // We shouldn't bring up the stack more than once.
  is_ready_for_up_ = false;
  up_callback(newblue_.get());
  message_loop_.RunUntilIdle();
  EXPECT_FALSE(is_ready_for_up_);
}

TEST_F(NewblueTest, ListenReadyForUpFailed) {
  newblue_->Init();

  EXPECT_CALL(*libnewblue_, HciUp(_, _, _)).WillOnce(Return(false));
  bool success = newblue_->ListenReadyForUp(
      base::Bind(&NewblueTest::OnReadyForUp, base::Unretained(this)));
  EXPECT_FALSE(success);
}

TEST_F(NewblueTest, BringUp) {
  EXPECT_CALL(*libnewblue_, HciIsUp()).WillOnce(Return(false));
  EXPECT_FALSE(newblue_->BringUp());

  EXPECT_CALL(*libnewblue_, HciIsUp()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, L2cInit()).WillOnce(Return(0));
  EXPECT_CALL(*libnewblue_, AttInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, GattProfileInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, GattBuiltinInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, SmInit(HCI_DISP_CAP_NONE)).WillOnce(Return(true));
  EXPECT_TRUE(newblue_->BringUp());
}

TEST_F(NewblueTest, StartDiscovery) {
  newblue_->Init();

  EXPECT_CALL(*libnewblue_, HciIsUp()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, L2cInit()).WillOnce(Return(0));
  EXPECT_CALL(*libnewblue_, AttInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, GattProfileInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, GattBuiltinInit()).WillOnce(Return(true));
  EXPECT_CALL(*libnewblue_, SmInit(HCI_DISP_CAP_NONE)).WillOnce(Return(true));
  EXPECT_TRUE(newblue_->BringUp());

  hciDeviceDiscoveredLeCbk inquiry_response_callback;
  void* inquiry_response_callback_data;
  EXPECT_CALL(*libnewblue_, HciDiscoverLeStart(_, _, true, false))
      .WillOnce(DoAll(SaveArg<0>(&inquiry_response_callback),
                      SaveArg<1>(&inquiry_response_callback_data),
                      Return(kDiscoveryHandle)));
  newblue_->StartDiscovery(
      base::Bind(&NewblueTest::OnDeviceDiscovered, base::Unretained(this)));

  // 2 devices discovered.
  struct bt_addr addr1 = {.type = BT_ADDR_TYPE_LE_RANDOM,
                          .addr = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06}};
  uint8_t eir1[] = {
      6, static_cast<uint8_t>(EirType::NAME_SHORT), 'a', 'l', 'i', 'c', 'e'};
  inquiry_response_callback(inquiry_response_callback_data, &addr1, -101,
                            HCI_ADV_TYPE_SCAN_RSP, &eir1, arraysize(eir1));
  struct bt_addr addr2 = {.type = BT_ADDR_TYPE_LE_PUBLIC,
                          .addr = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07}};
  uint8_t eir2[] = {
      5, static_cast<uint8_t>(EirType::NAME_SHORT), 'b', 'o', 'b', '\0'};
  inquiry_response_callback(inquiry_response_callback_data, &addr2, -102,
                            HCI_ADV_TYPE_ADV_IND, &eir2, arraysize(eir2));
  message_loop_.RunUntilIdle();

  EXPECT_EQ(2, discovered_devices_.size());
  EXPECT_EQ("alice", discovered_devices_[0].name);
  EXPECT_EQ("06:05:04:03:02:01", discovered_devices_[0].address);
  EXPECT_EQ(-101, discovered_devices_[0].rssi);
  EXPECT_EQ("bob", discovered_devices_[1].name);
  EXPECT_EQ("07:06:05:04:03:02", discovered_devices_[1].address);
  EXPECT_EQ(-102, discovered_devices_[1].rssi);

  // Scan response for device 1.
  uint8_t eir3[] = {4, static_cast<uint8_t>(EirType::CLASS_OF_DEV), 0x21, 0x22,
                    0x23};
  inquiry_response_callback(inquiry_response_callback_data, &addr1, -103,
                            HCI_ADV_TYPE_SCAN_RSP, &eir3, arraysize(eir3));

  message_loop_.RunUntilIdle();

  // The third discovery event should be an update to the first device, not a
  // new device.
  EXPECT_EQ(3, discovered_devices_.size());
  EXPECT_EQ("alice", discovered_devices_[2].name);
  EXPECT_EQ("06:05:04:03:02:01", discovered_devices_[2].address);
  EXPECT_EQ(-103, discovered_devices_[2].rssi);
  EXPECT_EQ(0x232221, discovered_devices_[2].eir_class);

  EXPECT_CALL(*libnewblue_, HciDiscoverLeStop(kDiscoveryHandle))
      .WillOnce(Return(true));
  newblue_->StopDiscovery();
  // Any inquiry response after StopDiscovery should be ignored.
  inquiry_response_callback(inquiry_response_callback_data, &addr1, -101,
                            HCI_ADV_TYPE_SCAN_RSP, &eir1, arraysize(eir1));
  message_loop_.RunUntilIdle();
  // Check that discovered_devices_ is still the same.
  EXPECT_EQ(3, discovered_devices_.size());
}

TEST_F(NewblueTest, UpdateEirNormal) {
  Device device;
  uint8_t eir[] = {
      // Flag
      3, static_cast<uint8_t>(EirType::FLAGS), 0xAA, 0xBB,
      // UUID16_COMPLETE - Battery Service
      3, static_cast<uint8_t>(EirType::UUID16_COMPLETE), 0x0F, 0x18,
      // UUID32_INCOMPLETE - Blood Pressure
      5, static_cast<uint8_t>(EirType::UUID32_INCOMPLETE), 0x10, 0x18, 0x00,
      0x00,
      // UUID128_COMPLETE
      17, static_cast<uint8_t>(EirType::UUID128_COMPLETE), 0x0F, 0x0E, 0x0D,
      0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      0x00,
      // Name
      4, static_cast<uint8_t>(EirType::NAME_SHORT), 'f', 'o', 'o',
      // TX Power
      2, static_cast<uint8_t>(EirType::TX_POWER), 0xC7,
      // Class
      4, static_cast<uint8_t>(EirType::CLASS_OF_DEV), 0x01, 0x02, 0x03,
      // Service data associated with 16-bit Battery Service UUID
      5, static_cast<uint8_t>(EirType::SVC_DATA16), 0x0F, 0x18, 0x22, 0x11,
      // Service data associate with 32-bit Bond Management Service UUID
      7, static_cast<uint8_t>(EirType::SVC_DATA32), 0x1E, 0x18, 0x00, 0x00,
      0x44, 0x33,
      // Appearance
      3, static_cast<uint8_t>(EirType::GAP_APPEARANCE), 0x01, 0x02,
      // Manufacturer data
      5, static_cast<uint8_t>(EirType::MANUFACTURER_DATA), 0x0E, 0x00, 0x55,
      0x66};
  Uuid battery_service_uuid16({0x18, 0x0F});
  Uuid blood_pressure_uuid32({0x00, 0x00, 0x18, 0x10});
  Uuid bond_management_service_uuid32({0x00, 0x00, 0x18, 0x1E});
  Uuid uuid128({0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F});

  Newblue::UpdateEir(&device, std::vector<uint8_t>(eir, eir + arraysize(eir)));

  EXPECT_EQ(std::vector<uint8_t>({0xAA}), device.flags.value());
  EXPECT_THAT(device.service_uuids.value(),
              UnorderedElementsAre(battery_service_uuid16,
                                   blood_pressure_uuid32, uuid128));
  EXPECT_EQ("foo", device.name.value());
  EXPECT_EQ(-57, device.tx_power.value());
  EXPECT_EQ(0x00030201, device.eir_class.value());
  EXPECT_THAT(device.service_data.value(),
              UnorderedElementsAre(Pair(battery_service_uuid16,
                                        std::vector<uint8_t>({0x11, 0x22})),
                                   Pair(bond_management_service_uuid32,
                                        std::vector<uint8_t>({0x33, 0x44}))));
  EXPECT_EQ(0x0201, device.appearance.value());
  EXPECT_THAT(
      device.manufacturer.value(),
      UnorderedElementsAre(Pair(0x000E, std::vector<uint8_t>({0x55, 0x66}))));

  uint8_t eir2[] = {
      // Flag with zero octet
      1, static_cast<uint8_t>(EirType::FLAGS),
      // UUID32_INCOMPLETE - Bond Management Service
      5, static_cast<uint8_t>(EirType::UUID32_INCOMPLETE), 0x1E, 0x18, 0x00,
      0x00,
      // Service data associate with 32-bit Bond Management Service UUID
      7, static_cast<uint8_t>(EirType::SVC_DATA32), 0x1E, 0x18, 0x00, 0x00,
      0x66, 0x55};

  Newblue::UpdateEir(&device,
                     std::vector<uint8_t>(eir2, eir2 + arraysize(eir2)));

  EXPECT_FALSE(device.flags.value().empty());
  EXPECT_THAT(device.service_uuids.value(),
              UnorderedElementsAre(bond_management_service_uuid32));
  EXPECT_EQ("foo", device.name.value());
  EXPECT_EQ(-57, device.tx_power.value());
  EXPECT_EQ(0x00030201, device.eir_class.value());
  EXPECT_THAT(device.service_data.value(),
              UnorderedElementsAre(Pair(bond_management_service_uuid32,
                                        std::vector<uint8_t>({0x55, 0x66}))));
  EXPECT_EQ(0x0201, device.appearance.value());
  EXPECT_THAT(
      device.manufacturer.value(),
      UnorderedElementsAre(Pair(0x000E, std::vector<uint8_t>({0x55, 0x66}))));
}

TEST_F(NewblueTest, UpdateEirAbnormal) {
  Device device;
  uint8_t eir[] = {
      // Even if there are more than one instance of a UUID size of either
      // COMPLETE or INCOMPLETE type, the later one will still be honored
      3, static_cast<uint8_t>(EirType::UUID16_COMPLETE), 0x0F, 0x18,  //
      3, static_cast<uint8_t>(EirType::UUID16_INCOMPLETE), 0x10, 0x18,
      // Invalid UUID will be dropped.
      2, static_cast<uint8_t>(EirType::UUID32_INCOMPLETE), 0x10,
      // Contains non-ascii character
      5, static_cast<uint8_t>(EirType::NAME_SHORT), 0x80, 0x81, 'a', '\0',
      // TX Power with more than one octet will be dropped
      3, static_cast<uint8_t>(EirType::TX_POWER), 0xC7, 0x00,
      // Class with a wrong field length (2, should be 3)
      3, static_cast<uint8_t>(EirType::CLASS_OF_DEV), 0x01, 0x02,
      // Service data with an invalid service UUID will be dropped
      3, static_cast<uint8_t>(EirType::SVC_DATA16), 0x0F, 0x18,
      // Service data with zero length associated with 16-bit Battery Service
      // will be dropped
      3, static_cast<uint8_t>(EirType::SVC_DATA16), 0x0F, 0x18,
      // Wrong field length (4, should be 3)
      4, static_cast<uint8_t>(EirType::GAP_APPEARANCE), 0x01, 0x02, 0x03};
  Uuid battery_service_uuid16({0x18, 0x0F});
  Uuid blood_pressure_uuid16({0x18, 0x10});

  Newblue::UpdateEir(&device, std::vector<uint8_t>(eir, eir + arraysize(eir)));

  // Non-ascii characters are replaced with spaces.
  EXPECT_TRUE(device.flags.value().empty());
  EXPECT_THAT(
      device.service_uuids.value(),
      UnorderedElementsAre(battery_service_uuid16, blood_pressure_uuid16));
  EXPECT_EQ("  a", device.name.value());
  EXPECT_EQ(-128, device.tx_power.value());
  EXPECT_EQ(0x1F00, device.eir_class.value());
  EXPECT_TRUE(device.service_data.value().empty());
  EXPECT_EQ(0x0000, device.appearance.value());
  EXPECT_THAT(device.manufacturer.value(),
              UnorderedElementsAre(Pair(0xFFFF, std::vector<uint8_t>())));
}

}  // namespace bluetooth
