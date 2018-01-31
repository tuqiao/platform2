// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/service.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/route.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/vm_sockets.h>  // Needs to come after sys/socket.h

#include <map>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/callback.h>
#include <base/files/file_enumerator.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <base/memory/ptr_util.h>
#include <base/single_thread_task_runner.h>
#include <base/strings/stringprintf.h>
#include <base/synchronization/waitable_event.h>
#include <base/threading/thread_task_runner_handle.h>
#include <base/time/time.h>
#include <base/version.h>
#include <chromeos/dbus/service_constants.h>
#include <crosvm/qcow_utils.h>
#include <vm_concierge/proto_bindings/service.pb.h>

#include "vm_tools/common/constants.h"

using std::string;

namespace vm_tools {
namespace concierge {

namespace {

using Subnet = SubnetPool::Subnet;
using ProcessExitBehavior = VirtualMachine::ProcessExitBehavior;
using ProcessStatus = VirtualMachine::ProcessStatus;

// Path to the runtime directory used by VMs.
constexpr char kRuntimeDir[] = "/run/vm";

// Default path to VM kernel image and rootfs.
constexpr char kVmDefaultPath[] = "/run/imageloader/cros-termina";

// Name of the VM kernel image.
constexpr char kVmKernelName[] = "vm_kernel";

// Name of the VM rootfs image.
constexpr char kVmRootfsName[] = "vm_rootfs.img";

// Maximum number of extra disks to be mounted inside the VM.
constexpr int kMaxExtraDisks = 10;

// How long to wait before timing out on `lxd waitready`.
constexpr base::TimeDelta kLxdWaitreadyTimeout =
    base::TimeDelta::FromSeconds(10);

// How long we should wait for a VM to start up.
constexpr base::TimeDelta kVmStartupTimeout = base::TimeDelta::FromSeconds(5);

// crosvm directory name.
constexpr char kCrosvmDir[] = "crosvm";

// Cryptohome root base path.
constexpr char kCryptohomeRoot[] = "/home/root";

// Cryptohome user base path.
constexpr char kCryptohomeUser[] = "/home/user";

// Downloads directory for a user.
constexpr char kDownloadsDir[] = "Downloads";

// Passes |method_call| to |handler| and passes the response to
// |response_sender|. If |handler| returns NULL, an empty response is created
// and sent.
void HandleSynchronousDBusMethodCall(
    base::Callback<std::unique_ptr<dbus::Response>(dbus::MethodCall*)> handler,
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  std::unique_ptr<dbus::Response> response = handler.Run(method_call);
  if (!response)
    response = dbus::Response::FromMethodCall(method_call);
  response_sender.Run(std::move(response));
}

// Posted to the grpc thread to run the StartupListener service.  Puts a copy
// of the pointer to the grpc server in |server_copy| and then signals |event|.
void RunStartupListenerService(StartupListenerImpl* listener,
                               base::WaitableEvent* event,
                               std::shared_ptr<grpc::Server>* server_copy) {
  // We are not interested in getting SIGCHLD or SIGTERM on this thread.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  // Build the grpc server.
  grpc::ServerBuilder builder;
  builder.AddListeningPort(base::StringPrintf("vsock:%u:%u", VMADDR_CID_ANY,
                                              vm_tools::kStartupListenerPort),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(listener);

  std::shared_ptr<grpc::Server> server(builder.BuildAndStart().release());

  *server_copy = server;
  event->Signal();

  if (server) {
    server->Wait();
  }
}

// Converts an IPv4 address to a string. The result will be stored in |str|
// on success.
bool IPv4AddressToString(const uint32_t address, std::string* str) {
  CHECK(str);

  char result[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &address, result, sizeof(result)) != result) {
    return false;
  }
  *str = std::string(result);
  return true;
}

// Get the path to the latest available cros-termina component.
base::FilePath GetLatestVMPath() {
  base::FilePath component_dir(kVmDefaultPath);
  base::FileEnumerator dir_enum(component_dir, false,
                                base::FileEnumerator::DIRECTORIES);

  base::Version latest_version("0");
  base::FilePath latest_path;

  for (base::FilePath path = dir_enum.Next(); !path.empty();
       path = dir_enum.Next()) {
    base::Version version(path.BaseName().value());
    if (!version.IsValid())
      continue;

    if (version > latest_version) {
      latest_version = version;
      latest_path = path;
    }
  }

  return latest_path;
}

}  // namespace

std::unique_ptr<Service> Service::Create(base::Closure quit_closure) {
  auto service = base::WrapUnique(new Service(std::move(quit_closure)));

  if (!service->Init()) {
    service.reset();
  }

  return service;
}

Service::Service(base::Closure quit_closure)
    : watcher_(FROM_HERE), quit_closure_(std::move(quit_closure)) {}

Service::~Service() {
  if (grpc_server_) {
    grpc_server_->Shutdown();
  }
}

void Service::OnFileCanReadWithoutBlocking(int fd) {
  DCHECK_EQ(signal_fd_.get(), fd);

  struct signalfd_siginfo siginfo;
  if (read(signal_fd_.get(), &siginfo, sizeof(siginfo)) != sizeof(siginfo)) {
    PLOG(ERROR) << "Failed to read from signalfd";
    return;
  }

  if (siginfo.ssi_signo == SIGCHLD) {
    HandleChildExit();
  } else if (siginfo.ssi_signo == SIGTERM) {
    HandleSigterm();
  } else {
    LOG(ERROR) << "Received unknown signal from signal fd: "
               << strsignal(siginfo.ssi_signo);
  }
}

void Service::OnFileCanWriteWithoutBlocking(int fd) {
  NOTREACHED();
}

bool Service::Init() {
  dbus::Bus::Options opts;
  opts.bus_type = dbus::Bus::SYSTEM;
  bus_ = new dbus::Bus(std::move(opts));

  if (!bus_->Connect()) {
    LOG(ERROR) << "Failed to connect to system bus";
    return false;
  }

  exported_object_ =
      bus_->GetExportedObject(dbus::ObjectPath(kVmConciergeServicePath));
  if (!exported_object_) {
    LOG(ERROR) << "Failed to export " << kVmConciergeServicePath << " object";
    return false;
  }

  using ServiceMethod =
      std::unique_ptr<dbus::Response> (Service::*)(dbus::MethodCall*);
  const std::map<const char*, ServiceMethod> kServiceMethods = {
      {kStartVmMethod, &Service::StartVm},
      {kStopVmMethod, &Service::StopVm},
      {kStopAllVmsMethod, &Service::StopAllVms},
      {kGetVmInfoMethod, &Service::GetVmInfo},
      {kCreateDiskImageMethod, &Service::CreateDiskImage},
  };

  for (const auto& iter : kServiceMethods) {
    bool ret = exported_object_->ExportMethodAndBlock(
        kVmConciergeInterface, iter.first,
        base::Bind(&HandleSynchronousDBusMethodCall,
                   base::Bind(iter.second, base::Unretained(this))));
    if (!ret) {
      LOG(ERROR) << "Failed to export method " << iter.first;
      return false;
    }
  }

  if (!bus_->RequestOwnershipAndBlock(kVmConciergeServiceName,
                                      dbus::Bus::REQUIRE_PRIMARY)) {
    LOG(ERROR) << "Failed to take ownership of " << kVmConciergeServiceName;
    return false;
  }

  // Start the grpc thread.
  if (!grpc_thread_.Start()) {
    LOG(ERROR) << "Failed to start grpc thread";
    return false;
  }

  base::WaitableEvent event(false /*manual_reset*/,
                            false /*initially_signaled*/);
  bool ret = grpc_thread_.task_runner()->PostTask(
      FROM_HERE, base::Bind(&RunStartupListenerService, &startup_listener_,
                            &event, &grpc_server_));
  if (!ret) {
    LOG(ERROR) << "Failed to post server startup task to grpc thread";
    return false;
  }

  // Wait for the grpc server to start.
  event.Wait();

  if (!grpc_server_) {
    LOG(ERROR) << "grpc server failed to start";
    return false;
  }

  // Change the umask so that the runtime directory for each VM will get the
  // right permissions.
  umask(002);

  // Set up the signalfd for receiving SIGCHLD and SIGTERM.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGTERM);

  signal_fd_.reset(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!signal_fd_.is_valid()) {
    PLOG(ERROR) << "Failed to create signalfd";
    return false;
  }

  ret = base::MessageLoopForIO::current()->WatchFileDescriptor(
      signal_fd_.get(), true /*persistent*/, base::MessageLoopForIO::WATCH_READ,
      &watcher_, this);
  if (!ret) {
    LOG(ERROR) << "Failed to watch signalfd";
    return false;
  }

  // Now block signals from the normal signal handling path so that we will get
  // them via the signalfd.
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
    PLOG(ERROR) << "Failed to block signals via sigprocmask";
    return false;
  }

  return true;
}

void Service::HandleChildExit() {
  // We can't just rely on the information in the siginfo structure because
  // more than one child may have exited but only one SIGCHLD will be
  // generated.
  while (true) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) {
      if (pid == -1 && errno != ECHILD) {
        PLOG(ERROR) << "Unable to reap child processes";
      }
      break;
    }

    // See if this is a process we launched.
    string name;
    for (const auto& pair : vms_) {
      if (pid == pair.second->pid()) {
        name = pair.first;
        break;
      }
    }

    if (WIFEXITED(status)) {
      LOG(INFO) << " Process " << pid << " exited with status "
                << WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      LOG(INFO) << " Process " << pid << " killed by signal "
                << WTERMSIG(status)
                << (WCOREDUMP(status) ? " (core dumped)" : "");
    } else {
      LOG(WARNING) << "Unknown exit status " << status << " for process "
                   << pid;
    }

    // Remove this process from the our set of VMs.
    vms_.erase(std::move(name));
  }
}

void Service::HandleSigterm() {
  LOG(INFO) << "Shutting down due to SIGTERM";

  base::ThreadTaskRunnerHandle::Get()->PostTask(FROM_HERE, quit_closure_);
}

std::unique_ptr<dbus::Response> Service::StartVm(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Received StartVm request";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  StartVmRequest request;
  StartVmResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse StartVmRequest from message";

    response.set_failure_reason("Unable to parse protobuf");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Make sure the VM has a name.
  if (request.name().empty()) {
    LOG(ERROR) << "Ignoring request with empty name";

    response.set_failure_reason("Missing VM name");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (vms_.find(request.name()) != vms_.end()) {
    LOG(ERROR) << "VM with requested name is already running";

    response.set_failure_reason("VM name is taken");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (request.disks_size() > kMaxExtraDisks) {
    LOG(ERROR) << "Rejecting request with " << request.disks_size()
               << " extra disks";

    response.set_failure_reason("Too many extra disks");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  base::FilePath kernel, rootfs;

  if (request.start_termina()) {
    base::FilePath component_path = GetLatestVMPath();
    if (component_path.empty()) {
      LOG(ERROR) << "Termina component is not loaded";

      response.set_failure_reason("Termina component is not loaded");
      writer.AppendProtoAsArrayOfBytes(response);
      return dbus_response;
    }

    kernel = component_path.Append(kVmKernelName);
    rootfs = component_path.Append(kVmRootfsName);
  } else {
    kernel = base::FilePath(request.vm().kernel());
    rootfs = base::FilePath(request.vm().rootfs());
  }

  if (!base::PathExists(kernel)) {
    LOG(ERROR) << "Missing VM kernel path: " << kernel.value();

    response.set_failure_reason("Kernel path does not exist");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!base::PathExists(rootfs)) {
    LOG(ERROR) << "Missing VM rootfs path: " << rootfs.value();

    response.set_failure_reason("Rootfs path does not exist");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  std::vector<VirtualMachine::Disk> disks;
  for (const auto& disk : request.disks()) {
    if (!base::PathExists(base::FilePath(disk.path()))) {
      LOG(ERROR) << "Missing disk path: " << disk.path();
      response.set_failure_reason("One or more disk paths do not exist");
      writer.AppendProtoAsArrayOfBytes(response);
      return dbus_response;
    }

    VirtualMachine::DiskImageType image_type;
    if (disk.image_type() == vm_tools::concierge::DISK_IMAGE_RAW) {
      image_type = VirtualMachine::DiskImageType::RAW;
    } else if (disk.image_type() == vm_tools::concierge::DISK_IMAGE_QCOW2) {
      image_type = VirtualMachine::DiskImageType::QCOW2;
    } else {
      LOG(ERROR) << "Invalid disk type";
      response.set_failure_reason("Invalid disk type specified");
      writer.AppendProtoAsArrayOfBytes(response);
      return dbus_response;
    }

    disks.emplace_back(VirtualMachine::Disk{
        .path = base::FilePath(disk.path()),
        .writable = disk.writable(),
        .image_type = image_type,
    });
  }

  // Create the runtime directory.
  base::FilePath runtime_dir;
  if (!base::CreateTemporaryDirInDir(base::FilePath(kRuntimeDir), "vm.",
                                     &runtime_dir)) {
    PLOG(ERROR) << "Unable to create runtime directory for VM";

    response.set_failure_reason(
        "Internal error: unable to create runtime directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Allocate resources for the VM.
  MacAddress mac_address = mac_address_generator_.Generate();
  std::unique_ptr<Subnet> subnet = subnet_pool_.AllocateVM();
  if (!subnet) {
    LOG(ERROR) << "No available subnets; unable to start VM";

    response.set_failure_reason("No available subnets");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }
  uint32_t vsock_cid = vsock_cid_pool_.Allocate();

  // Associate a WaitableEvent with this VM.  This needs to happen before
  // starting the VM to avoid a race where the VM reports that it's ready
  // before it gets added as a pending VM.
  base::WaitableEvent event(false /*manual_reset*/,
                            false /*initially_signaled*/);
  startup_listener_.AddPendingVm(vsock_cid, &event);

  // Start the VM and build the response.
  auto vm = VirtualMachine::Create(std::move(kernel), std::move(rootfs),
                                   std::move(disks), std::move(mac_address),
                                   std::move(subnet), vsock_cid,
                                   std::move(runtime_dir));
  if (!vm) {
    LOG(ERROR) << "Unable to start VM";

    startup_listener_.RemovePendingVm(vsock_cid);
    response.set_failure_reason("Unable to start VM");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Wait for the VM to finish starting up and for maitre'd to signal that it's
  // ready.
  if (!event.TimedWait(kVmStartupTimeout)) {
    LOG(ERROR) << "VM failed to start in " << kVmStartupTimeout.InSeconds()
               << " seconds";

    response.set_failure_reason("VM failed to start in time");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // maitre'd is ready.  Finish setting up the VM.
  if (!vm->ConfigureNetwork()) {
    LOG(ERROR) << "Failed to configure VM network";

    response.set_failure_reason("Failed to configure VM network");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Do all the mounts.  Assume that the rootfs filesystem was assigned
  // /dev/vda and that every subsequent image was assigned a letter in
  // alphabetical order starting from 'b'.
  unsigned char disk_letter = 'b';
  unsigned char offset = 0;
  for (const auto& disk : request.disks()) {
    string src = base::StringPrintf("/dev/vd%c", disk_letter + offset);
    ++offset;

    if (!disk.do_mount())
      continue;

    uint64_t flags = disk.flags();
    if (!disk.writable()) {
      flags |= MS_RDONLY;
    }
    if (!vm->Mount(std::move(src), disk.mount_point(), disk.fstype(), flags,
                   disk.data())) {
      LOG(ERROR) << "Failed to mount " << disk.path() << " -> "
                 << disk.mount_point();

      response.set_failure_reason("Failed to mount extra disk");
      writer.AppendProtoAsArrayOfBytes(response);
      return dbus_response;
    }
  }

  // If at least one extra disk was given, assume that one of them was a
  // container disk image mounted to /mnt/container_rootfs.  Try to start it
  // with run_oci.  TODO: Remove this once all the lxc/lxd stuff is ready.
  if (request.disks_size() > 0) {
    std::vector<string> run_oci = {
        "run_oci",
        "run",
        "--cgroup_parent=chronos_containers",
        "--container_path=/mnt/container_rootfs",
        "termina_container",
    };
    if (!vm->StartProcess(std::move(run_oci), {},
                          ProcessExitBehavior::ONE_SHOT)) {
      LOG(WARNING) << "run_oci did not launch successfully";
    }
  }
  string failure_reason;
  if (request.start_termina() && !StartTermina(vm.get(), &failure_reason)) {
    response.set_failure_reason(std::move(failure_reason));
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  LOG(INFO) << "Started VM with pid " << vm->pid();

  VmInfo* vm_info = response.mutable_vm_info();
  response.set_success(true);
  vm_info->set_ipv4_address(vm->IPv4Address());
  vm_info->set_pid(vm->pid());
  vm_info->set_cid(vsock_cid);
  writer.AppendProtoAsArrayOfBytes(response);

  vms_[request.name()] = std::move(vm);

  return dbus_response;
}

std::unique_ptr<dbus::Response> Service::StopVm(dbus::MethodCall* method_call) {
  LOG(INFO) << "Received StopVm request";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  StopVmRequest request;
  StopVmResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse StopVmRequest from message";

    response.set_failure_reason("Unable to parse protobuf");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  auto iter = vms_.find(request.name());
  if (iter == vms_.end()) {
    LOG(ERROR) << "Requested VM does not exist";

    response.set_failure_reason("Requested VM does not exist");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!iter->second->Shutdown()) {
    LOG(ERROR) << "Unable to shut down VM";

    response.set_failure_reason("Unable to shut down VM");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  vms_.erase(iter);
  response.set_success(true);
  writer.AppendProtoAsArrayOfBytes(response);

  return dbus_response;
}

std::unique_ptr<dbus::Response> Service::StopAllVms(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Received StopAllVms request";

  std::vector<std::thread> threads;
  threads.reserve(vms_.size());

  // Spawn a thread for each VM to shut it down.
  for (auto& pair : vms_) {
    // By resetting the unique_ptr, each thread calls the destructor for that
    // VM, which will shut it down.
    threads.emplace_back([](std::unique_ptr<VirtualMachine> vm) { vm.reset(); },
                         std::move(pair.second));
  }

  // Wait for all VMs to shutdown.
  for (auto& thread : threads) {
    thread.join();
  }

  vms_.clear();

  return nullptr;
}

std::unique_ptr<dbus::Response> Service::GetVmInfo(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Received GetVmInfo request";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  GetVmInfoRequest request;
  GetVmInfoResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse GetVmInfoRequest from message";

    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  auto iter = vms_.find(request.name());
  if (iter == vms_.end()) {
    LOG(ERROR) << "Requested VM does not exist";

    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  auto& vm = iter->second;
  VmInfo* vm_info = response.mutable_vm_info();
  vm_info->set_ipv4_address(vm->IPv4Address());
  vm_info->set_pid(vm->pid());
  vm_info->set_cid(vm->cid());

  response.set_success(true);
  writer.AppendProtoAsArrayOfBytes(response);

  return dbus_response;
}

bool Service::StartTermina(VirtualMachine* vm, string* failure_reason) {
  LOG(INFO) << "Starting lxd";

  // Common environment for all LXD functionality.
  std::map<string, string> lxd_env = {{"LXD_DIR", "/mnt/stateful/lxd"},
                                      {"LXD_CONF", "/mnt/stateful/lxd_conf"}};

  // Set up the stateful disk. This will format the disk if necessary, then
  // mount it.
  if (!vm->RunProcess({"stateful_setup.sh"}, lxd_env)) {
    LOG(ERROR) << "Stateful setup failed";
    *failure_reason = "stateful setup failed";
    return false;
  }

  // Launch the main lxd process.
  if (!vm->StartProcess({"lxd", "--group", "lxd"}, lxd_env,
                        ProcessExitBehavior::RESPAWN_ON_EXIT)) {
    LOG(ERROR) << "lxd failed to start";
    *failure_reason = "lxd failed to start";
    return false;
  }

  // Wait for lxd to be ready. The first start may take a few seconds, so use
  // a longer timeout than the default.
  if (!vm->RunProcessWithTimeout({"lxd", "waitready"}, lxd_env,
                                 kLxdWaitreadyTimeout)) {
    LOG(ERROR) << "lxd waitready failed";
    *failure_reason = "lxd waitready failed";
    return false;
  }

  // Perform any setup for lxd to be usable. On first run, this sets up the
  // lxd configuration (network bridge, storage pool, etc).
  if (!vm->RunProcess({"lxd_setup.sh"}, lxd_env)) {
    LOG(ERROR) << "lxd setup failed";
    *failure_reason = "lxd setup failed";
    return false;
  }

  // Allocate the subnet for lxd's bridge to use.
  std::unique_ptr<SubnetPool::Subnet> container_subnet =
      subnet_pool_.AllocateContainer();
  if (!container_subnet) {
    LOG(ERROR) << "Could not allocate container subnet";
    *failure_reason = "could not allocate container subnet";
    return false;
  }
  vm->SetContainerSubnet(std::move(container_subnet));

  // Set up a route for the container using the VM as a gateway.
  uint32_t container_gateway_addr = vm->IPv4Address();
  uint32_t container_netmask = vm->ContainerNetmask();
  uint32_t container_subnet_addr = vm->ContainerSubnet();

  struct rtentry route;
  memset(&route, 0, sizeof(route));

  struct sockaddr_in* gateway =
      reinterpret_cast<struct sockaddr_in*>(&route.rt_gateway);
  gateway->sin_family = AF_INET;
  gateway->sin_addr.s_addr = static_cast<in_addr_t>(container_gateway_addr);

  struct sockaddr_in* dst =
      reinterpret_cast<struct sockaddr_in*>(&route.rt_dst);
  dst->sin_family = AF_INET;
  dst->sin_addr.s_addr = (container_subnet_addr & container_netmask);

  struct sockaddr_in* genmask =
      reinterpret_cast<struct sockaddr_in*>(&route.rt_genmask);
  genmask->sin_family = AF_INET;
  genmask->sin_addr.s_addr = container_netmask;

  route.rt_flags = RTF_UP | RTF_GATEWAY;

  base::ScopedFD fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (!fd.is_valid()) {
    PLOG(ERROR) << "Failed to create socket";
    *failure_reason = "failed to create socket";
    return false;
  }

  if (HANDLE_EINTR(ioctl(fd.get(), SIOCADDRT, &route)) != 0) {
    PLOG(ERROR) << "Failed to set route for container";
    *failure_reason = "failed to set route for container";
    return false;
  }

  std::string dst_addr;
  IPv4AddressToString(container_subnet_addr, &dst_addr);
  size_t prefix = vm->ContainerPrefix();

  // The route has been installed on the host, so inform lxd of its subnet.
  std::string container_subnet_cidr =
      base::StringPrintf("%s/%zu", dst_addr.c_str(), prefix);
  if (!vm->RunProcess({"lxc", "network", "set", "lxdbr0", "ipv4.address",
                       std::move(container_subnet_cidr)},
                      lxd_env)) {
    LOG(ERROR) << "lxc network config failed";
    *failure_reason = "lxc network config failed";
    return false;
  }

  return true;
}

std::unique_ptr<dbus::Response> Service::CreateDiskImage(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Received CreateDiskImage request";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  CreateDiskImageRequest request;
  CreateDiskImageResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse CreateDiskImageRequest from message";
    response.set_status(DISK_STATUS_FAILED);
    response.set_failure_reason("Unable to parse CreateImageDiskRequest");

    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  base::FilePath disk_path;
  if (request.storage_location() == STORAGE_CRYPTOHOME_ROOT) {
    base::FilePath crosvm_dir = base::FilePath(kCryptohomeRoot)
                                    .Append(request.cryptohome_id())
                                    .Append(kCrosvmDir);
    base::File::Error dir_error;
    if (!base::DirectoryExists(crosvm_dir) &&
        !base::CreateDirectoryAndGetError(crosvm_dir, &dir_error)) {
      string error_description = base::File::ErrorToString(dir_error);
      LOG(ERROR) << "Failed to create crosvm directory in /home/root: "
                 << error_description;
      response.set_status(DISK_STATUS_FAILED);
      response.set_failure_reason(
          "Failed to create crosvm directory in /home/root: " +
          error_description);
      writer.AppendProtoAsArrayOfBytes(response);

      return dbus_response;
    }
    disk_path = crosvm_dir.Append(request.disk_path());
  } else if (request.storage_location() == STORAGE_CRYPTOHOME_DOWNLOADS) {
    disk_path = base::FilePath(kCryptohomeUser)
                    .Append(request.cryptohome_id())
                    .Append(kDownloadsDir)
                    .Append(request.disk_path());
  } else {
    LOG(ERROR) << "Unknown storage location type";
    response.set_status(DISK_STATUS_FAILED);
    response.set_failure_reason("Unknown storage location type");
    writer.AppendProtoAsArrayOfBytes(response);

    return dbus_response;
  }

  if (disk_path.ReferencesParent()) {
    LOG(ERROR) << "Disk path references parent";
    response.set_status(DISK_STATUS_FAILED);
    response.set_failure_reason("Disk path references parent");
    writer.AppendProtoAsArrayOfBytes(response);

    return dbus_response;
  }

  if (base::PathExists(disk_path)) {
    response.set_status(DISK_STATUS_EXISTS);
    response.set_disk_path(disk_path.value());
    writer.AppendProtoAsArrayOfBytes(response);

    return dbus_response;
  }

  if (request.image_type() == DISK_IMAGE_RAW) {
    LOG(INFO) << "Creating raw disk at: " << disk_path.value() << " size "
              << request.disk_size();
    base::ScopedFD fd(
        open(disk_path.value().c_str(), O_CREAT | O_NONBLOCK | O_WRONLY, 0600));
    if (!fd.is_valid()) {
      PLOG(ERROR) << "Failed to create raw disk";
      response.set_status(DISK_STATUS_FAILED);
      response.set_failure_reason("Failed to create raw disk file");
      writer.AppendProtoAsArrayOfBytes(response);

      return dbus_response;
    }

    int ret = ftruncate(fd.get(), request.disk_size());
    if (ret != 0) {
      PLOG(ERROR) << "Failed to truncate raw disk";
      response.set_status(DISK_STATUS_FAILED);
      response.set_failure_reason("Failed to truncate raw disk file");
      writer.AppendProtoAsArrayOfBytes(response);

      return dbus_response;
    }
    response.set_status(DISK_STATUS_CREATED);
    response.set_disk_path(disk_path.value());
    writer.AppendProtoAsArrayOfBytes(response);

    return dbus_response;
  }

  LOG(INFO) << "Creating qcow2 disk at: " << disk_path.value() << " size "
            << request.disk_size();
  int ret =
      create_qcow_with_size(disk_path.value().c_str(), request.disk_size());
  if (ret != 0) {
    LOG(ERROR) << "Failed to create qcow2 disk image: " << strerror(ret);
    response.set_status(DISK_STATUS_FAILED);
    response.set_failure_reason("Failed to create qcow2 disk image");
    writer.AppendProtoAsArrayOfBytes(response);

    return dbus_response;
  }

  response.set_disk_path(disk_path.value());
  response.set_status(DISK_STATUS_CREATED);
  writer.AppendProtoAsArrayOfBytes(response);

  return dbus_response;
}
}  // namespace concierge
}  // namespace vm_tools
