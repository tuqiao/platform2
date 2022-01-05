// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/memory/scoped_refptr.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "rmad/constants.h"
#include "rmad/state_handler/components_repair_state_handler.h"
#include "rmad/state_handler/state_handler_test_common.h"
#include "rmad/system/mock_runtime_probe_client.h"

using ComponentRepairStatus =
    rmad::ComponentsRepairState::ComponentRepairStatus;
using testing::_;
using testing::DoAll;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::SetArgPointee;

namespace rmad {

class ComponentsRepairStateHandlerTest : public StateHandlerTest {
 public:
  scoped_refptr<ComponentsRepairStateHandler> CreateStateHandler(
      bool runtime_probe_client_retval,
      const ComponentsWithIdentifier& probed_components) {
    auto mock_runtime_probe_client =
        std::make_unique<NiceMock<MockRuntimeProbeClient>>();
    ON_CALL(*mock_runtime_probe_client, ProbeCategories(_, _))
        .WillByDefault(DoAll(SetArgPointee<1>(probed_components),
                             Return(runtime_probe_client_retval)));
    return base::MakeRefCounted<ComponentsRepairStateHandler>(
        json_store_, std::move(mock_runtime_probe_client));
  }

  std::unique_ptr<ComponentsRepairState> CreateDefaultComponentsRepairState() {
    static const std::vector<RmadComponent> default_original_components = {
        RMAD_COMPONENT_KEYBOARD,           RMAD_COMPONENT_POWER_BUTTON,
        RMAD_COMPONENT_BASE_ACCELEROMETER, RMAD_COMPONENT_LID_ACCELEROMETER,
        RMAD_COMPONENT_BASE_GYROSCOPE,     RMAD_COMPONENT_LID_GYROSCOPE,
        RMAD_COMPONENT_AUDIO_CODEC};
    auto components_repair = std::make_unique<ComponentsRepairState>();
    for (auto component : default_original_components) {
      ComponentRepairStatus* component_repair_status =
          components_repair->add_components();
      component_repair_status->set_component(component);
      component_repair_status->set_repair_status(
          ComponentRepairStatus::RMAD_REPAIR_STATUS_ORIGINAL);
      component_repair_status->set_identifier("");
    }
    return components_repair;
  }
};

TEST_F(ComponentsRepairStateHandlerTest, InitializeState_Success) {
  auto handler = CreateStateHandler(true, {});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);
}

TEST_F(ComponentsRepairStateHandlerTest, InitializeState_Fail) {
  auto handler = CreateStateHandler(false, {});
  EXPECT_EQ(handler->InitializeState(),
            RMAD_ERROR_STATE_HANDLER_INITIALIZATION_FAILED);
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_Success) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();
  ComponentRepairStatus* component_repair_status =
      components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_BATTERY);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_REPLACED);
  component_repair_status->set_identifier("battery_abcd");
  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_OK);
  EXPECT_EQ(state_case, RmadState::StateCase::kDeviceDestination);

  std::vector<std::string> replaced_components;
  EXPECT_TRUE(
      json_store_->GetValue(kReplacedComponentNames, &replaced_components));
  EXPECT_EQ(
      replaced_components,
      std::vector<std::string>{RmadComponent_Name(RMAD_COMPONENT_BATTERY)});
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_Success_MlbRework) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();
  components_repair->set_mainboard_rework(true);
  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_OK);
  EXPECT_EQ(state_case, RmadState::StateCase::kDeviceDestination);

  std::vector<std::string> replaced_components;
  EXPECT_TRUE(
      json_store_->GetValue(kReplacedComponentNames, &replaced_components));

  std::set<std::string> replaced_components_set(replaced_components.begin(),
                                                replaced_components.end());
  std::set<std::string> expected_replaced_components_set = {
      RmadComponent_Name(RMAD_COMPONENT_BATTERY),
      RmadComponent_Name(RMAD_COMPONENT_KEYBOARD),
      RmadComponent_Name(RMAD_COMPONENT_POWER_BUTTON),
      RmadComponent_Name(RMAD_COMPONENT_BASE_ACCELEROMETER),
      RmadComponent_Name(RMAD_COMPONENT_LID_ACCELEROMETER),
      RmadComponent_Name(RMAD_COMPONENT_BASE_GYROSCOPE),
      RmadComponent_Name(RMAD_COMPONENT_LID_GYROSCOPE),
      RmadComponent_Name(RMAD_COMPONENT_AUDIO_CODEC),
  };
  EXPECT_EQ(replaced_components_set, expected_replaced_components_set);
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_MissingState) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  // No ComponentsRepairState.
  RmadState state;

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kComponentsRepair);
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_UnknownComponent) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();
  ComponentRepairStatus* component_repair_status =
      components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_BATTERY);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_ORIGINAL);
  component_repair_status->set_identifier("battery_abcd");
  // RMAD_COMPONENT_NETWORK is deprecated.
  component_repair_status = components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_NETWORK);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_ORIGINAL);
  component_repair_status->set_identifier("network_abcd");

  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kComponentsRepair);
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_UnprobedComponent) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();
  ComponentRepairStatus* component_repair_status =
      components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_BATTERY);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_ORIGINAL);
  component_repair_status->set_identifier("battery_abcd");
  // RMAD_COMPONENT_STORAGE is not probed.
  component_repair_status = components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_STORAGE);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_ORIGINAL);
  component_repair_status->set_identifier("storage_abcd");

  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kComponentsRepair);
}

TEST_F(ComponentsRepairStateHandlerTest,
       GetNextStateCase_MissingProbedComponent) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();
  // RMAD_COMPONENT_BATTERY is probed but set to MISSING.
  ComponentRepairStatus* component_repair_status =
      components_repair->add_components();
  component_repair_status->set_component(RMAD_COMPONENT_BATTERY);
  component_repair_status->set_repair_status(
      ComponentRepairStatus::RMAD_REPAIR_STATUS_MISSING);
  component_repair_status->set_identifier("storage_abcd");

  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kComponentsRepair);
}

TEST_F(ComponentsRepairStateHandlerTest, GetNextStateCase_UnknownRepairState) {
  auto handler =
      CreateStateHandler(true, {{RMAD_COMPONENT_BATTERY, "battery_abcd"}});
  EXPECT_EQ(handler->InitializeState(), RMAD_ERROR_OK);

  // State doesn't contain RMAD_COMPONENT_BATTERY.
  std::unique_ptr<ComponentsRepairState> components_repair =
      CreateDefaultComponentsRepairState();

  RmadState state;
  state.set_allocated_components_repair(components_repair.release());

  auto [error, state_case] = handler->GetNextStateCase(state);
  EXPECT_EQ(error, RMAD_ERROR_REQUEST_INVALID);
  EXPECT_EQ(state_case, RmadState::StateCase::kComponentsRepair);
}

}  // namespace rmad
