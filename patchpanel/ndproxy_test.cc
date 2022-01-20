// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "patchpanel/ndproxy.h"

#include <stdlib.h>

#include <net/ethernet.h>

#include <memory>
#include <string>

#include <base/logging.h>
#include <gtest/gtest.h>

namespace patchpanel {

const MacAddress physical_if_mac({0xa0, 0xce, 0xc8, 0xc6, 0x91, 0x0a});
const MacAddress guest_if_mac({0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53});

const uint8_t ping_frame[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x86, 0xdd, 0x60, 0x0b, 0x8d, 0xb4, 0x00, 0x40, 0x3a, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0xb9, 0x3c, 0x13, 0x8f,
    0x00, 0x09, 0xde, 0x6a, 0x78, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x8e, 0x13,
    0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
    0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37};

const uint8_t rs_frame[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x02, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0,
    0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2, 0x80, 0x97, 0x83,
    0x76, 0xbf, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x85, 0x00, 0x2f, 0xfc, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0};

// The byte at index 19 should be 0x10 instead of 0x11
const uint8_t rs_frame_too_large_plen[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x02, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0,
    0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x11, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2, 0x80, 0x97, 0x83,
    0x76, 0xbf, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x85, 0x00, 0x2f, 0xfc, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0};

// The byte at index 19 should be 0x10 instead of 0x0f
const uint8_t rs_frame_too_small_plen[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x02, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0,
    0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2, 0x80, 0x97, 0x83,
    0x76, 0xbf, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x85, 0x00, 0x2f, 0xfc, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0};

const uint8_t rs_frame_translated[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x02, 0xa0, 0xce, 0xc8, 0xc6, 0x91, 0x0a,
    0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2, 0x80, 0x97, 0x83,
    0x76, 0xbf, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x85, 0x00, 0x93, 0x55, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0xa0, 0xce, 0xc8, 0xc6, 0x91, 0x0a};

const uint8_t ra_frame[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xc4, 0x71, 0xfe, 0xf1, 0xf6, 0x7f,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0x8a, 0xd5, 0x40, 0x00,
    0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0xc4, 0x71, 0xfe, 0xf1, 0xf6, 0x7f, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x05, 0xdc, 0x03, 0x04, 0x40, 0xc0, 0x00, 0x27, 0x8d, 0x00, 0x00, 0x09,
    0x3a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x24, 0x01, 0xfa, 0x00, 0x00, 0x04,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t ra_frame_translated[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0xdc, 0x53, 0x40, 0x04,
    0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x05, 0xdc, 0x03, 0x04, 0x40, 0xc0, 0x00, 0x27, 0x8d, 0x00, 0x00, 0x09,
    0x3a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x24, 0x01, 0xfa, 0x00, 0x00, 0x04,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t ra_frame_option_reordered[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xc4, 0x71, 0xfe, 0xf1, 0xf6, 0x7f,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0x8a, 0xd5, 0x40, 0x00,
    0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x05, 0xdc, 0x01, 0x01, 0xc4, 0x71, 0xfe, 0xf1,
    0xf6, 0x7f, 0x03, 0x04, 0x40, 0xc0, 0x00, 0x27, 0x8d, 0x00, 0x00, 0x09,
    0x3a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x24, 0x01, 0xfa, 0x00, 0x00, 0x04,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t ra_frame_option_reordered_translated[] = {
    0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0xdc, 0x53, 0x40, 0x04,
    0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x05, 0xdc, 0x01, 0x01, 0xd2, 0x47, 0xf7, 0xc5,
    0x9e, 0x53, 0x03, 0x04, 0x40, 0xc0, 0x00, 0x27, 0x8d, 0x00, 0x00, 0x09,
    0x3a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x24, 0x01, 0xfa, 0x00, 0x00, 0x04,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t ns_frame[] = {
    0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53, 0x1a, 0x9b, 0x82, 0xbd, 0xc0,
    0xa0, 0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x20, 0x3a, 0xff,
    0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2,
    0x80, 0x97, 0x83, 0x76, 0xbf, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1, 0xf6, 0x7f, 0x87,
    0x00, 0xba, 0x27, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1, 0xf6,
    0x7f, 0x01, 0x01, 0x1a, 0x9b, 0x82, 0xbd, 0xc0, 0xa0};

const uint8_t ns_frame_translated[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xa0, 0xce, 0xc8, 0xc6, 0x91,
    0x0a, 0x86, 0xdd, 0x60, 0x00, 0x00, 0x00, 0x00, 0x20, 0x3a, 0xff,
    0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75, 0xb2,
    0x80, 0x97, 0x83, 0x76, 0xbf, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1, 0xf6, 0x7f, 0x87,
    0x00, 0x1d, 0x81, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1, 0xf6,
    0x7f, 0x01, 0x01, 0xa0, 0xce, 0xc8, 0xc6, 0x91, 0x0a};

const uint8_t na_frame[] = {
    0xa0, 0xce, 0xc8, 0xc6, 0x91, 0x0a, 0xc4, 0x71, 0xfe, 0xf1, 0xf6, 0x7f,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75,
    0xb2, 0x80, 0x97, 0x83, 0x76, 0xbf, 0x88, 0x00, 0x58, 0x29, 0xc0, 0x00,
    0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71,
    0xfe, 0xff, 0xfe, 0xf1, 0xf6, 0x7f};

const uint8_t na_frame_translated[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd2, 0x47, 0xf7, 0xc5, 0x9e, 0x53,
    0x86, 0xdd, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3a, 0xff, 0xfe, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71, 0xfe, 0xff, 0xfe, 0xf1,
    0xf6, 0x7f, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x75,
    0xb2, 0x80, 0x97, 0x83, 0x76, 0xbf, 0x88, 0x00, 0x58, 0x29, 0xc0, 0x00,
    0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x71,
    0xfe, 0xff, 0xfe, 0xf1, 0xf6, 0x7f};

const uint8_t tcp_frame[] = {
    0xc4, 0x71, 0xfe, 0xf1, 0xf6, 0x7f, 0xa0, 0xce, 0xc8, 0xc6, 0x91,
    0x0a, 0x86, 0xdd, 0x60, 0x03, 0xa3, 0x57, 0x00, 0x20, 0x06, 0x40,
    0x24, 0x01, 0xfa, 0x00, 0x00, 0x04, 0x00, 0x02, 0xf0, 0x94, 0x0d,
    0xa1, 0x12, 0x6f, 0xfd, 0x6b, 0x24, 0x04, 0x68, 0x00, 0x40, 0x08,
    0x0c, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x85,
    0xc0, 0x01, 0xbb, 0xb2, 0x7e, 0xd0, 0xa6, 0x0c, 0x57, 0xa5, 0x6c,
    0x80, 0x10, 0x01, 0x54, 0x04, 0xb9, 0x00, 0x00, 0x01, 0x01, 0x08,
    0x0a, 0x00, 0x5a, 0x59, 0xc0, 0x32, 0x53, 0x14, 0x3a};

constexpr const char hex_chars[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

std::string ToHexString(const uint8_t* buffer, size_t len) {
  std::string s;
  std::string sep;
  for (int i = 0; i < len; i++) {
    const uint8_t b = buffer[i];
    s += sep;
    s += "0x";
    s += hex_chars[(b & 0xF0) >> 4];
    s += hex_chars[(b & 0x0F) >> 0];
    sep = ", ";
  }
  return s;
}

TEST(NDProxyTest, GetPrefixInfoOption) {
  uint8_t in_buffer_extended[IP_MAXPACKET + ETHER_HDR_LEN + 4];
  uint8_t* in_buffer = NDProxy::AlignFrameBuffer(in_buffer_extended);

  struct {
    std::string name;
    const uint8_t* input_frame;
    size_t input_frame_len;
    uint8_t expected_prefix_len;
    uint32_t expected_valid_time;
    uint32_t expected_preferred_time;
  } test_cases[] = {
      {
          "ra_frame",
          ra_frame,
          sizeof(ra_frame),
          64,
          ntohl(720 * 60 * 60),
          ntohl(168 * 60 * 60),
      },
      {
          "ra_frame_translated",
          ra_frame_translated,
          sizeof(ra_frame_translated),
          64,
          ntohl(720 * 60 * 60),
          ntohl(168 * 60 * 60),
      },
      {
          "ra_frame_option_reordered",
          ra_frame_option_reordered,
          sizeof(ra_frame_option_reordered),
          64,
          ntohl(720 * 60 * 60),
          ntohl(168 * 60 * 60),
      },
      {
          "ra_frame_option_reordered_translated",
          ra_frame_option_reordered_translated,
          sizeof(ra_frame_option_reordered_translated),
          64,
          ntohl(720 * 60 * 60),
          ntohl(168 * 60 * 60),
      },
      {
          "rs_frame",
          rs_frame,
          sizeof(rs_frame),
          0,
          0,
          0,
      },
      {
          "ns_frame",
          ns_frame,
          sizeof(ns_frame),
          0,
          0,
          0,
      },
      {
          "na_frame",
          na_frame,
          sizeof(na_frame),
          0,
          0,
          0,
      },
  };

  for (const auto& test_case : test_cases) {
    LOG(INFO) << test_case.name;

    memcpy(in_buffer, test_case.input_frame, test_case.input_frame_len);
    size_t offset = ETHER_HDR_LEN + sizeof(ip6_hdr);
    size_t icmp6_len = test_case.input_frame_len - offset;
    // const icmp6_hdr* icmp6 = reinterpret_cast<icmp6_hdr*>();
    const nd_opt_prefix_info* prefix_info =
        NDProxy::GetPrefixInfoOption(in_buffer + offset, icmp6_len);

    if (test_case.expected_prefix_len == 0) {
      EXPECT_EQ(nullptr, prefix_info);
    } else {
      EXPECT_NE(nullptr, prefix_info);
      EXPECT_EQ(test_case.expected_prefix_len,
                prefix_info->nd_opt_pi_prefix_len);
      EXPECT_EQ(test_case.expected_valid_time,
                prefix_info->nd_opt_pi_valid_time);
      EXPECT_EQ(test_case.expected_preferred_time,
                prefix_info->nd_opt_pi_preferred_time);
    }
  }
}

TEST(NDProxyTest, TranslateFrame) {
  uint8_t in_buffer_extended[IP_MAXPACKET + ETHER_HDR_LEN + 4];
  uint8_t out_buffer_extended[IP_MAXPACKET + ETHER_HDR_LEN + 4];
  uint8_t* in_buffer = NDProxy::AlignFrameBuffer(in_buffer_extended);
  uint8_t* out_buffer = NDProxy::AlignFrameBuffer(out_buffer_extended);
  int result;

  NDProxy ndproxy;
  ndproxy.Init();

  struct {
    std::string name;
    const uint8_t* input_frame;
    size_t input_frame_len;
    MacAddress local_mac;
    in6_addr* src_ip;
    ssize_t expected_error;
    const uint8_t* expected_output_frame;
    size_t expected_output_frame_len;
  } test_cases[] = {
      {
          "tcp_frame",
          tcp_frame,
          sizeof(tcp_frame),
          physical_if_mac,
          nullptr,
          NDProxy::kTranslateErrorNotICMPv6Frame,
      },
      {
          "ping_frame",
          ping_frame,
          sizeof(ping_frame),
          physical_if_mac,
          nullptr,
          NDProxy::kTranslateErrorNotNDFrame,
      },
      {
          "rs_frame_too_large_plen",
          rs_frame_too_large_plen,
          sizeof(rs_frame_too_large_plen),
          physical_if_mac,
          nullptr,
          NDProxy::kTranslateErrorMismatchedIp6Length,
      },
      {
          "rs_frame_too_small_plen",
          rs_frame_too_small_plen,
          sizeof(rs_frame_too_small_plen),
          physical_if_mac,
          nullptr,
          NDProxy::kTranslateErrorMismatchedIp6Length,
      },
      {
          "rs_frame",
          rs_frame,
          sizeof(rs_frame),
          physical_if_mac,
          nullptr,
          0,  // no error
          rs_frame_translated,
          sizeof(rs_frame_translated),
      },
      {
          "ra_frame",
          ra_frame,
          sizeof(ra_frame),
          guest_if_mac,
          nullptr,
          0,  // no error
          ra_frame_translated,
          sizeof(ra_frame_translated),
      },
      {
          "ra_frame_option_reordered",
          ra_frame_option_reordered,
          sizeof(ra_frame_option_reordered),
          guest_if_mac,
          nullptr,
          0,  // no error
          ra_frame_option_reordered_translated,
          sizeof(ra_frame_option_reordered_translated),
      },
      {
          "ns_frame",
          ns_frame,
          sizeof(ns_frame),
          physical_if_mac,
          nullptr,
          0,  // no error
          ns_frame_translated,
          sizeof(ns_frame_translated),
      },
      {
          "na_frame",
          na_frame,
          sizeof(na_frame),
          guest_if_mac,
          nullptr,
          0,  // no error
          na_frame_translated,
          sizeof(na_frame_translated),
      },
  };

  for (const auto& test_case : test_cases) {
    LOG(INFO) << test_case.name;

    memcpy(in_buffer, test_case.input_frame, test_case.input_frame_len);
    result = ndproxy.TranslateNDFrame(in_buffer, test_case.input_frame_len,
                                      test_case.local_mac, test_case.src_ip,
                                      out_buffer);

    if (test_case.expected_error != 0) {
      EXPECT_EQ(test_case.expected_error, result);
    } else {
      EXPECT_EQ(test_case.expected_output_frame_len, result);

      const auto expected = ToHexString(test_case.expected_output_frame,
                                        test_case.expected_output_frame_len);
      const auto received =
          ToHexString(out_buffer, test_case.expected_output_frame_len);
      EXPECT_EQ(expected, received);
    }
  }
}

}  // namespace patchpanel
