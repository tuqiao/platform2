// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/storage/storage_base.h"

#include <base/files/file.h>
#include <base/functional/callback.h>
#include <base/memory/ref_counted.h>
#include <base/task/thread_pool.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/containers/adapters.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/files/file_util.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "missive/analytics/metrics.h"
#include "missive/encryption/encryption_module_interface.h"
#include "missive/encryption/primitives.h"
#include "missive/encryption/verification.h"
#include "missive/proto/record.pb.h"
#include "missive/proto/record_constants.pb.h"
#include "missive/storage/storage_configuration.h"
#include "missive/storage/storage_queue.h"
#include "missive/storage/storage_uploader_interface.h"
#include "missive/util/file.h"
#include "missive/util/status.h"
#include "missive/util/status_macros.h"

namespace reporting {

QueueUploaderInterface::QueueUploaderInterface(
    Priority priority,
    std::unique_ptr<UploaderInterface> storage_uploader_interface)
    : priority_(priority),
      storage_uploader_interface_(std::move(storage_uploader_interface)) {}

// Factory method.
void QueueUploaderInterface::AsyncProvideUploader(
    Priority priority,
    UploaderInterface::AsyncStartUploaderCb async_start_upload_cb,
    scoped_refptr<EncryptionModuleInterface> encryption_module,
    UploaderInterface::UploadReason reason,
    UploaderInterfaceResultCb start_uploader_cb) {
  async_start_upload_cb.Run(
      (/*need_encryption_key=*/encryption_module->is_enabled() &&
       encryption_module->need_encryption_key())
          ? UploaderInterface::UploadReason::KEY_DELIVERY
          : reason,
      base::BindOnce(&QueueUploaderInterface::WrapInstantiatedUploader,
                     priority, std::move(start_uploader_cb)));
}

void QueueUploaderInterface::ProcessRecord(
    EncryptedRecord encrypted_record,
    ScopedReservation scoped_reservation,
    base::OnceCallback<void(bool)> processed_cb) {
  // Update sequence information: add Priority.
  SequenceInformation* const sequence_info =
      encrypted_record.mutable_sequence_information();
  sequence_info->set_priority(priority_);
  storage_uploader_interface_->ProcessRecord(std::move(encrypted_record),
                                             std::move(scoped_reservation),
                                             std::move(processed_cb));
}

void QueueUploaderInterface::ProcessGap(
    SequenceInformation start,
    uint64_t count,
    base::OnceCallback<void(bool)> processed_cb) {
  // Update sequence information: add Priority.
  start.set_priority(priority_);
  storage_uploader_interface_->ProcessGap(std::move(start), count,
                                          std::move(processed_cb));
}

void QueueUploaderInterface::Completed(Status final_status) {
  storage_uploader_interface_->Completed(final_status);
}

void QueueUploaderInterface::WrapInstantiatedUploader(
    Priority priority,
    UploaderInterfaceResultCb start_uploader_cb,
    StatusOr<std::unique_ptr<UploaderInterface>> uploader_result) {
  if (!uploader_result.ok()) {
    std::move(start_uploader_cb).Run(uploader_result.status());
    return;
  }
  std::move(start_uploader_cb)
      .Run(std::make_unique<QueueUploaderInterface>(
          priority, std::move(uploader_result.ValueOrDie())));
}

// Factory method, returns smart pointer with deletion on sequence.
std::unique_ptr<KeyDelivery, base::OnTaskRunnerDeleter> KeyDelivery::Create(
    scoped_refptr<EncryptionModuleInterface> encryption_module,
    UploaderInterface::AsyncStartUploaderCb async_start_upload_cb) {
  auto sequence_task_runner = base::ThreadPool::CreateSequencedTaskRunner(
      {base::TaskPriority::BEST_EFFORT, base::MayBlock()});
  return std::unique_ptr<KeyDelivery, base::OnTaskRunnerDeleter>(
      new KeyDelivery(encryption_module, async_start_upload_cb,
                      sequence_task_runner),
      base::OnTaskRunnerDeleter(sequence_task_runner));
}

KeyDelivery::~KeyDelivery() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  upload_timer_.AbandonAndStop();
  PostResponses(
      Status(error::UNAVAILABLE, "Key not delivered - NewStorage shuts down"));
}

void KeyDelivery::Request(RequestCallback callback) {
  sequenced_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&KeyDelivery::EuqueueRequestAndPossiblyStart,
                                base::Unretained(this), std::move(callback)));
}

void KeyDelivery::OnCompletion(Status status) {
  sequenced_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&KeyDelivery::PostResponses,
                                base::Unretained(this), status));
}

void KeyDelivery::StartPeriodicKeyUpdate(const base::TimeDelta period) {
  sequenced_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](KeyDelivery* self, base::TimeDelta period) {
                       if (self->upload_timer_.IsRunning()) {
                         // We've already started the periodic key update.
                         return;
                       }
                       // `base::Unretained` is ok here because `upload_timer_`
                       // is destructed in the class destructor
                       self->upload_timer_.Start(
                           FROM_HERE, period,
                           base::BindRepeating(&KeyDelivery::RequestKeyIfNeeded,
                                               base::Unretained(self)));
                     },
                     base::Unretained(this), period));
}

KeyDelivery::KeyDelivery(
    scoped_refptr<EncryptionModuleInterface> encryption_module,
    UploaderInterface::AsyncStartUploaderCb async_start_upload_cb,
    scoped_refptr<base::SequencedTaskRunner> sequenced_task_runner)
    : sequenced_task_runner_(sequenced_task_runner),
      async_start_upload_cb_(async_start_upload_cb),
      encryption_module_(encryption_module) {
  DCHECK(encryption_module_) << "Encryption module pointer not set";
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

void KeyDelivery::RequestKeyIfNeeded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (encryption_module_->has_encryption_key() &&
      !encryption_module_->need_encryption_key()) {
    return;
  }
  // Request the key
  Request(base::BindOnce([](Status status) {
    // Log the request status in UMA
    const auto res = analytics::Metrics::SendEnumToUMA(
        /*name=*/kKeyDeliveryResultUma, status.code(), error::Code::MAX_VALUE);
    LOG_IF(ERROR, !res) << "SendLinearToUMA failure, " << kKeyDeliveryResultUma
                        << " " << static_cast<int>(status.code());
  }));
}

void KeyDelivery::EuqueueRequestAndPossiblyStart(RequestCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(callback);
  callbacks_.push_back(std::move(callback));

  // The first request, starting the roundtrip.
  // Initiate upload with need_encryption_key flag and no records.
  UploaderInterface::UploaderInterfaceResultCb start_uploader_cb =
      base::BindOnce(&KeyDelivery::EncryptionKeyReceiverReady,
                     base::Unretained(this));
  async_start_upload_cb_.Run(
      UploaderInterface::UploadReason::KEY_DELIVERY,
      base::BindOnce(&KeyDelivery::WrapInstantiatedKeyUploader,
                     /*priority=*/MANUAL_BATCH, std::move(start_uploader_cb)));
}

void KeyDelivery::PostResponses(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& callback : callbacks_) {
    std::move(callback).Run(status);
  }
  callbacks_.clear();
}

void KeyDelivery::WrapInstantiatedKeyUploader(
    Priority priority,
    UploaderInterface::UploaderInterfaceResultCb start_uploader_cb,
    StatusOr<std::unique_ptr<UploaderInterface>> uploader_result) {
  if (!uploader_result.ok()) {
    std::move(start_uploader_cb).Run(uploader_result.status());
    return;
  }
  std::move(start_uploader_cb)
      .Run(std::make_unique<QueueUploaderInterface>(
          priority, std::move(uploader_result.ValueOrDie())));
}

void KeyDelivery::EncryptionKeyReceiverReady(
    StatusOr<std::unique_ptr<UploaderInterface>> uploader_result) {
  if (!uploader_result.ok()) {
    OnCompletion(uploader_result.status());
    return;
  }
  uploader_result.ValueOrDie()->Completed(Status::StatusOK());
}

KeyInStorage::KeyInStorage(base::StringPiece signature_verification_public_key,
                           const base::FilePath& directory)
    : verifier_(signature_verification_public_key), directory_(directory) {}

KeyInStorage::~KeyInStorage() = default;

// Uploads signed encryption key to a file with an |index| >=
// |next_key_file_index_|. Returns status in case of any error. If succeeds,
// removes all files with lower indexes (if any). Called every time encryption
// key is updated.
Status KeyInStorage::UploadKeyFile(
    const SignedEncryptionInfo& signed_encryption_key) {
  // Atomically reserve file index (none else will get the same index).
  uint64_t new_file_index = next_key_file_index_.fetch_add(1);
  // Write into file.
  RETURN_IF_ERROR(WriteKeyInfoFile(new_file_index, signed_encryption_key));

  // Enumerate data files and delete all files with lower index.
  RemoveKeyFilesWithLowerIndexes(new_file_index);
  return Status::StatusOK();
}

// Locates and downloads the latest valid enumeration keys file.
// Atomically sets |next_key_file_index_| to the a value larger than any found
// file. Returns key and key id pair, or error status (NOT_FOUND if no valid
// file has been found). Called once during initialization only.
StatusOr<std::pair<std::string, EncryptionModuleInterface::PublicKeyId>>
KeyInStorage::DownloadKeyFile() {
  // Make sure the assigned directory exists.
  base::File::Error error;
  if (!base::CreateDirectoryAndGetError(directory_, &error)) {
    return Status(
        error::UNAVAILABLE,
        base::StrCat(
            {"Storage directory '", directory_.MaybeAsASCII(),
             "' does not exist, error=", base::File::ErrorToString(error)}));
  }

  // Enumerate possible key files, collect the ones that have valid name,
  // set next_key_file_index_ to a value that is definitely not used.
  base::flat_set<base::FilePath> all_key_files;
  base::flat_map<uint64_t, base::FilePath> found_key_files;
  EnumerateKeyFiles(&all_key_files, &found_key_files);

  // Try to unserialize the key from each found file (latest first).
  auto signed_encryption_key_result = LocateValidKeyAndParse(found_key_files);

  // If not found, return error.
  if (!signed_encryption_key_result.has_value()) {
    return Status(error::NOT_FOUND, "No valid encryption key found");
  }

  // Found and validated, delete all other files.
  for (const auto& full_name : all_key_files) {
    if (full_name == signed_encryption_key_result.value().first) {
      continue;  // This file is used.
    }
    DeleteFileWarnIfFailed(full_name);  // Ignore errors, if any.
  }

  // Return the key.
  return std::make_pair(
      signed_encryption_key_result.value().second.public_asymmetric_key(),
      signed_encryption_key_result.value().second.public_key_id());
}

Status KeyInStorage::VerifySignature(
    const SignedEncryptionInfo& signed_encryption_key) {
  if (signed_encryption_key.public_asymmetric_key().size() != kKeySize) {
    return Status{error::FAILED_PRECONDITION, "Key size mismatch"};
  }
  char value_to_verify[sizeof(EncryptionModuleInterface::PublicKeyId) +
                       kKeySize];
  const EncryptionModuleInterface::PublicKeyId public_key_id =
      signed_encryption_key.public_key_id();
  memcpy(value_to_verify, &public_key_id,
         sizeof(EncryptionModuleInterface::PublicKeyId));
  memcpy(value_to_verify + sizeof(EncryptionModuleInterface::PublicKeyId),
         signed_encryption_key.public_asymmetric_key().data(), kKeySize);
  return verifier_.Verify(std::string(value_to_verify, sizeof(value_to_verify)),
                          signed_encryption_key.signature());
}

// Writes key into file. Called during key upload.
Status KeyInStorage::WriteKeyInfoFile(
    uint64_t new_file_index,
    const SignedEncryptionInfo& signed_encryption_key) {
  base::FilePath key_file_path =
      directory_.Append(kEncryptionKeyFilePrefix)
          .AddExtensionASCII(base::NumberToString(new_file_index));
  base::File key_file(key_file_path,
                      base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  if (!key_file.IsValid()) {
    return Status(error::DATA_LOSS,
                  base::StrCat({"Cannot open key file='",
                                key_file_path.MaybeAsASCII(), "' for append"}));
  }
  std::string serialized_key;
  if (!signed_encryption_key.SerializeToString(&serialized_key) ||
      serialized_key.empty()) {
    return Status(error::DATA_LOSS,
                  base::StrCat({"Failed to seralize key into file='",
                                key_file_path.MaybeAsASCII(), "'"}));
  }
  const int32_t write_result = key_file.Write(
      /*offset=*/0, serialized_key.data(), serialized_key.size());
  if (write_result < 0) {
    return Status(
        error::DATA_LOSS,
        base::StrCat({"File write error=",
                      key_file.ErrorToString(key_file.GetLastFileError()),
                      " file=", key_file_path.MaybeAsASCII()}));
  }
  if (static_cast<size_t>(write_result) != serialized_key.size()) {
    return Status(error::DATA_LOSS,
                  base::StrCat({"Failed to seralize key into file='",
                                key_file_path.MaybeAsASCII(), "'"}));
  }
  return Status::StatusOK();
}

// Enumerates key files and deletes those with index lower than
// |new_file_index|. Called during key upload.
void KeyInStorage::RemoveKeyFilesWithLowerIndexes(uint64_t new_file_index) {
  base::FileEnumerator dir_enum(directory_,
                                /*recursive=*/false,
                                base::FileEnumerator::FILES,
                                base::StrCat({kEncryptionKeyFilePrefix, "*"}));
  DeleteFilesWarnIfFailed(
      dir_enum,
      base::BindRepeating(
          [](uint64_t new_file_index, const base::FilePath& full_name) {
            const auto file_index =
                StorageQueue::GetFileSequenceIdFromPath(full_name);
            if (!file_index.ok() ||  // Should not happen, will remove file.
                file_index.ValueOrDie() <
                    static_cast<int64_t>(
                        new_file_index)) {  // Lower index file, will remove
                                            // it.
              return true;
            }
            return false;
          },
          new_file_index));
}

// Enumerates possible key files, collects the ones that have valid name,
// sets next_key_file_index_ to a value that is definitely not used.
// Called once, during initialization.
void KeyInStorage::EnumerateKeyFiles(
    base::flat_set<base::FilePath>* all_key_files,
    base::flat_map<uint64_t, base::FilePath>* found_key_files) {
  base::FileEnumerator dir_enum(directory_,
                                /*recursive=*/false,
                                base::FileEnumerator::FILES,
                                base::StrCat({kEncryptionKeyFilePrefix, "*"}));
  for (auto full_name = dir_enum.Next(); !full_name.empty();
       full_name = dir_enum.Next()) {
    if (!all_key_files->emplace(full_name).second) {
      // Duplicate file name. Should not happen.
      continue;
    }
    const auto file_index = StorageQueue::GetFileSequenceIdFromPath(full_name);
    if (!file_index.ok()) {  // Shouldn't happen, something went wrong.
      continue;
    }
    if (!found_key_files
             ->emplace(static_cast<uint64_t>(file_index.ValueOrDie()),
                       full_name)
             .second) {
      // Duplicate extension (e.g., 01 and 001). Should not happen (file is
      // corrupt).
      continue;
    }
    // Set 'next_key_file_index_' to a number which is definitely not used.
    if (static_cast<int64_t>(next_key_file_index_.load()) <=
        file_index.ValueOrDie()) {
      next_key_file_index_.store(
          static_cast<uint64_t>(file_index.ValueOrDie() + 1));
    }
  }
}

// Enumerates found key files and locates one with the highest index and
// valid key. Returns pair of file name and loaded signed key proto.
// Called once, during initialization.
std::optional<std::pair<base::FilePath, SignedEncryptionInfo>>
KeyInStorage::LocateValidKeyAndParse(
    const base::flat_map<uint64_t, base::FilePath>& found_key_files) {
  // Try to unserialize the key from each found file (latest first).
  for (const auto& [index, file_path] : base::Reversed(found_key_files)) {
    base::File key_file(file_path,
                        base::File::FLAG_OPEN | base::File::FLAG_READ);
    if (!key_file.IsValid()) {
      continue;  // Could not open.
    }

    SignedEncryptionInfo signed_encryption_key;
    {
      char key_file_buffer[kEncryptionKeyMaxFileSize];
      const int32_t read_result = key_file.Read(
          /*offset=*/0, key_file_buffer, kEncryptionKeyMaxFileSize);
      if (read_result < 0) {
        LOG(WARNING) << "File read error="
                     << key_file.ErrorToString(key_file.GetLastFileError())
                     << " " << file_path.MaybeAsASCII();
        continue;  // File read error.
      }
      if (read_result == 0 || read_result >= kEncryptionKeyMaxFileSize) {
        continue;  // Unexpected file size.
      }
      google::protobuf::io::ArrayInputStream key_stream(  // Zero-copy stream.
          key_file_buffer, read_result);
      if (!signed_encryption_key.ParseFromZeroCopyStream(&key_stream)) {
        LOG(WARNING) << "Failed to parse key file, full_name='"
                     << file_path.MaybeAsASCII() << "'";
        continue;
      }
    }

    // Parsed successfully. Verify signature of the whole "id"+"key" string.
    const auto signature_verification_status =
        VerifySignature(signed_encryption_key);
    if (!signature_verification_status.ok()) {
      LOG(WARNING) << "Loaded key failed verification, status="
                   << signature_verification_status << ", full_name='"
                   << file_path.MaybeAsASCII() << "'";
      continue;
    }

    // Validated successfully. Return file name and signed key proto.
    return std::make_pair(file_path, signed_encryption_key);
  }

  // Not found, return error.
  return std::nullopt;
}

}  // namespace reporting