# OS Jackfruit – Multi-Container Runtime


---

## 📌 Project Overview

This project implements a lightweight container runtime using Linux namespaces and a custom kernel module. It enables creation, execution, and supervision of multiple containers with memory limits and scheduling support.

---

## 🚀 Features

* Multi-container supervision using a single supervisor
* Container isolation using Linux namespaces
* Metadata tracking (`ps` command)
* Bounded-buffer logging system
* CLI and IPC using Unix domain sockets
* Memory monitoring with soft and hard limits
* Scheduling control using nice values
* Clean teardown (no zombie processes)

---

## 🛠️ Technologies Used

* C Programming
* Linux Kernel Modules
* POSIX System Calls
* GCC Compiler
* Ubuntu Linux

---

## ⚙️ Build Instructions

```bash id="b1"
cd boilerplate
make clean
make
```

---

## ▶️ How to Run

### 🔹 Step 1: Start Supervisor (Terminal 1)

```bash id="b2"
sudo ./engine supervisor ./rootfs-base
```

---

### 🔹 Step 2: Start Containers (Terminal 2)

```bash id="b3"
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 40 --hard-mib 64
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 50 --hard-mib 80
```

---

### 🔹 Step 3: Check Running Containers

```bash id="b4"
sudo ./engine ps
```

---

### 🔹 Step 4: Run Test Command

```bash id="b5"
echo '#!/bin/sh' > rootfs-alpha/hello.sh
echo 'echo hello > /tmp/output.txt' >> rootfs-alpha/hello.sh
chmod +x rootfs-alpha/hello.sh

sudo ./engine run test ./rootfs-alpha /hello.sh
cat rootfs-alpha/tmp/output.txt
```

---

## 📄 Report

All screenshots and detailed analysis are included in:

👉 **os_ss.pdf**

---

## 🧠 Engineering Analysis

### 1. Isolation Mechanisms

The runtime uses Linux namespaces:

* PID namespace for process isolation
* UTS namespace for hostname isolation
* Mount namespace for filesystem isolation

`chroot()` is used to isolate the root filesystem.

---

### 2. Supervisor and Process Lifecycle

The supervisor acts as the init process:

* Reaps zombie processes using `waitpid()`
* Maintains metadata for containers
* Handles process lifecycle and signals

---

### 3. IPC and Synchronization

* Pipes used for container logging
* Unix domain sockets used for CLI communication
* Bounded buffer with mutex and condition variables ensures thread-safe logging

---

### 4. Memory Management

* RSS used to track memory usage
* Soft limit → warning
* Hard limit → process termination
* Enforcement done in kernel space

---

### 5. Scheduling Behavior

Linux Completely Fair Scheduler (CFS):

* nice values range from -20 to 19
* Lower nice → higher priority
* Verified through CPU workload experiments

---

## 📊 Design Decisions

| Subsystem      | Design Choice      | Tradeoff             | Justification          |
| -------------- | ------------------ | -------------------- | ---------------------- |
| Namespaces     | PID, UTS, Mount    | No network isolation | Simpler design         |
| IPC            | Unix domain socket | Slight overhead      | Reliable communication |
| Logging        | Bounded buffer     | Blocking possible    | Prevents data loss     |
| Kernel monitor | Timer-based        | Coarse granularity   | Easy implementation    |

---

## 📈 Scheduling Experiment Results

* nice -20 → higher CPU usage
* nice 19 → lower CPU usage

This confirms Linux scheduler behavior.

---



---

## ✅ Status

✔ Build successful
✔ Containers running
✔ Memory limits enforced
✔ Scheduling verified
✔ No zombie processes

---

