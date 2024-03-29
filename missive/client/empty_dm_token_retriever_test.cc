// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/client/empty_dm_token_retriever.h"

#include <string>

#include <base/functional/bind.h>
#include <base/test/task_environment.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "missive/util/statusor.h"
#include "missive/util/test_support_callbacks.h"

namespace reporting {

namespace {

class EmptyDMTokenRetrieverTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(EmptyDMTokenRetrieverTest, GetDMToken) {
  test::TestEvent<StatusOr<std::string>> dm_token_retrieved_event;
  EmptyDMTokenRetriever empty_dm_token_retriever;
  empty_dm_token_retriever.RetrieveDMToken(dm_token_retrieved_event.cb());
  const auto dm_token_result = dm_token_retrieved_event.result();
  ASSERT_OK(dm_token_result);
  EXPECT_THAT(dm_token_result.ValueOrDie(), ::testing::IsEmpty());
}

}  // namespace

}  // namespace reporting
