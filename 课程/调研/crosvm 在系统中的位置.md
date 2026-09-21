# crosvm 在系统中的位置：VMM 与 hypervisor 的解耦

> 本文解释 crosvm 这个 VMM 的职责，以及它在普通 Linux（Type-2）和 xhyper（Type-1）两种系统里的位置，重点说明为什么同一个 crosvm 能在两种迥异的架构上运行。
>
> 证据来源：
> - xhyper：`README.md`、`docs/xhyper-host-boot-memory.md`、`scripts/phase-d4-2-virtio-blk.sh`、`hypervisor/runtime/src/vcpu_run.rs`（`VCPU_RUN_STATE_ADDRSPACE_VMMIO_*`）
> - crosvm 上游：标准 KVM ioctl API（`KVM_CREATE_VM` / `KVM_RUN` / `KVM_SET_USER_MEMORY_REGION` / `KVM_IRQFD` 等）

---

## 1. 一句话结论

**crosvm 是个用户态 VMM，干"设备仿真 + guest 生命周期"，不干 CPU 虚拟化**。它靠一个 KVM-like 的 ioctl 接口和后端 hypervisor 配合。在 Linux 上后端是 KVM（Type-2）；在 xhyper 上后端是 `/dev/xhyper`（Host VM 驱动 → xhyper-mgr → EL2，Type-1）。**同一个 crosvm 能两边跑，是因为 xhyper 在 Host VM 里提供了一个 KVM-like 接口**——crosvm 以为自己还在和 KVM 说话，其实后端是个 Type-1 hypervisor。

VMM ≠ hypervisor。这是理解整件事的关键。

---

## 2. crosvm 这个 VMM 的职责

crosvm 是 Google 用 Rust 写的开源 VMM，最早为 ChromeOS/Chromebook 做虚拟化。它跑在用户态，本身**不实现 CPU 虚拟化**，只负责"设备仿真 + guest 生命周期"：

```text
crosvm (userspace program) responsibilities:
+---------------------------------------------------+
| 1. create VM      allocate guest memory, create   |
|                   vCPUs                           |
| 2. load guest     kernel Image + initrd + cmdline |
| 3. device tree    describe hardware topology to   |
|   / ACPI         the guest                        |
| 4. emulate        virtio-blk/net/gpu/input/vsock, |
|   devices        serial/console, ...             |
| 5. vCPU run loop  call VCPU_RUN, receive exits    |
|                   (MMIO/IO port), emulate, resume |
| 6. interrupt      inject vIRQ to guest when a     |
|   management     device has data ready           |
+---------------------------------------------------+
it needs a "hypervisor backend" providing CPU
virtualization and the trap mechanism
```

crosvm **不能单独跑虚拟机**——它必须有一个 hypervisor 在背后提供 CPU 虚拟化（vCPU 调度、Stage-2/MMU 隔离、trap 转发）。它和 hypervisor 是**搭档关系**，不是替代。

---

## 3. 在普通 Linux 上的位置（Type-2）

你 Ubuntu 里跑 crosvm 时，系统结构是这样：

```text
+----------------------------------------------+
| crosvm (userspace)                           |
|     |  ioctl(/dev/kvm, KVM_RUN, ...)         |
|     v                                        |
| /dev/kvm (KVM module in Linux kernel)       |  <-- hypervisor lives in host OS kernel
|     |  hardware virtualization (VMX/SVM/     |
|     |  ARM VHE)                              |
|     v                                        |
| hardware                                    |
+----------------------------------------------+
   This is Type-2: the hypervisor is part of a host OS.
   KVM provides CPU virtualization;
   crosvm provides device emulation.
   They cooperate via /dev/kvm ioctl interface.
```

- **KVM** 是 Linux 内核模块，用硬件虚拟化扩展（Intel VMX / AMD SVM / ARM VHE）
- **crosvm** 是用户态程序，通过 `/dev/kvm` 的 ioctl 驱动 KVM
- 关键 ioctl：`KVM_CREATE_VM`、`KVM_CREATE_VCPU`、`KVM_SET_USER_MEMORY_REGION`、`KVM_RUN`、`KVM_IRQFD` 等
- 这是 **Type-2**：hypervisor 寄生在 host OS 里，host OS 直接管硬件

---

## 4. 在 xhyper 里的位置（Type-1）

```text
+-------------------------------------------------------------+
|  XHyper (EL2)  <-- real Type-1 hypervisor, independent of   |
|                   any host OS                               |
|  vCPU / Stage-2 MMU / vGIC / vRTC / IPC / timer            |
+------------------------------+------------------------------+
                               | hypercall
+------------------------------+------------------------------+
| xhyper-mgr (EL1, Root VM)   resource manager / policy      |
+------------------------------+------------------------------+
                               | /dev/xhyper (UAPI)
+------------------------------+------------------------------+
| Host VM (EL1, Linux)        <-- a "service VM"             |
|                                                              |
|  +----------------+        +---------------------------+   |
|  | crosvm          |        | /dev/xhyper kernel driver |   |
|  | (userspace)     | ioctl | (Host VM's Linux kernel)  |   |
|  | device emulate  | ----> | translates to hypercall   |   |
|  | + vCPU run loop | <---- | to xhyper-mgr             |   |
|  +----------------+        +---------------------------+   |
+-------------------------------------------------------------+
                                                              |
                                              +---------------+
                                              |  Secondary VM (EL1, guest)
                                              |  kernel + initrd + virtio devices
```

关键点：

- **XHyper 是 Type-1**：EL2 独立 hypervisor，不依赖任何 host OS，直接管硬件
- **crosvm 跑在 Host VM**（一个 Linux guest，EL1）的用户态——它是某个 guest 里的一个普通进程
- Host VM 的 Linux 内核有个 `/dev/xhyper` 驱动；crosvm 调它，驱动把请求翻译成 hypercall 经 xhyper-mgr（Root VM）给 EL2
- crosvm **以为自己还在跑 Linux+KVM**（接口长得像），后端其实是 xhyper
- Host VM 在这里充当**"服务 VM"**（service VM）：它是个 Linux guest，但承担"跑 VMM"的职责，看起来像 Type-2 的 host OS

---

## 5. 为什么同一个 crosvm 能两边跑

这才是有意思的地方。关键在**接口抽象**：

```text
crosvm source code (abstraction):
   fd = open("/dev/kvm"  or  "/dev/xhyper")   <-- backend is swappable
   ioctl(fd, CREATE_VM, ...)
   ioctl(fd, SET_USER_MEMORY_REGION, ...)
   ioctl(fd, VCPU_RUN, ...)                   <-- exits return MMIO access info
   ... emulate virtio, inject irq, resume ...

Backend implementations:
   Linux + KVM:   /dev/kvm    -> KVM kernel module   -> hardware VT
   xhyper:        /dev/xhyper -> Host VM driver      -> xhyper-mgr -> hypercall -> EL2
```

crosvm 是**冲着 KVM 的 ioctl API 写的**。xhyper 只要在 Host VM 里提供一个**长得像 KVM API 的 `/dev/xhyper` 驱动**，crosvm（可能少量改动或加个后端适配层）就能跑。

**VMM 不关心后端是真 KVM 还是 Type-1 hypervisor 伪装的 KVM**——它只调 ioctl。

这就是 VMM 和 hypervisor **解耦**的好处：

- VMM（设备仿真 + guest 生命周期）是**可复用的用户态代码**
- hypervisor（CPU/内存虚拟化 + trap）是**可替换的后端**
- 中间靠一个**标准化的 ioctl 接口**（KVM API 成了事实标准）

KVM API 实际上成了一个**跨 hypervisor 的 VMM 接口标准**。任何 Type-1 hypervisor 只要提供一个 KVM-like 的用户态接口，就能复用 crosvm / firecracker / QEMU 这些现成的 VMM。xhyper 走的就是这条路。

---

## 6. 两种模式对比

```text
Linux + KVM + crosvm (Type-2):          xhyper + Host VM + crosvm (Type-1):

  crosvm (userspace)                      crosvm (Host VM userspace)
     |  /dev/kvm ioctl                       |  /dev/xhyper ioctl
     v                                       v
  KVM (Linux kernel module)              /dev/xhyper driver (Host VM kernel)
     |  hardware VT                          |  hypercall
     v                                       v
  hardware                               xhyper-mgr (Root VM, EL1)
                                            |
                                            v
                                         XHyper (EL2)
                                            |  hardware VT (EL2)
                                            v
                                         hardware
```

| 维度 | Linux + KVM + crosvm（Type-2） | xhyper + Host VM + crosvm（Type-1） |
|---|---|---|
| hypervisor 位置 | host OS 内核模块（KVM） | 独立 EL2（XHyper） |
| crosvm 跑在哪 | host OS 用户态 | Host VM（Linux guest）用户态 |
| 后端接口 | /dev/kvm | /dev/xhyper（KVM-like） |
| host OS | 就是 host 本身 | Host VM 是个"服务 VM"（guest 当 host 用） |
| TCB | host OS 内核 + KVM + crosvm | XHyper + xhyper-mgr + Host VM Linux + crosvm（更大） |
| virtio 仿真 | crosvm | crosvm（完全一样） |
| CPU 虚拟化 | KVM（硬件 VT） | XHyper（硬件 VT at EL2） |
| 启动链 | host OS 起来就能跑 | 要等 EL2 → Root VM → Host VM Linux → crosvm 都起来 |
| Secondary VM 延迟 | guest → KVM → crosvm | guest → EL2 → Host VM → crosvm（多一跳） |

---

## 7. 取舍

**xhyper 选这条路（Type-1 + Host VM + crosvm）的收益**：

- **复用成熟 VMM**：crosvm 的 virtio 栈是 ChromeOS/Android 验证过的，不用自己用 Rust 重写一套 virtio mmio/pci/virtq 仿真
- **复用 KVM API 生态**：未来 firecracker、QEMU 等其他 KVM-based VMM 也可能接入
- **隔离强**：XHyper 是真 Type-1，EL2 独立，不受 Host VM Linux 崩溃影响（Host VM 挂了 hypervisor 还在）

**代价**：

- **TCB 变大**：要信任 XHyper + xhyper-mgr + 一个完整 Host VM Linux + crosvm
- **延迟高**：Secondary VM 的 virtio 路径是 guest → EL2 → Host VM → crosvm，比 Gunyah 的 guest → EL2 → RM VM 多一跳
- **强依赖 Host VM**：Secondary VM 的 virtio 设备可用性依赖 Host VM Linux + crosvm 都在线（phase-d4-2 的验证标记里专门断言 `XHYPER_D423_HOST_ALIVE` 就是这个原因）
- **启动链长**：要 EL2 → Root VM → Host VM → crosvm 全起来才能拉 Secondary

**对比 Gunyah C 版**：Gunyah 把 virtio transport 放 EL2、backend 放 RM VM，**不用 Host VM Linux + crosvm**，TCB 小、延迟低，但要自研全套 virtio。两种是相反的权衡。

---

## 8. 结论

1. **crosvm 是用户态 VMM**：干"设备仿真 + guest 生命周期"，不干 CPU 虚拟化；需要一个 hypervisor 后端配合。

2. **VMM 和 hypervisor 解耦**：靠 KVM API 这个事实标准的 ioctl 接口。VMM 可复用，hypervisor 后端可替换。

3. **同一个 crosvm 能两边跑**：Linux 上后端是 KVM（Type-2）；xhyper 在 Host VM 里提供 KVM-like 的 `/dev/xhyper`，crosvm 以为在和 KVM 说话，实际后端是 Type-1 hypervisor。

4. **xhyper 的 Host VM 是"服务 VM"**：它是个 Linux guest，但承担"跑 VMM"的职责，看起来像 Type-2 的 host OS。这是 Type-1 + 服务 VM 的常见模式。

5. **代价是 TCB 变大、延迟变高、强依赖 Host VM**；收益是复用成熟 crosvm + KVM API 生态，不用自研 virtio。这是和 Gunyah C 版"EL2 自研 virtio + RM VM backend"相反的权衡。

> 注：crosvm 的源码不在本机（xhyper workspace 里没有），本报告关于 crosvm 的职责和 KVM API 是基于公开常识 + xhyper 的 `/dev/xhyper` UAPI 设计反推。crosvm 接入 xhyper 的具体适配层（是否纯 ioctl 兼容、有无 shim）未逐行追踪，那部分代码在 Host VM Linux 内核仓（另查）。
