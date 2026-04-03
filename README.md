# OS-Jackfruit

## Multi-Container Runtime and Memory Monitor

This project implements a lightweight container runtime that can supervise multiple containers concurrently and enforce per‑container memory limits using a Linux kernel module. The runtime is written in C and runs on stock Ubuntu kernels (22.04/24.04) inside a virtual machine. It demonstrates process isolation via namespaces, safe producer/consumer logging with a bounded buffer, an IPC channel for control commands, kernel‑space memory monitoring, and basic scheduling experiments.

Team Information
•	Member 1 – SRN:  PES2UG25AM809
•	Member 2 – SRN: 

Build, Load, and Run Instructions
All commands below assume you are inside the boilerplate/ directory of your cloned repository. These instructions have been tested on a fresh Ubuntu 22.04/24.04 virtual machine with Secure Boot disabled.
 
## Prerequisites
1.	Ubuntu VM – Use a virtual machine running Ubuntu 22.04 or 24.04. Do not use WSL; the kernel module will not load there. Make sure Secure Boot is turned off so that out‑of‑tree kernel modules can be loaded.
   
2.	Packages – Install the C compiler and kernel headers:
 	sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

3.	Environment check – Run the provided preflight script to verify that your VM meets the project requirements:
 	chmod +x environment-check.sh
sudo ./environment-check.sh

## Building the Runtime and Kernel Module
Compile the user‑space runtime (engine) and all workload programs, and build the kernel module (monitor.ko) in one step:
make
The user‑space only build used by CI can be invoked as:
make ci

## Preparing the Root Filesystem
Containers must run inside their own root filesystem to achieve proper isolation. We use Alpine’s minimal rootfs as a template. Choose the appropriate architecture (e.g., aarch64 for ARM‑based VMs or x86_64 for Intel/AMD VMs):

# Create a base rootfs directory
mkdir rootfs-base

# Download the Alpine minirootfs for your architecture
# For AArch64/ARM hosts:
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
# For x86_64 hosts, use the x86_64 tarball instead
# wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

sudo tar -xzf alpine-minirootfs-3.20.3-*.tar.gz -C rootfs-base

# Copy the workload binaries into the base rootfs so containers can execute them
sudo cp cpu_hog io_pulse memory_hog rootfs-base/


⚠️ Important: Each live container must have its own writable copy of the rootfs. Do not run two containers against the same directory. Before starting a container, make a copy for each container you plan to run:
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-mem
cp -a rootfs-base rootfs-cpu
cp -a rootfs-base rootfs-io

## Loading the Kernel Module
Insert the kernel module and verify that the device file /dev/container_monitor appears:
sudo insmod monitor.ko
ls -l /dev/container_monitor

## Starting the Supervisor
Launch the supervisor (long‑running daemon) pointing at a base rootfs. This command does not start any containers; it simply prepares the logging infrastructure and command socket:
sudo ./engine supervisor ./rootfs-base
Leave this terminal running. The supervisor prints nothing by default but listens on /tmp/mini_runtime.sock for control requests.

## Starting and Managing Containers
Open a second terminal in the same directory to act as the CLI client. Each command spawns a short‑lived process that connects to the supervisor over the UNIX domain socket, sends a request, prints the response, and exits.
•	Start a container in the background (assigns default limits of 40 MiB soft / 64 MiB hard):
 	sudo ./engine start alpha ./rootfs-alpha "/bin/sh -c 'echo alpha alive; sleep 1000'"
sudo ./engine start beta  ./rootfs-beta  "/bin/sh -c 'echo beta alive;  sleep 1000'"
•	Run a container in the foreground (waits for completion; returns exit code):
 	sudo ./engine run test ./rootfs-alpha "/bin/sh -c 'echo hello inside test; exit 42'"
# Prints: Container test exited with status 42
•	List containers and inspect metadata:
 	sudo ./engine ps
 	The output contains one row per container with the fields: ID, host PID, state, soft limit (MiB), hard limit (MiB), and start time.
•	Read logs from a container:
 	sudo ./engine logs alpha
•	Stop a container gracefully:
 	sudo ./engine stop alpha
# Prints: Sent stop signal to alpha
 	The state in ps will eventually change to stopped once the process exits.
  
## Running the Memory Hog Test
The memory_hog workload repeatedly allocates and touches memory to grow its RSS. Use it to exercise soft and hard limit enforcement:
sudo ./engine start mem3 ./rootfs-mem "/bin/sh -c '/memory_hog 4 500'" --soft-mib 16 --hard-mib 32
Monitor the kernel logs with dmesg. When the process RSS crosses 16 MiB, the kernel module emits a soft‑limit warning. If it continues to 32 MiB, the module sends SIGKILL and the supervisor marks the container as killed.

## Running the Scheduling Experiments
The cpu_hog and io_pulse programs stress different scheduler paths. Use them concurrently to observe how Linux schedules CPU‑bound versus I/O‑bound workloads:
sudo ./engine start cpu1 ./rootfs-cpu "/bin/sh -c '/cpu_hog 20'"
sudo ./engine start io1  ./rootfs-io  "/bin/sh -c '/io_pulse 20 100'"
After a few seconds run ps and inspect the logs. The CPU‑bound container continues printing “cpu_hog alive elapsed=…” while the I/O‑bound container finishes quickly. Repeat with different --nice values (e.g., --nice -5 for higher priority, --nice 10 for lower) to see how the scheduler weights CPU usage.

## Shutdown and Cleanup
1.	Stop any running containers using engine stop <id>.
2.	Press Ctrl+C in the supervisor terminal to exit the daemon cleanly.
3.	Unload the kernel module:
 	sudo rmmod monitor
4.	Remove the UNIX socket and logs directory if needed:
 	sudo rm -f /tmp/mini_runtime.sock
rm -rf logs

 	
## Demo Screenshots

1.	Multi‑container supervision – show two or more containers running concurrently under one supervisor (e.g., alpha and beta). 


   ![WhatsApp Image 2026-03-31 at 10 19 55 PM](https://github.com/user-attachments/assets/13a3afe7-59fd-41cc-8053-d210d01eb714)

                        Screenshot: screenshots/01_multi_container.png





2.	Metadata tracking – run engine ps and display the table of container IDs, PIDs, states, limits, and start times.   



   ![WhatsApp Image 2026-03-31 at 10 19 55 PM](https://github.com/user-attachments/assets/e13b5e92-c892-45fc-a199-4f7d74e37f93)

                    Screenshot: screenshots/02_ps_output.png







3.	Bounded‑buffer logging – display logs from two containers and explain that output flows through the bounded buffer from producer threads to a consumer thread.       

 

![WhatsApp Image 2026-03-31 at 10 20 58 PM](https://github.com/user-attachments/assets/50a86bce-7a4e-4c4b-9229-215f94271d61)

                        Screenshot: screenshots/03_logging.png







4.	CLI and IPC – show a CLI command (such as engine stop beta) and the supervisor response (Sent stop signal to beta).   


![WhatsApp Image 2026-03-31 at 10 27 59 PM](https://github.com/user-attachments/assets/81d465d7-6622-49a7-9c4e-8616ca1438c8)

 

Screenshot: screenshots/04_cli_ipc.png







5.	Soft‑limit warning – capture dmesg output when a container first exceeds its soft limit.   


![WhatsApp Image 2026-03-31 at 10 23 48 PM](https://github.com/user-attachments/assets/175997e0-bf04-4621-9912-c031e09cad75)

Screenshot: screenshots/05_soft_limit.png








6.	Hard‑limit enforcement – capture dmesg output and ps showing a container being killed after crossing its hard limit.       

![WhatsApp Image 2026-03-31 at 10 23 22 PM](https://github.com/user-attachments/assets/9493a34b-de1c-42da-b3b4-36e4b8b43655)



Screenshot: screenshots/06_hard_limit.png






7.	Scheduling experiment – show logs and/or timing results comparing CPU‑bound and I/O‑bound workloads (and nice values if applicable). A simple table of wall‑clock duration vs. configuration is sufficient. 



  




![WhatsApp Image 2026-03-31 at 10 25 29 PM](https://github.com/user-attachments/assets/1c24ee7f-d658-489d-8d29-638970e51640)

![WhatsApp Image 2026-03-31 at 10 25 49 PM](https://github.com/user-attachments/assets/e005d33e-d459-43aa-84e1-57d766ded3d8)



     Screenshot: screenshots/07_scheduling.png

8.	Clean teardown – show ps aux | grep defunct returning no zombies and lsmod | grep monitor returning nothing after unloading the module. 

 
![WhatsApp Image 2026-04-01 at 12 41 40 AM](https://github.com/user-attachments/assets/ee8bddce-fb1a-4431-b90b-1f0ddb575ef4)


        Screenshot: screenshots/08_teardown.png




## Engineering Analysis
Isolation Mechanisms
The runtime isolates containers using Linux namespaces and chroot:
•	PID namespace (CLONE_NEWPID) – gives each container its own process numbering; processes inside cannot see host PIDs. The initial child becomes PID 1 inside the namespace.
•	UTS namespace (CLONE_NEWUTS) – allows each container to have its own hostname (unused here but isolates the UTS view).
•	Mount namespace (CLONE_NEWNS) – provides a separate view of the filesystem mount table. We mount /proc inside the container so that tools like ps and top work.
•	chroot – changes the container’s root directory to its own copy of the Alpine filesystem. This prevents escape via .. traversal. For stronger isolation you could use pivot_root, but chroot suffices since each container uses its own rootfs copy and no additional mounts are shared.
Namespaces isolate only kernel resources; they do not protect the kernel itself. The host kernel remains shared by all containers.

## Supervisor and Process Lifecycle
The supervisor process starts once and lives until explicitly terminated. It maintains a linked list of container_record_t structs, each storing the container ID, host PID, memory limits, state, start time, log path, and exit status. Container creation uses clone() with the namespace flags. Child processes inherit file descriptors, but we explicitly redirect stdout and stderr to a pipe used for logging.
Signal handling is delegated to a dedicated thread that synchronously waits for SIGCHLD, SIGINT, and SIGTERM. On SIGCHLD it reaps all exited children via waitpid(-1, WNOHANG), updates the corresponding record, closes the log file descriptor, unregisters the PID from the kernel monitor, and broadcasts a condition variable to wake any thread waiting on that record. On SIGINT/SIGTERM it marks all running containers as stop_requested, sends them SIGTERM, and breaks out of the accept loop so the supervisor can shut down.
IPC, Threads, and Synchronization
Two separate IPC channels are used:
•	Logging pipeline (producer/consumer) – each container’s stdout/stderr is connected to the supervisor via a pipe. A producer thread reads from the pipe and pushes log chunks into a bounded buffer protected by a mutex and condition variables. A single consumer thread pops items from the buffer and writes them to per‑container log files. This design decouples log ingestion from disk I/O and prevents blocking the container on disk writes.
•	Control channel – a UNIX domain socket at /tmp/mini_runtime.sock handles control requests (start, run, ps, logs, stop). Each client is a short‑lived process that connects, sends a control_request_t, waits for a control_response_t, and disconnects. The supervisor’s accept loop spawns a detached thread per client.
Concurrent access to shared data structures is synchronized appropriately:
•	The bounded buffer uses a mutex and two condition variables. Producers wait on not_full when the buffer is full; the consumer waits on not_empty when empty. A shutting_down flag tells both to exit cleanly.
•	Container metadata is protected by a pthread_mutex_t. The supervisor holds this lock while modifying the list or reading container records. Signal handlers and client threads coordinate via this mutex and a per‑record condition variable.

## Memory Management and Enforcement
Resident Set Size (RSS) measures the amount of physical memory currently used by a process. The kernel module tracks each registered PID’s RSS using get_mm_rss(). Soft limits provide an early warning; the module logs a SOFT LIMIT event once when RSS exceeds the soft threshold. Hard limits are enforced by calling send_sig(SIGKILL) when RSS exceeds the hard threshold. The module removes killed or exited entries immediately to avoid use‑after‑free. A spinlock is used because the timer callback runs in soft‑IRQ context and cannot sleep; holding a mutex there would be illegal. The spinlock protects the global list during insertions, deletions, and iteration.

## Scheduling Behavior
cpu_hog is a tight loop that burns CPU and prints once per second. io_pulse writes small bursts to a temporary file and sleeps between bursts. Running them concurrently shows that the Linux scheduler tends to give more CPU to the I/O‑bound process because it sleeps often, allowing other runnable tasks to run. The CPU‑bound workload continues printing “alive” messages while the I/O workload finishes its 20 iterations quickly. Varying the nice value demonstrates priority effects: a lower nice (e.g., --nice -5) gives the CPU‑bound container more CPU time, while a higher nice (e.g., --nice 10) yields CPU time to other processes.

## Design Decisions and Tradeoffs
•	IPC choice – We used a UNIX domain socket for the control plane because it provides a reliable, stream‑oriented API with authentication via file permissions, unlike FIFOs or shared memory which require more manual framing and synchronization.
•	Spinlock vs mutex in the kernel – The kernel timer callback runs in soft‑interrupt context, so it cannot sleep; therefore a spinlock is necessary to protect the monitored list. Mutexes would cause deadlock if held in interrupt context. In user space we use mutexes because the threads operate in process context and can block safely.
•	chroot vs pivot_root – We chose chroot() to simplify implementation. Because each container uses its own rootfs copy, escaping via .. is not a concern. pivot_root() could further prevent breakouts, but it requires additional mounts and cleanup.
•	Single logger thread – A single consumer thread writing logs is sufficient for the small buffer size and workload. Multiple consumers would add complexity around ordering of log entries and require more synchronization.
•	Limit enforcement in kernel – Enforcing hard limits in kernel space avoids races between user‑space sampling and enforcement. User‑space only enforcement could miss transient spikes or allow evasion; the kernel can reliably kill the process as 

## Scheduler Experiment Results
We ran several scheduling experiments using cpu_hog and io_pulse. Each workload was run to completion inside a container, and we measured the wall‑clock runtime by reading the timestamps from the log files. 

The table below summarises typical results (times in seconds):


## Results

| Experiment                         | Nice values | CPU runtime (s) | I/O runtime (s) | Observations                                                                                                                         |
|------------------------------------|-------------|----------------:|----------------:|----------------------------------------------------
| CPU vs I/O (default nice)          | 0 / 0       |              20 |            ~0.4 | The I/O-bound `io_pulse` finished its 20 iterations very quickly because it frequently sleeps, giving up the CPU. `cpu_hog` kept the CPU busy for the full duration. |
| CPU vs I/O (nice -5 / +10)         | -5 / 10     |              20 |            ~0.3 | Giving the CPU-bound task a higher priority (`nice -5`) did not meaningfully reduce its runtime, but slightly reduced the already short I/O runtime. |
| Dual CPU hogs (nice 0 / -5)        | 0 / -5      |        ~20 / 18 |             N/A | Running two CPU hogs concurrently showed the higher-priority task finishing sooner (18 s), while the default-priority task took the full duration (20 s). |


These results illustrate how the Linux scheduler balances CPU and I/O workloads and how the nice value influences CPU allocation among competing tasks.<img width="468" height="84" alt="image" src="https://github.com/user-attachments/assets/b4a96a51-4415-44b3-b02c-cc937ffb774c" />


