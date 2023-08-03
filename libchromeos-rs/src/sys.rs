// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Re-export types from crosvm_base that are used in CrOS.
// Note: This list is supposed to shrink over time as crosvm_base functionality is replaced with
// third_party crates or ChromeOS specific implementations.
// Do not add to this list.
pub use crosvm_base::errno_result;
pub use crosvm_base::unix::duration_to_timespec;
pub use crosvm_base::unix::vsock;
pub use crosvm_base::unix::SharedMemory;
pub use crosvm_base::AsRawDescriptor;
pub use crosvm_base::Error;
pub use crosvm_base::FromRawDescriptor;
pub use crosvm_base::IntoRawDescriptor;
pub use crosvm_base::RawDescriptor;
pub use crosvm_base::Result;
pub use crosvm_base::SafeDescriptor;
pub use crosvm_base::ScmSocket;
