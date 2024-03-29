// Copyright 2015 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <base/strings/string_piece.h>
#include <brillo/any.h>
#include <brillo/variant_dictionary.h>
#include <gtest/gtest.h>

using brillo::GetVariantValueOrDefault;
using brillo::VariantDictionary;

TEST(VariantDictionary, GetVariantValueOrDefault) {
  VariantDictionary dictionary;
  dictionary.emplace("a", 1);
  dictionary.emplace("b", "string");

  // Test values that are present in the VariantDictionary.
  EXPECT_EQ(1, GetVariantValueOrDefault<int>(dictionary, "a"));
  EXPECT_EQ("string", GetVariantValueOrDefault<const char*>(dictionary, "b"));

  // Test that missing keys result in defaults.
  EXPECT_EQ("", GetVariantValueOrDefault<std::string>(dictionary, "missing"));
  EXPECT_EQ(0, GetVariantValueOrDefault<int>(dictionary, "missing"));
}

TEST(VariantDictionary, LookupWithStringPiece) {
  VariantDictionary dictionary;
  dictionary.emplace("a", 1);
  dictionary.emplace("b", "string");

  EXPECT_NE(dictionary.find(base::StringPiece("a")), dictionary.end());
  EXPECT_NE(dictionary.find(base::StringPiece("b")), dictionary.end());
}
