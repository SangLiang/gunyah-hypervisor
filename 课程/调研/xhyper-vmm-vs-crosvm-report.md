# XHyper 两种 VMM 对比说明：xhyper-vmm vs crosvm

> 报告对象：远程开发机 `10.42.27.86:/media/test/USR-DATA/work/2030/gunyah/xhyper`
> 基于分支：main（HEAD = `e8d4aa98`），版本基线 v0.1.0-2606
> 证据来源：`README.md`、`README-base.md`、`docs/xhyper-host-boot-memory.md`、
> `scripts/phase-d4-secondary-vm.sh`、`scripts/phase-d4-2-virtio-blk.sh`、
> `scripts/phase-d4-build-initrds.sh`、`tools/phase-d4-guest-init`、
> `tools/phase-d4-guest-shell-init`、`tools/phase-d4-guest-virtio-init`、
> `hypervisor/runtime/src/guest_uart.rs`

---

## 1. 一句话结论

XHyper 的 Secondary VM（guest）由 **Host VM（Linux）用户态的 VMM** 拉起，经
`/dev/xhyper → xhyper-mgr → XHyper hypercall → EL2` 完成 VM 创建。当前设计支持两种 VMM：

| VMM | 来源 | 给 guest 的设备 | 能跑的镜像 |
|---|---|---|---|
| **xhyper-vmm** | 麒麟自研，轻量 | 仅 EL2 底座（vCPU/内存/vGIC/vRTC/pl011 控制台），**无 virtio** | kernel Image + initrd（rootfs 在内存） |
| **crosvm** | 上游，成熟 | EL2 底座 + **全套 virtio**（blk/net/gpu/input…） | kernel Image + initrd + **disk image**，可跑完整发行版 |

**关键区别**：xhyper-vmm 只能给 guest 一个"跑在内存里的 initrd"；crosvm 能给
guest 真实的块设备（`/dev/vda`），所以能跑依赖磁盘 rootfs 的发行版（如 Ubuntu）。

---

## 2. 两者在系统中的位置

下图展示两个 VMM 都在 Host VM（Linux，EL1）用户态，都走同一条 UAPI 调用链
到 EL2；区别只在于它们各自给 guest 配了什么设备。图中所有设备由谁提供一目了然。

```text
+--------------------------------------------------------------+
| EL2:  XHyper (built on x-kernel base)                        |
|       vCPU / Stage-2 MMU / vGIC+vIC / vRTC(PL031)            |
|       PSCI / SMCCC / IPC(doorbell,msgqueue) / timer          |
|       pl011 console bridge  (guest_uart.rs)                  |
+------------------------------+-------------------------------+
                               |  hypercall: VM_CREATE / VCPU_RUN / MEM_REGISTER ...
+------------------------------+-------------------------------+
| EL1:  xhyper-mgr  (Root VM, resource manager / policy)       |
+------------------------------+-------------------------------+
                               |  /dev/xhyper  (UAPI)
+------------------------------+-------------------------------+
| EL1:  Host VM (Linux)                                        |
|                                                              |
|  +-------------------+          +------------------------+    |
|  | xhyper-vmm        |          | crosvm                 |    |
|  | /sbin/xhyper-vmm  |          | /sbin/crosvm           |    |
|  | (light, no virtio)|          | (full virtio stack)    |    |
|  +--------+----------+          +-----------+------------+    |
+-----------+--------------------------------+-------------------+
            |                                |
+-----------+----------------+  +-----------+-------------------+
| Secondary VM (guest)       |  | Secondary VM (guest)          |
| kernel + initrd (in RAM)   |  | kernel + initrd + disk        |
| devices: EL2 only          |  | devices: EL2 + virtio         |
| reaches: busybox shell     |  | reaches: mount /dev/vda (rw) |
+----------------------------+  +-------------------------------+
```

> 两条路径都被测试脚本覆盖：
> - `phase-d4-secondary-vm.sh` 走 xhyper-vmm，验证到 `ShellOk`
> - `phase-d4-2-virtio-blk.sh` 走 crosvm，验证到 `/dev/vda` ext4 读写回读

注意：EL2 底座那一部分（vCPU/内存/vGIC/vRTC/pl011）两者**完全相同**，都来自
XHyper（EL2），不由 VMM 提供。VMM 的差异**只在 virtio 设备层**。

---

## 3. 引导流程：VMM 在哪一步出场

无论用哪个 VMM，**Root VM 和 Host VM 都不经过任何用户态 VMM**（crosvm/xhyper-vmm
都不参与它们的拉起）。但两者的拉起机制不同：

- **Root VM（xhyper-mgr）**：EL2 直接造 vCPU 上下文并 enter（`EL1H_MASKED`，Root VM 跑在 EL1）。
- **Host VM（Linux）**：EL2 与 Root VM **协作**完成——EL2 先 enter Root VM，Root VM 在 EL1 发 hypercall，EL2 在 run loop 里响应、准备 Host 的 Stage-2/FDT/命令行，最后 EL2 自己 enter Host vCPU。Host kernel 字节来自打包镜像 `xhyper.img`（不是 Root VM 提供的）。
- VMM 只在最后一步——拉起 Secondary VM——才出场。

```text
xhyper.img 内含三段：XHyper(EL2) + Root VM package(rootvm_gpkg) + Host Linux Image
（Host initrd 是外部输入，不在镜像内）

[1] EL2 解包，读出 Root package 与 Host kernel 字节
        boot_sources.host_kernel_bytes()            lib.rs:1305

[2] EL2 造 Root VM(xhyper-mgr)的 vCPU 上下文
        root_vm::initial_context()                  lib.rs:2535 / root_vm.rs:14
        GuestPstate::EL1H_MASKED   <-- Root VM 是 EL1 guest

[3] EL2 enter Root VM
        arch_vcpu::enter_once()                    lib.rs ~2550
        xhyper-mgr 在 EL1 跑，向 EL2 发 hypercall
        EL2 在 vCPU run loop 里响应这些 hypercall
        (hypercalls_serviced / last_hypercall_number)

[4] EL2 响应 hypercall，准备 Host：
        把 Host kernel 映射进 Host 的 Stage-2、建 Host FDT + 命令行

[5] EL2 enter Host vCPU
        "XHYPER_HOST_BOOT_VCPU ..."                 lib.rs:2837
        Host Linux 在 EL1 启动
                |
        [Host initrd 里带 VMM：xhyper-vmm 或 crosvm]
                |
        +-------+-------+
        |               |
 选 xhyper-vmm:     选 crosvm:
        |               |
 /sbin/xhyper-vmm  /sbin/crosvm run --block /guest/virtio-blk.img ...
   --image ...       --initrd ...
   --initrd ...
        |               |
        +-> /dev/xhyper <-+
              |
        xhyper-mgr (Root VM，经 hypercall 中继)
              |
        XHyper hypercall (EL2)
              |
        创建 Secondary VM
```

要点：
- **Root VM、Host VM 都不依赖任何用户态 VMM**。Root VM 由 EL2 直接造 vCPU 并 enter（`EL1H_MASKED`）；Host VM 由 EL2 与 Root VM 经 hypercall 协作构建，EL2 最后 enter Host vCPU（`hypervisor/runtime/src/lib.rs`：`run_root_rm_boot`、`XHYPER_HOST_BOOT_VCPU`，`hypervisor/runtime/src/root_vm.rs`、`host_boot.rs`）。
- **VMM 是 Secondary VM 的发起方**；不装任何 VMM，Secondary VM 就没人创建。
- crosvm / xhyper-vmm **二选一即可**，不是必须 crosvm。

---

## 4. 镜像要求对比（核心）

### 4.1 接受的输入

```text
xhyper-vmm 的输入：                      crosvm 的输入：
+------------------------+               +------------------------+
| --image  <ARM64 Image> | (必需)        | <ARM64 Image>           | (必需)
| --initrd  <initrd>     | (必需)        | --initrd <initrd>       | (可选)
+------------------------+               | --block  <disk image>  | (可选)
                                         | --device virtio-net/.. | (可选)
        不接受 ISO / disk                +------------------------+
                                         可挂块设备、可加网卡
```

### 4.2 实际调用（从 phase 脚本摘录的真实命令）

**xhyper-vmm**（来自 `README-base.md`）：

```bash
/sbin/xhyper-vmm --image /guest/Image --initrd /guest/initrd.img
```

**crosvm**（来自 `scripts/phase-d4-2-virtio-blk.sh`）：

```bash
/sbin/crosvm run --disable-sandbox --mem 64 --cpus 2 \
    --initrd /guest/initrd-virtio.img \
    -p xhyper.d423=1 \
    --block /guest/virtio-blk.img \
    /guest/Image
```

### 4.3 rootfs 落在哪

```text
xhyper-vmm：rootfs = initrd，全在内存
+----------------------------------------------+
| Guest RAM                                    |
| +----------+  +----------+  +------------+   |
| | kernel   |  | initrd   |  | (无 disk)  |   |
| | Image    |  | rootfs   |  |            |   |
| +----------+  +----------+  +------------+   |
+----------------------------------------------+
 优点：简单、不依赖块设备
 缺点：rootfs 必须塞进 initrd，大 rootfs 不现实

crosvm：rootfs = disk image（virtio-blk），可外加 initrd
+----------------------------------------------+
| Guest RAM                                    |
| +----------+  +----------+                   |
| | kernel   |  | initrd   |                   |
| | Image    |  | (可选)   |                   |
| +----------+  +----------+                   |
+-----------------+----------------------------+
                  | virtio-blk
          +-------+--------+
          | disk image     |  <- /dev/vda (ext4/qcow2/squashfs...)
          | (real rootfs)  |
          +----------------+
 优点：可挂任意大小 rootfs、可持久化
 缺点：需要 virtio-blk 仿真（crosvm 提供）
```

---

## 5. 给 guest 的设备对比

```text
xhyper-vmm 给 guest 的设备          crosvm 给 guest 的设备
+----------------------------+      +------------------------------+
| vCPU          -- from EL2   |      | vCPU          -- from EL2   |
| mem (Stage-2) -- from EL2   |      | mem (Stage-2) -- from EL2   |
| vGIC          -- from EL2   |      | vGIC          -- from EL2   |
| vRTC(PL031)   -- from EL2   |      | vRTC(PL031)   -- from EL2   |
| pl011 console -- EL2 bridge |      | pl011 console -- EL2 bridge |
+----------------------------+      | ---------------------------- |
| x  no virtio-blk (no block) |      | v  virtio-blk  -- crosvm     |
| x  no virtio-net (no net)   |      | v  virtio-net  -- crosvm     |
| x  no virtio-gpu (no disp)  |      | v  virtio-gpu  -- crosvm     |
| x  no virtio-input          |      | v  virtio-input-- crosvm     |
| x  no virtio-vsock          |      | v  virtio-vsock-- crosvm     |
+----------------------------+      +------------------------------+
```

（图例：`x` = 缺失，`v` = 有。EL2 底座两者完全相同，差异只在 virtio 层。）

---

## 6. Guest 实际能跑到什么程度（已验证）

### 6.1 xhyper-vmm 路径（phase-d4-secondary-vm）

- guest = 真 AArch64 Linux kernel Image + busybox initrd（`/init` 被换成仓库自己的脚本）
- `tools/phase-d4-guest-shell-init` 落到 `/bin/sh` 交互 shell（pl011 控制台）
- `tools/phase-d4-guest-init` 探测 SMP（`cpu online = 0-1`）、探 `/sys/class/rtc/rtc0`（EL2 给的 vRTC），用 `devmem` 打 marker
- 验证标记：`BootEnter` → `SmpOk(2)` → `VrtcDeviceOk/TimeOk/SetOk/AlarmOk` → `ShellOk` → `ShutdownBegin`，跑 3 个 create/boot/shutdown 周期
- **能**：busybox shell、内核/驱动回归、SMP/vRTC 探测
- **不能**：挂磁盘、上网、显示

### 6.2 crosvm 路径（phase-d4-2-virtio-blk）

- guest = Linux kernel + initrd-virtio + `virtio-blk.img`（ext4）
- `tools/phase-d4-guest-virtio-init`：等 `/dev/vda` → 挂 ext4 → 读预置 `EXPECTED` → 写 `WRITTEN` → 只读重挂回读 → `poweroff`
- 验证标记：`VIRTIO_DEVICE=/dev/vda` → `READ_OK` → `WRITE_OK` → `READBACK_OK` → `CPUS=0-1` → `POWERDOWN` → `CROSVM_EXIT=0` → `HOST_ALIVE`
- **能**：块设备读写、（理论上）完整发行版启动
- 该路径**还在 phase-d4-2 推进中**，尚未并入主线稳定门禁的全部范围

---

## 7. 能不能直接拉 Ubuntu ISO？

```text
Ubuntu ISO 启动需要的条件：
 1. 访问 ISO 内的 squashfs rootfs  --> 需要块设备 (virtio-blk)
 2. 网络（安装 / cloud-init）       --> 需要 virtio-net
 3. kernel + initrd（可从 ISO 提取）

                 xhyper-vmm           crosvm
 virtio-blk       x  没有              v  有
 virtio-net       x  没有              v  有
 吃 ISO 当块设备   x  只认 Image+initrd  v  --block 可挂镜像
 -------------------------------------------------
 能拉 Ubuntu ISO   x  不能              v  可以（需适配）
```

### 7.1 xhyper-vmm：不能

- 入参只接 `--image <kernel> --initrd <initrd>`，**不加载 ISO**，也没法把 ISO 当块设备给 guest。
- **无 virtio-blk** → guest 看不到任何块设备 → Ubuntu 的 squashfs rootfs 挂不上。
- 即便把 Ubuntu kernel 提取出来喂进去，rootfs 也得塞 initrd，Ubuntu 的 rootfs 几百 MB~GB，塞 initrd 不现实。

### 7.2 crosvm：可以（需做适配）

- crosvm 原生支持 `--block`，可把 Ubuntu 的 disk image / squashfs 挂成 `/dev/vda`。
- 从 ISO 提取 kernel（`casper/vmlinuz`）和 initrd（`casper/initrd`）传给 crosvm，把 ISO 本身或其 squashfs 作为 `--block`。
- 还需要补 `virtio-net`（若要联网安装）。当前 phase-d4-2 只验证了 virtio-blk 的 ext4 读写，**还没验证完整 Ubuntu 启动**，属于待推进项。

---

## 8. 选型建议

```text
你的 guest 需要...                              建议
------------------------------------------------------
 - 跑测试/回归、busybox shell、内核探针      --> xhyper-vmm
 - 只要有控制台、vRTC、SMP 验证               --> xhyper-vmm
 - 不需要磁盘/网络/显卡

 - 需要块设备（磁盘 rootfs、持久化）         --> crosvm
 - 需要网络                                   --> crosvm
 - 要跑完整发行版（Ubuntu/Debian/...）       --> crosvm
 - 需要显示/输入                              --> crosvm
```

一句话：
- **xhyper-vmm = "内核 + 内存 initrd" 的极简启动器**，适合测试型 guest；
- **crosvm = 带完整 virtio 的通用 VMM**，能跑依赖磁盘/网络的真 OS；
- 两者**不是非此即彼的替代**，而是**不同能力档位**：xhyper-vmm 轻量快速，
  crosvm 功能完整。当前（v0.1.0）xhyper-vmm 路径已验证，crosvm+virtio 路径在 phase-d4-2 推进中。

---

## 9. 附：关键证据索引

| 事实 | 证据位置 |
|---|---|
| xhyper-vmm 是独立仓、静态二进制、装到 `/sbin/xhyper-vmm` | `scripts/phase-d4-build-initrds.sh` |
| xhyper-vmm 调用形式 `--image --initrd` | `README-base.md:142` |
| xhyper-vmm 能把真 Linux+busybox 跑到交互 shell | `tools/phase-d4-guest-shell-init`、`scripts/phase-d4-secondary-vm.sh` |
| crosvm 调用形式（含 `--block`） | `scripts/phase-d4-2-virtio-blk.sh:152` |
| crosvm+virtio-blk 跑通 ext4 读写 | `tools/phase-d4-guest-virtio-init`、`scripts/phase-d4-2-virtio-blk.sh` |
| Secondary VM 的 pl011 控制台由 EL2 桥接 | `hypervisor/runtime/src/guest_uart.rs`、架构图 |
| Root VM 由 EL2 直接造 vCPU 上下文（`EL1H_MASKED`）并 enter | `hypervisor/runtime/src/root_vm.rs:14`、`hypervisor/runtime/src/lib.rs:2535` |
| Host VM 由 EL2 构造 vCPU + Stage-2，由 Root VM 经 hypercall 驱动；EL2 最后 enter Host vCPU | `hypervisor/runtime/src/lib.rs`（`run_root_rm_boot`、`XHYPER_HOST_BOOT_VCPU:2837`、`boot_sources.host_kernel_bytes():1305`） |
| Root VM / Host VM 都不经过用户态 VMM | `docs/xhyper-host-boot-memory.md` |
| EL2 提供的设备：vCPU/内存/vGIC/vRTC/IPC/timer | `hypervisor/{arch_vcpu,memory,irq,device/vrtc,ipc,time}` |

> 注：xhyper-vmm 的源码不在本机（workspace 里只有 `xhyper`/`xhyper-abi`/`xhyper-mgr`），
> 上述关于它的能力边界是从调用形式 + guest init 脚本 + 验证标记**反推**的；
> "不能跑 Ubuntu ISO" 是据此推理的结论，未做实机验证。
