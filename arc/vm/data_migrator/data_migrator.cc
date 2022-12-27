// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/mount.h>

#include <arcvm_data_migrator/proto_bindings/arcvm_data_migrator.pb.h>
#include <base/bind.h>
#include <base/command_line.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <base/threading/thread.h>
#include <brillo/blkdev_utils/loop_device.h>
#include <brillo/cryptohome.h>
#include <brillo/daemons/dbus_daemon.h>
#include <brillo/syslog_logging.h>
#include <chromeos/dbus/service_constants.h>
#include <cryptohome/data_migrator/migration_helper.h>
#include <cryptohome/platform.h>
#include <dbus/bus.h>

#include "arc/vm/data_migrator/arcvm_data_migration_helper_delegate.h"
#include "arc/vm/data_migrator/dbus_adaptors/org.chromium.ArcVmDataMigrator.h"

namespace {

// The mount point for the migration destinaiton.
constexpr char kDestinationMountPoint[] = "/tmp/arcvm-data-migration-mount";

class DBusAdaptor : public org::chromium::ArcVmDataMigratorAdaptor,
                    public org::chromium::ArcVmDataMigratorInterface {
 public:
  explicit DBusAdaptor(scoped_refptr<dbus::Bus> bus)
      : org::chromium::ArcVmDataMigratorAdaptor(this),
        dbus_object_(nullptr, bus, GetObjectPath()) {
    exported_object_ = bus->GetExportedObject(
        dbus::ObjectPath(arc::data_migrator::kArcVmDataMigratorServicePath));
  }

  ~DBusAdaptor() override {
    // TODO(momohatt): Cancel migration running on migration_thread_.

    CleanupMount();
  }

  DBusAdaptor(const DBusAdaptor&) = delete;
  DBusAdaptor& operator=(const DBusAdaptor&) = delete;

  // Registers the D-Bus object and interfaces.
  void RegisterAsync(
      brillo::dbus_utils::AsyncEventSequencer::CompletionAction cb) {
    RegisterWithDBusObject(&dbus_object_);
    dbus_object_.RegisterAsync(std::move(cb));
  }

  // org::chromium::ArcVmDataMigratorInterface overrides:
  bool StartMigration(
      brillo::ErrorPtr* error,
      const arc::data_migrator::StartMigrationRequest& request) override {
    const base::FilePath user_root_dir =
        brillo::cryptohome::home::GetRootPath(request.username());
    const base::FilePath android_data_dir =
        user_root_dir.Append("android-data");
    const base::FilePath source_dir = android_data_dir.Append("data");

    base::FilePath destination_disk;
    switch (request.destination_type()) {
      case arc::data_migrator::CROSVM_DISK: {
        // Disk path /home/root/<hash>/crosvm/YXJjdm0=.img is constructed in
        // concierge's CreateDiskImage method. Image name YXJjdm0=.img is static
        // because it is generated by vm_tools::GetEncodedName("arcvm").
        destination_disk = user_root_dir.Append("crosvm/YXJjdm0=.img");
        break;
      }
      case arc::data_migrator::LVM_DEVICE: {
        const std::string user_hash =
            brillo::cryptohome::home::SanitizeUserName(request.username());
        // The volume path is constructed using
        // cryptohome::DmcryptVolumePrefix().
        destination_disk = base::FilePath(base::StringPrintf(
            "/dev/mapper/vm/dmcrypt-%s-arcvm", user_hash.substr(0, 8).c_str()));
        break;
      }
      default:
        NOTREACHED();
    }

    // The mount point will be automatically removed when the upstart job stops
    // since it is created under /tmp where tmpfs is mounted.
    if (!base::CreateDirectory(base::FilePath(kDestinationMountPoint))) {
      PLOG(ERROR) << "Failed to create destination mount point "
                  << kDestinationMountPoint;
      return false;
    }

    loop_device_manager_ = std::make_unique<brillo::LoopDeviceManager>();
    loop_device_ = loop_device_manager_->AttachDeviceToFile(destination_disk);
    if (!loop_device_->IsValid()) {
      PLOG(ERROR) << "Failed to attach a loop device";
      CleanupMount();
      return false;
    }

    if (mount(loop_device_->GetDevicePath().value().c_str(),
              kDestinationMountPoint, "ext4", 0, "")) {
      PLOG(ERROR) << "Failed to mount the loop device";
      CleanupMount();
      return false;
    }
    mounted_ = true;

    // Unretained is safe to use here because |migration_thread_| will be joined
    // on the destruction of |this|.
    auto migrate = base::BindOnce(&DBusAdaptor::Migrate, base::Unretained(this),
                                  source_dir, android_data_dir);
    migration_thread_ = std::make_unique<base::Thread>("migration_helper");
    migration_thread_->Start();
    migration_thread_->task_runner()->PostTask(FROM_HERE, std::move(migrate));

    return true;
  }

  // TODO(momohatt): Add StopMigration as a D-Bus method?

 private:
  void Migrate(const base::FilePath& source_dir,
               const base::FilePath& status_files_dir) {
    cryptohome::Platform platform;
    arc::ArcVmDataMigrationHelperDelegate delegate;
    constexpr uint64_t kMaxChunkSize = 128 * 1024 * 1024;

    cryptohome::data_migrator::MigrationHelper migration_helper(
        &platform, &delegate, source_dir,
        base::FilePath(kDestinationMountPoint), status_files_dir,
        kMaxChunkSize);
    // Unretained is safe to use here because this method (DBusAdaptor::Migrate)
    // runs on |migration_thread_| which is joined on the destruction on |this|.
    bool success = migration_helper.Migrate(base::BindRepeating(
        &DBusAdaptor::MigrationHelperCallback, base::Unretained(this)));

    arc::data_migrator::DataMigrationProgress progress;
    if (success) {
      progress.set_status(arc::data_migrator::DATA_MIGRATION_SUCCESS);
    } else {
      progress.set_status(arc::data_migrator::DATA_MIGRATION_FAILED);
    }
    SendMigrationProgressSignal(progress);

    CleanupMount();
  }

  void MigrationHelperCallback(uint64_t current_bytes, uint64_t total_bytes) {
    arc::data_migrator::DataMigrationProgress progress_to_send;
    if (total_bytes == 0) {
      // Ignore the callback when MigrationHelper is still initializing.
      return;
    }
    progress_to_send.set_status(arc::data_migrator::DATA_MIGRATION_IN_PROGRESS);
    progress_to_send.set_current_bytes(current_bytes);
    progress_to_send.set_total_bytes(total_bytes);
    SendMigrationProgressSignal(progress_to_send);
  }

  void SendMigrationProgressSignal(
      const arc::data_migrator::DataMigrationProgress& progress) {
    dbus::Signal signal(arc::data_migrator::kArcVmDataMigratorInterface,
                        arc::data_migrator::kMigrationProgressSignal);
    dbus::MessageWriter writer(&signal);
    writer.AppendProtoAsArrayOfBytes(progress);

    exported_object_->SendSignal(&signal);
  }

  void CleanupMount() {
    if (mounted_) {
      PLOG_IF(ERROR, umount(kDestinationMountPoint))
          << "Failed to unmount the loop device from "
          << kDestinationMountPoint;
      mounted_ = false;
    }
    if (loop_device_) {
      PLOG_IF(ERROR, !loop_device_->Detach()) << "Failed to detach loop device";
      loop_device_.reset();
    }
  }

  // Set to true if the migration destination has been mounted on host.
  bool mounted_ = false;

  std::unique_ptr<brillo::LoopDevice> loop_device_;
  std::unique_ptr<brillo::LoopDeviceManager> loop_device_manager_;

  std::unique_ptr<base::Thread> migration_thread_;
  brillo::dbus_utils::DBusObject dbus_object_;
  dbus::ExportedObject* exported_object_;  // Owned by the Bus object
};

class Daemon : public brillo::DBusServiceDaemon {
 public:
  Daemon()
      : DBusServiceDaemon(arc::data_migrator::kArcVmDataMigratorServiceName) {}
  ~Daemon() override = default;

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;

 protected:
  void RegisterDBusObjectsAsync(
      brillo::dbus_utils::AsyncEventSequencer* sequencer) override {
    adaptor_ = std::make_unique<DBusAdaptor>(bus_);
    adaptor_->RegisterAsync(
        sequencer->GetHandler("RegisterAsync() failed.", true));
  }

 private:
  std::unique_ptr<DBusAdaptor> adaptor_;
};

}  // namespace

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);

  return Daemon().Run();
}
