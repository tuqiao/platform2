// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "patchpanel/subnet.h"

#include <arpa/inet.h>

#include <string>
#include <utility>

#include <base/check_op.h>
#include <base/functional/bind.h>
#include <base/logging.h>

#include "patchpanel/net_util.h"

namespace {
// Returns the offset from the base address given in network-byte order for
// the address given in network-byte order, or 0 if the second address is
// lower than the base address. Returns the offset in host-byte order.
uint32_t OffsetFromBaseAddress(uint32_t base_no, uint32_t addr_no) {
  if (ntohl(addr_no) < ntohl(base_no))
    return 0;
  return ntohl(addr_no) - ntohl(base_no);
}
// Adds a positive offset given in host order to the address given in
// network byte order. Returns the address in network-byte order.
uint32_t AddOffset(uint32_t addr_no, uint32_t offset_ho) {
  return htonl(ntohl(addr_no) + offset_ho);
}
}  // namespace

namespace patchpanel {

SubnetAddress::SubnetAddress(const net_base::IPv4CIDR& cidr,
                             base::OnceClosure release_cb)
    : cidr_(cidr), release_cb_(std::move(release_cb)) {}

SubnetAddress::~SubnetAddress() = default;

Subnet::Subnet(uint32_t base_addr,
               int prefix_length,
               base::OnceClosure release_cb)
    : base_addr_(base_addr),
      prefix_length_(prefix_length),
      release_cb_(std::move(release_cb)),
      weak_factory_(this) {
  CHECK_LT(prefix_length, 32);

  addrs_.resize(1ul << (32 - prefix_length), false);

  // Mark the base address and broadcast address as allocated.
  addrs_.front() = true;
  addrs_.back() = true;
}

Subnet::~Subnet() {
  std::move(release_cb_).Run();
}

std::unique_ptr<SubnetAddress> Subnet::Allocate(uint32_t addr) {
  return AllocateAtOffset(OffsetFromBaseAddress(base_addr_, addr) - 1);
}

std::unique_ptr<SubnetAddress> Subnet::AllocateAtOffset(uint32_t offset) {
  uint32_t addr = AddressAtOffset(offset);
  if (addr == INADDR_ANY) {
    return nullptr;
  }

  if (addrs_[offset + 1]) {
    // Address is already allocated.
    return nullptr;
  }

  addrs_[offset + 1] = true;
  return std::make_unique<SubnetAddress>(
      *net_base::IPv4CIDR::CreateFromAddressAndPrefix(
          ConvertUint32ToIPv4Address(addr), prefix_length_),
      base::BindOnce(&Subnet::Free, weak_factory_.GetWeakPtr(), offset + 1));
}

uint32_t Subnet::AddressAtOffset(uint32_t offset) const {
  if (offset < 0 || offset >= AvailableCount())
    return INADDR_ANY;

  // The first usable IP is after the base address.
  return AddOffset(base_addr_, 1 + offset);
}

uint32_t Subnet::AvailableCount() const {
  // The available IP count is all IPs in a subnet, minus the network ID
  // and the broadcast address.
  return static_cast<uint32_t>(addrs_.size()) - 2;
}

uint32_t Subnet::BaseAddress() const {
  return base_addr_;
}

uint32_t Subnet::Netmask() const {
  return Ipv4Netmask(prefix_length_);
}

uint32_t Subnet::Prefix() const {
  return base_addr_ & Netmask();
}

int Subnet::PrefixLength() const {
  return prefix_length_;
}

std::string Subnet::ToCidrString() const {
  return IPv4AddressToCidrString(base_addr_, prefix_length_);
}

void Subnet::Free(uint32_t offset) {
  DCHECK_NE(offset, 0);
  DCHECK_LT(offset, addrs_.size() - 1);

  addrs_[offset] = false;
}

}  // namespace patchpanel
