# XHyper EL2 Virtio 实现开发计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 xhyper EL2 内实现 Gunyah 式 virtio 框架，达到 **Stage 2 闭环**——Secondary guest 的 `/dev/hvc0` 双向可交互，全链路不依赖 crosvm、不依赖 Host Linux 内核改动。

**Architecture:** 三阶段递进。Stage 0 把 xhyper 既有的 vRTC/vITS MMIO 三态派发（`Unhandled`/`Fault`/`Emulated`）泛化为多设备动态注册表 + 只读 mirror 映射。Stage 1 把 Gunyah 的四部件（`virtio_t` 状态机 / mmio transport / backend 桥对象 / virtq 辅助）移植到 Rust crate 组，接入 object/capability 模型与已冻结的 `xhyper-abi` 快照（0x49–0x55）。Stage 2 实现 virtio-console 的 EL2 内 backend 并唤醒 xhyper-mgr 的休眠编排域。

**Tech Stack:** Rust 1.95（no_std，`aarch64-unknown-none`）、xhyper crates（`hypervisor/*`、`runtime`）、`xhyper-dev:2026.08` 容器、QEMU 10.2.1 AArch64 virt、Kconfig + xkmake 构建。

**Reference spec:** `docs/superpowers/specs/2026-09-23-xhyper-el2-virtio-design.md`
**Reference impl (C):** 本地 Gunyah `hyp/vm/virtio*`（逐文件对照移植源）
**Baseline:** xhyper `main@d67e6f5e`、xhyper-mgr `main@0a39089`、xhyper-abi `main@cba464c`（2026-09-23 拉新后）

**实施约定（沿用 xhyper 规范）：**
- `unsafe` 块旁必须配 `SAFETY:` 注释
- 提交格式 `子系统: 祈使句`、`git commit -s`（DCO）
- 提交前 `cargo fmt --all && make clippy && make unittest`
- model 层纯逻辑先行、红绿循环

---

## 文件结构

### 新建 crate（xhyper 仓）

| 路径 | 职责 | 依赖 |
|---|---|---|
| `hypervisor/virtio/model/src/lib.rs` | 纯状态机：`VirtioStatus` / `FeatureNegotiation` / `QueueState` / `ConfigUpdate` | 无（纯逻辑，no_std + alloc） |
| `hypervisor/virtio/mmio/src/lib.rs` | MMIO transport：寄存器布局 + 写派发 + 读 mirror 维护 | `virtio/model`、`hypervisor/memory/address_space` |
| `hypervisor/virtio/backend/src/lib.rs` | 桥对象：`ObjectKind::VirtioBackend` + reason 位图 + virq 通知 + RAII 生命周期 | `virtio/model`、`hypervisor/object/model`、`hypervisor/irq/vic` |
| `hypervisor/virtio/virtq/src/lib.rs` | 描述符链遍历 + used 回写（基于 `useraccess`） | `hypervisor/memory/useraccess` |
| `hypervisor/virtio/console/src/lib.rs`（Stage 2） | console backend：rx/tx 队列消费 + `guest_uart` 出口 | `virtio/backend`、`virtio/virtq`、runtime `guest_uart` |

### 修改（xhyper 仓）

| 路径 | 改动 | 说明 |
|---|---|---|
| `hypervisor/memory/address_space/src/lib.rs` | 新增 RO mirror 映射 API + VMMIO handler 注册表接口 | Stage 0 |
| `hypervisor/runtime/src/lib.rs` | fault 路径接入注册表查表；新增 virtio_backend hypercall 分发条目 | Stage 0 / 1 |
| `hypervisor/object/model/src/lib.rs` | 新增 `ObjectKind::VirtioBackend` + rights 常量 | Stage 1 |
| `hypervisor/runtime/src/object_cleanup.rs` | 新增 `VirtioBackend` 的 prepared/cleanup 事务 | Stage 1 |
| `Kconfig`、`platforms/kplat-aarch64/qemu_defconfig` | `CONFIG_HYPERVISOR_VIRTIO`（默认 off） | Stage 0 |
| `scripts/phase-e0-vdevice.sh`、`scripts/phase-e1-probe.sh`、`scripts/phase-e2-console.sh` | 新门禁脚本 | 各 Stage |

### 修改（xhyper-mgr 仓，Stage 2）

| 路径 | 改动 |
|---|---|
| `crates/xhyper-mgr-core/src/domains/platform.rs` | `virtio_mmio: Unavailable` → `Available`（feature-gated） |
| `crates/xhyper-mgr-core/src/domains/virtual_device.rs` | 唤醒 `realize_virtio_mmio` 配置路径 + console 配置项 |

### Gunyah C 参照表（移植源）

| Rust 目标 | Gunyah C 源（本地 `hyp/vm/`） |
|---|---|
| `virtio/model` | `virtio/src/frontend.c`（状态机 `virtio_write_status` 等，617 行）+ `virtio/src/backend.c` |
| `virtio/mmio` | `virtio_mmio/src/{virtio_mmio.c,vdevice.c}` |
| `virtio/backend` | `virtio_backend/src/virtio_backend.c`（223 行，reason 位图 + virq 通知） |
| `virtio/virtq` | `virtio_virtq/src/virtio_virtq.c`（201 行，描述符链 + release fence） |
| ABI 合同 | `hyp/interfaces/virtio_backend/virtio_backend.hvc`（0x49–0x55） |

---

## 前置准备

- [ ] **Step 0.1: 建工作分支**

```bash
ssh root@10.42.27.86   # 已配置
cd /media/test/USR-DATA/work/2030/gunyah/xhyper
git checkout -b virtio-el2 origin/main
cd ../xhyper-mgr && git checkout -b virtio-el2 origin/main
```

- [ ] **Step 0.2: 容器内验证基线可构建**

```bash
docker run --rm -it --network host \
    -v /media/test/USR-DATA/work/2030/gunyah:/workspace \
    -v xhyper-cargo-cache:/cache \
    -w /workspace/xhyper xhyper-dev:2026.08 bash
cp platforms/kplat-aarch64/qemu_defconfig .config
make defconfig && make build
```
Expected: 构建成功，无 virtio 相关 warning。

- [ ] **Step 0.3: 确认 xhyper-abi 快照含数据面 ABI**

```bash
grep -c HYPERCALL_VIRTIO_BACKEND /workspace/xhyper-abi/generated/hypercalls.rs
```
Expected: `11`（0x49–0x55 全套 + partition_create）。确认 EL2 侧常量可用。

- [ ] **Step 0.4: 提交基线快照**

```bash
git commit --allow-empty -s -m "virtio-el2: baseline snapshot for EL2 virtio work"
```

---

## Stage 0 · EL2 MMIO 派发框架泛化

**目标：** 把 vRTC/vITS 既有的固定地址三态派发泛化为按 `(addrspace, IPA range)` 的多设备动态注册表，并支持只读 mirror 映射。产出：一个 trivial 测试设备能被 guest 读写，且既有 phase-d4 门禁零回归。

### Task 0.1: 调研既有派发机制（不写代码，产出笔记）

**Files:**
- Read: `hypervisor/device/vrtc/src/store.rs`（`access_mmio` + `VrtcMmioResult` 三态）
- Read: `hypervisor/runtime/src/lib.rs`（grep `vrtc` 派发点，~10 处）
- Read: `hypervisor/memory/address_space/src/fault.rs`（`FaultKind::Vmmio` / `ResumeRequest`）
- Read: `hypervisor/device/vrtc/docs/design.md`（`ADDRSPACE_CONFIGURE_RANGE(ADD_VMMIO)` 描述）

- [ ] **Step 1: 记录既有派发流程**——vRTC 是怎么注册窗口、runtime 怎么在 fault 路径调到 `access_mmio`、三态结果怎么映射到 `ResumeRequest`。产出一段笔记存到 `hypervisor/virtio/docs/stage0-survey.md`。
- [ ] **Step 2: 识别泛化点**——当前是固定地址特判（vRTC window 写死在 runtime 派发点），需要改成"查注册表"。确认注册表的 key（`addrspace handle` + `IPA range`）、value（handler trait object 或 fn pointer）。
- [ ] **Step 3: Commit 笔记**

```bash
git add hypervisor/virtio/docs/stage0-survey.md
git commit -s -m "virtio: document existing MMIO dispatch for Stage 0 generalization"
```

### Task 0.2: VMMIO handler 注册表（model 层）

**Files:**
- Create: `hypervisor/memory/address_space/src/vmmio_registry.rs`
- Create: `hypervisor/memory/address_space/tests/vmmio_registry.rs`
- Modify: `hypervisor/memory/address_space/src/lib.rs`（pub mod 声明）

- [ ] **Step 1: 写失败测试——注册/查找/未命中**

```rust
// hypervisor/memory/address_space/tests/vmmio_registry.rs
use hypervisor_address_space::vmmio_registry::{VmmioRegistry, VmmioHandler, VmmioAccess, VmmioResult};

struct DummyHandler;
impl VmmioHandler for DummyHandler {
    fn handle(&self, access: VmmioAccess) -> VmmioResult {
        VmmioResult::Emulated(0xdead_beef)
    }
}

#[test]
fn registered_handler_is_found_by_ipa() {
    let mut reg = VmmioRegistry::new();
    reg.insert(addrspace_handle(1), 0x4a000000..0x4a000200, DummyHandler);
    let r = reg.lookup(addrspace_handle(1), 0x4a000100);
    assert!(matches!(r, Some(_)));
}

#[test]
fn unregistered_ipa_returns_none() {
    let reg = VmmioRegistry::new();
    assert!(reg.lookup(addrspace_handle(1), 0x4a000100).is_none());
}

#[test]
fn different_addrspace_is_isolated() {
    let mut reg = VmmioRegistry::new();
    reg.insert(addrspace_handle(1), 0x4a000000..0x4a000200, DummyHandler);
    assert!(reg.lookup(addrspace_handle(2), 0x4a000100).is_none());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cargo test -p hypervisor-address-space --test vmmio_registry`
Expected: FAIL（`VmmioRegistry` 未定义）

- [ ] **Step 3: 实现注册表**

```rust
// hypervisor/memory/address_space/src/vmmio_registry.rs
use alloc::boxed::Box;
use core::ops::Range;

pub struct VmmioAccess {
    pub ipa: u64,
    pub size: u8,
    pub write: bool,
    pub write_value: u64,
}

pub enum VmmioResult {
    Unhandled,    // 不在本 handler 管辖范围
    Fault,        // 非法访问（非对齐/宽度错）
    Emulated(u64), // 处理完成，返回 read 值（write 时忽略）
}

pub trait VmmioHandler {
    fn handle(&self, access: VmmioAccess) -> VmmioResult;
}

pub struct VmmioEntry {
    pub range: Range<u64>,
    pub handler: Box<dyn VmmioHandler>,
}

pub struct VmmioRegistry {
    // 按 addrspace handle 分桶；每桶内按 range 排序
    // 实现细节：BTreeMap<addrspace_handle, Vec<VmmioEntry>>
    // xhyper 的 ObjectHandle 见 hypervisor/object/model/src/lib.rs
    entries: alloc::collections::BTreeMap<u64, alloc::vec::Vec<VmmioEntry>>,
}

impl VmmioRegistry {
    pub const fn new() -> Self { Self { entries: alloc::collections::BTreeMap::new() } }
    pub fn insert(&mut self, addrspace: u64, range: Range<u64>, handler: impl VmmioHandler + 'static) { /* ... */ }
    pub fn lookup(&self, addrspace: u64, ipa: u64) -> Option<&VmmioEntry> { /* ... */ }
    pub fn remove(&mut self, addrspace: u64, range: &Range<u64>) { /* ... */ }
}
```

> **实现指引：** `lookup` 按 addrspace 取桶，桶内线性搜 range（QEMU virt 下设备数少，线性够；后续按需换区间树）。`insert` 检查与既有 range 不重叠（重叠返回 `Err`，映射 Gunyah `ERROR_BUSY`）。

- [ ] **Step 4: 跑测试确认通过**

Run: `cargo test -p hypervisor-address-space --test vmmio_registry`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add hypervisor/memory/address_space/src/vmmio_registry.rs hypervisor/memory/address_space/tests/vmmio_registry.rs hypervisor/memory/address_space/src/lib.rs
git commit -s -m "address_space: add VMMIO handler registry for multi-device dispatch"
```

### Task 0.3: runtime fault 路径接入注册表

**Files:**
- Modify: `hypervisor/runtime/src/lib.rs`（VMMIO fault 处理路径，在现有"exit to VMM"之前插查表）
- Reference: Task 0.1 调研笔记里 vRTC 派发点的位置（~10 处 `&self.vrtc` 调用）

- [ ] **Step 1: 写失败测试——注册的 handler 命中时不 exit 给 VMM**

参照 `hypervisor/runtime/tests/vcpu_run_dispatch.rs` 的既有 fixture 风格，加一个测试：注册一个 dummy handler 到某 IPA range，构造一个该 IPA 的 VMMIO fault，断言 runtime 返回 `ResumeRequest::vmmio_read(value, Default)` 而非 exit-to-VMM 状态。

- [ ] **Step 2: 跑测试确认失败**
Run: `cargo test -p hypervisor-runtime --test vcpu_run_dispatch`
Expected: FAIL（runtime 还没查注册表）

- [ ] **Step 3: 在 runtime 持有 `VmmioRegistry` 实例并在 fault 路径查表**

> **集成指引（对照 vRTC 既有派发点）：** runtime 已在 VMMIO fault 处调 `self.vrtc.access_mmio(...)`。把这种特判改成：先查 `self.vmmio_registry.lookup(addrspace, ipa)`，命中则调 handler 的 `handle(access)`，按 `VmmioResult` 映射到 `ResumeRequest`（`Emulated(v)` → `vmmio_read(v, Default)` / `vmmio_write(Default)`；`Fault` → `ResumeAction::Fault`；`Unhandled` → 继续走原 exit-to-VMM 路径）。vRTC 本身后续迁移成注册表里的一个 handler（本任务可保留 vRTC 既有路径不动，注册表仅服务新设备——避免一次性改太多）。

- [ ] **Step 4: 跑测试确认通过**
- [ ] **Step 5: 跑既有回归确认 vRTC/crosvm 路径不受影响**

Run: `make unittest BLK=n`（容器内）
Expected: 全绿（vRTC 测试 + VCPU dispatch 测试 + 其余）

- [ ] **Step 6: Commit**

```bash
git commit -s -m "runtime: consult VMMIO registry before exiting to VMM"
```

### Task 0.4: 只读 mirror 映射支持

**Files:**
- Modify: `hypervisor/memory/address_space/src/lib.rs`（新增 RO 映射 API）
- Modify: `hypervisor/memory/address_space/src/transaction.rs`
- Reference: `mem: add private MAP, MODIFY_PAGES` 提交（`78065eff`）已扩展 transaction，复用其模式

- [ ] **Step 1: 写失败测试——把一页以 RO 映射进 addrspace，guest 读不 fault、写 fault**

参照 `hypervisor/memory/address_space/tests/fault.rs` 既有风格，构造一个 addrspace，把一个 EL2 拥有的页以 RO 权限映射进某 IPA range，断言：读该 IPA → `ResumeAction::Retry`（命中映射，不 fault）；写该 IPA → `FaultKind::Page`（permission fault）。

- [ ] **Step 2: 跑测试确认失败**
- [ ] **Step 3: 实现 RO 映射 API**

> **实现指引：** 在 `transaction.rs` 的 mapping 操作里加一个 `permissions: Stage2Perms::ReadOnly` 选项（对照 `78065eff` 的 private MAP 模式）。映射的物理页来自 memextent 捐赠（EL2 经自己的 hyp 映射 RW 维护页内容）。

- [ ] **Step 4: 跑测试确认通过**
- [ ] **Step 5: Commit**

```bash
git commit -s -m "address_space: support read-only mirror mapping for vdevice config pages"
```

### Task 0.5: trivial 测试设备

**Files:**
- Create: `hypervisor/virtio/test_device/src/lib.rs`（一页寄存器：读返回已知模式、写翻转计数）
- Create: `hypervisor/virtio/test_device/tests/model.rs`
- Modify: `hypervisor/runtime/src/lib.rs`（注册 test_device 到 VmmioRegistry，feature-gated）

- [ ] **Step 1: 写 test_device 的 model 测试**（纯逻辑：写 offset X 后读 offset X 返回写入值；读 magic offset 返回已知 pattern）
- [ ] **Step 2: 实现 test_device**（实现 `VmmioHandler` trait）
- [ ] **Step 3: 在 runtime 加 feature-gated 注册**（`#[cfg(feature = "test_device")]`）
- [ ] **Step 4: 跑单测**
- [ ] **Step 5: Commit**

```bash
git commit -s -m "virtio: add trivial test device for dispatch framework validation"
```

### Task 0.6: `phase-e0-vdevice` 门禁

**Files:**
- Create: `scripts/phase-e0-vdevice.sh`（参照 `scripts/phase-d4-2-lifecycle.sh` 骨架）
- Modify: `Makefile`（加 `phase-e0-vdevice` 目标）

- [ ] **Step 1: 写门禁脚本**——构建带 `CONFIG_TEST_DEVICE=y` 的镜像，QEMU 启动到 `xhyper>` shell，从测试设备读 magic、写计数、读回，断言匹配。
- [ ] **Step 2: 跑门禁**

Run: `make phase-e0-vdevice SMP=2 MEM=1g`
Expected: `PASS`

- [ ] **Step 3: Commit**

```bash
git add scripts/phase-e0-vdevice.sh Makefile
git commit -s -m "scripts: add phase-e0-vdevice gate for MMIO dispatch framework"
```

### Task 0.7: 回归验证

- [ ] **Step 1: 跑既有 phase-d4 全套（feature off）**

Run: `make phase-d4-verify`
Expected: 全绿（crosvm 路径零回归）

- [ ] **Step 2: 跑 clippy + fmt**

Run: `make clippy && cargo fmt --all --check`
Expected: 无 warning

- [ ] **Stage 0 完成标志：** `phase-e0-vdevice` PASS + `phase-d4-verify` PASS + test_device 在 guest 里可读写。

---

## Stage 1 · virtio 框架核心（移植 Gunyah 四部件）

**目标：** Secondary guest Linux 标准 `virtio_mmio` 驱动能 probe、协商 feature 到 `DRIVER_OK`、建队列（用 stub backend 验证通知链路）。EL2 框架生产级、匹配 `xhyper-abi` 0x49–0x55 ABI。

### Task 1.1: `virtio/model` —— `VirtioStatus` 状态机

**Files:**
- Create: `hypervisor/virtio/model/src/lib.rs`
- Create: `hypervisor/virtio/model/tests/status.rs`
- Gunyah 参考: `hyp/vm/virtio/src/frontend.c` 的 `virtio_write_status`（第 269–368 行）+ `virtio_read_status`

- [ ] **Step 1: 写失败测试——合法递进**

```rust
// hypervisor/virtio/model/tests/status.rs
use hypervisor_virtio_model::status::{VirtioStatus, StatusWriteError};

#[test]
fn ack_then_driver_then_features_ok_then_driver_ok() {
    let mut s = VirtioStatus::reset();
    assert_eq!(s.write(VirtioStatus::ACKNOWLEDGE), Ok(()));
    assert_eq!(s.write(VirtioStatus::DRIVER), Ok(()));
    assert_eq!(s.write(VirtioStatus::FEATURES_OK), Ok(()));
    assert_eq!(s.write(VirtioStatus::DRIVER_OK), Ok(()));
    assert!(s.is_driver_ok());
}
```

- [ ] **Step 2: 写失败测试——非法递进被拒**

```rust
#[test]
fn driver_without_acknowledge_is_rejected() {
    let mut s = VirtioStatus::reset();
    assert_eq!(s.write(VirtioStatus::DRIVER), Err(StatusWriteError::ArgumentInvalid));
}

#[test]
fn driver_ok_without_features_ok_is_rejected() {
    let mut s = VirtioStatus::reset();
    s.write(VirtioStatus::ACKNOWLEDGE).unwrap();
    s.write(VirtioStatus::DRIVER).unwrap();
    assert_eq!(s.write(VirtioStatus::DRIVER_OK), Err(StatusWriteError::ArgumentInvalid));
}

#[test]
fn unknown_bits_rejected() {
    let mut s = VirtioStatus::reset();
    assert_eq!(s.write(0x80), Err(StatusWriteError::ArgumentInvalid));
}
```

- [ ] **Step 3: 写失败测试——写 0 触发复位请求**

```rust
#[test]
fn writing_zero_requests_reset_and_sets_needs_reset() {
    let mut s = VirtioStatus::reset();
    s.write(VirtioStatus::ACKNOWLEDGE).unwrap();
    s.write(VirtioStatus::DRIVER).unwrap();
    s.write(VirtioStatus::FEATURES_OK).unwrap();
    s.write(VirtioStatus::DRIVER_OK).unwrap();
    let ev = s.write(0).unwrap();
    assert!(ev.reset_requested);
    assert!(s.is_needs_reset());
}
```

- [ ] **Step 4: 跑测试确认失败**
Run: `cargo test -p hypervisor-virtio-model --test status`
Expected: FAIL

- [ ] **Step 5: 实现 `VirtioStatus`**

> **实现指引（对照 `frontend.c:269-368`）：**
> - `VirtioStatus` 是一个 bitfield（ACKNOWLEDGE=1, DRIVER=2, FEATURES_OK=8, DRIVER_OK=4, FAILED=0x80, NEEDS_RESET=0x40）
> - `write(bits) -> Result<StatusWriteEvent, StatusWriteError>`：按规范递进校验；写 0 → `reset_requested` + 置 NEEDS_RESET；unknown 位 → `ArgumentInvalid`
> - `read()` 在 `reset_requested` 时返回"复位进行中"语义（Gunyah 里会阻塞，model 层先返回状态枚举，阻塞在 runtime 层实现）
> - 用 enum 而非 raw u8，强制穷尽（Rust 优势）

```rust
// hypervisor/virtio/model/src/status.rs
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StatusWriteError { ArgumentInvalid, Denied }

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StatusWriteEvent {
    pub reset_requested: bool,
    pub driver_ok_set: bool,
    pub failed_set: bool,
}

bitflags::bitflags! {
    pub struct VirtioStatus: u8 {
        const ACKNOWLEDGE     = 1;
        const DRIVER          = 2;
        const DRIVER_OK       = 4;
        const FEATURES_OK     = 8;
        const NEEDS_RESET     = 0x40;
        const FAILED          = 0x80;
    }
}

impl VirtioStatus {
    pub const fn reset() -> Self { Self::empty() }
    pub fn write(&mut self, bits: u8) -> Result<StatusWriteEvent, StatusWriteError> {
        // 逐条对照 frontend.c:269-368 的递进校验
        // 返回事件让 runtime 决定通知谁
        todo!()  // 实现时照 frontend.c 逐条翻译
    }
    pub const fn is_driver_ok(self) -> bool { self.contains(Self::DRIVER_OK) }
    pub const fn is_needs_reset(self) -> bool { self.contains(Self::NEEDS_RESET) }
}
```

- [ ] **Step 6: 跑测试确认通过**
- [ ] **Step 7: Commit**

```bash
git add hypervisor/virtio/model/
git commit -s -m "virtio/model: add VirtioStatus state machine with exhaustive transitions"
```

### Task 1.2: `virtio/model` —— Feature 协商 + QueueState + ConfigUpdate

**Files:**
- Modify: `hypervisor/virtio/model/src/lib.rs`（加 `FeatureNegotiation` / `QueueState` / `ConfigUpdate`）
- Create: `hypervisor/virtio/model/tests/features.rs`、`tests/queue.rs`、`tests/config.rs`
- Gunyah 参考: `frontend.c` 的 `virtio_get_dev_features` / `virtio_set_drv_features` / `virtio_get_queue_info_ptr` / `virtio_config_update_begin/end`

- [ ] **Step 1: 写 feature 协商测试**（banked sel：写 `dev_feat_sel=0` 后 `dev_feat[0]` 可读；`features_ok` 写之前 `drv_feat` 不生效）
- [ ] **Step 2: 写 QueueState 测试**（`queue_sel` 选队列、`queue_num` 写入 min(size, max)、`queue_ready` 置位、`queue_desc/drv/dev` 高低 32 位拼 64 位地址）
- [ ] **Step 3: 写 ConfigUpdate 测试**（`begin` 置 update 标志、generation 读自增；`end` 时 `gen++` + 清标志）
- [ ] **Step 4–6: 实现 + 跑测试 + Commit**

```bash
git commit -s -m "virtio/model: add feature negotiation, queue state, config update"
```

> **实现指引：** 全部纯逻辑、无 unsafe、无硬件依赖。banked 寄存器用 struct + sel 索引建模（比 C 的 raw 数组更安全）。64 位地址拼接：`desc_addr = (desc_high as u64) << 32 | desc_low as u64`。

### Task 1.3: `virtio/mmio` —— MMIO transport

**Files:**
- Create: `hypervisor/virtio/mmio/src/lib.rs`
- Create: `hypervisor/virtio/mmio/tests/dispatch.rs`
- Gunyah 参考: `hyp/vm/virtio_mmio/src/virtio_mmio.c`（寄存器布局 + `virtio_mmio_handle_virtio_startup`）+ `vdevice.c`（`virtio_mmio_vdevice_write` 写派发，第 280–359 行）

- [ ] **Step 1: 写寄存器布局常量 + 测试**

```rust
// virtio-mmio 寄存器偏移（对照 virtio_mmio_regs_t）
pub const MAGIC_VALUE: u32      = 0x000;
pub const VERSION: u32          = 0x004;
pub const DEVICE_ID: u32        = 0x008;
pub const VENDOR_ID: u32        = 0x00c;
pub const DEVICE_FEATURES_SEL: u32 = 0x014;
pub const DEVICE_FEATURES: u32  = 0x020;
pub const DRIVER_FEATURES_SEL: u32 = 0x024;
pub const DRIVER_FEATURES: u32  = 0x028;
pub const QUEUE_SEL: u32        = 0x030;
pub const QUEUE_NUM_MAX: u32    = 0x034;
pub const QUEUE_NUM: u32        = 0x038;
pub const QUEUE_READY: u32      = 0x044;
pub const QUEUE_NOTIFY: u32     = 0x050;
pub const STATUS: u32           = 0x070;
pub const CONFIG_GEN: u32       = 0x078;
pub const DEVICE_CONFIG_BASE: u32 = 0x100;
```

- [ ] **Step 2: 写写派发测试**（写 `STATUS` → 调 `model.status.write()`；写 `QUEUE_NOTIFY` → 产生通知事件；写 `DRIVER_FEATURES` → 调 `model.features.set_drv()`）
- [ ] **Step 3: 写读 mirror 测试**（写 `DEVICE_FEATURES_SEL=1` 后读 `DEVICE_FEATURES` 返回 `dev_feat[1]`；写 `STATUS` 后读 `STATUS` 返回新值——证明写 handler 同步更新了页）
- [ ] **Step 4: 实现 `VirtioMmio` struct**（持有 `&VirtioModel` + config cache 页引用，实现 `VmmioHandler`）

> **实现指引（对照 `vdevice.c:280-359`）：** `handle(access)` 按 offset 大 match 派发。写 → 调 model 对应方法 + 更新 config cache 页内对应字段（保证 guest RO 读到新值）；读 → 从 config cache 页读（实际由 Stage 0 的 RO mirror 直接命中，不进 handler——但 handler 仍需支持 fault 路径读，对照 vdevice.c 的"读返回 UNHANDLED"语义）。

- [ ] **Step 5: 跑测试 + Commit**

```bash
git commit -s -m "virtio/mmio: add MMIO transport with register dispatch and read mirror"
```

### Task 1.4: `virtio/backend` —— 桥对象（object model 接入）

**Files:**
- Create: `hypervisor/virtio/backend/src/lib.rs`
- Create: `hypervisor/virtio/backend/tests/lifecycle.rs`、`tests/notify.rs`
- Modify: `hypervisor/object/model/src/lib.rs`（加 `ObjectKind::VirtioBackend` + rights）
- Modify: `hypervisor/runtime/src/object_cleanup.rs`（加 `PreparedVirtioBackendRemove`）
- Gunyah 参考: `hyp/vm/virtio_backend/src/virtio_backend.c`（223 行，`virtio_backend_notify` reason 位图 + virq）

- [ ] **Step 1: 在 object model 加 ObjectKind + rights**

```rust
// hypervisor/object/model/src/lib.rs
pub enum ObjectKind {
    // ... 既有
    VirtioBackend,  // 新增
}

// rights 常量（对照 CAP_RIGHTS_VIRTIO_BACKEND_*）
pub mod rights {
    // ... 既有
    pub const VIRTIO_BACKEND_CONFIG: u64        = 1 << N;
    pub const VIRTIO_BACKEND_BIND_MMIO_FRONTEND_VIRQ: u64 = 1 << (N+1);
    pub const VIRTIO_BACKEND_BIND_VIRQ: u64      = 1 << (N+2);
    // ... 对照 hyprights.h 的 CAP_RIGHTS_VIRTIO_BACKEND_* 逐条加
}
```

> **指引：** N 的具体值查 `hypervisor/object/model/src/lib.rs` 既有 rights 的最大位，顺延。对照 Gunyah `hyp/include/hyprights.h` 的 `CAP_RIGHTS_VIRTIO_BACKEND_*`。

- [ ] **Step 2: 写 reason 位图测试**（`fetch_or(new_buffer)` 后 `get_notification()` 返回 `new_buffer`；多次 `fetch_or` 累积；`Acquire` 读到 `Release` 写）

```rust
#[test]
fn reason_bitmap_accumulates_with_release_acquire() {
    let b = ReasonBitmap::new();
    b.fetch_or(NotifyReason::NEW_BUFFER, Ordering::Release);
    b.fetch_or(NotifyReason::DRIVER_OK, Ordering::Release);
    let r = b.get_and_clear(Ordering::Acquire);
    assert!(r.contains(NotifyReason::NEW_BUFFER | NotifyReason::DRIVER_OK));
    // 清后再次读为空
    assert!(b.get_and_clear(Ordering::Acquire).is_empty());
}
```

- [ ] **Step 3: 写生命周期测试**（create → configure → bind_virq → activate → deactivate → cleanup；activate 失败时 RAII guard 回滚）
- [ ] **Step 4: 实现 `VirtioBackend` struct**（持有 `VirtioModel` + reason 位图 + virq binding + lifecycle 状态）

> **实现指引（对照 `virtio_backend.c:76-89`）：**
> - `notify(reason)`：`reason_bitmap.fetch_or(reason, Release)`，若新位置位 → `virq_assert` 到 backend VM
> - 生命周期用 RAII guard：`ActivateGuard` 在 Drop 时按已完成步骤逆序回滚（对照 Gunyah `unwind_object_activate`，但 Rust 类型化）
> - `object_cleanup.rs` 加 `PreparedVirtioBackendRemove`（对照 `PreparedVrtcRemove` 的模式——先 prepare 验证无活跃 binding，再 commit）

- [ ] **Step 5: 跑测试 + Commit**

```bash
git commit -s -m "virtio/backend: add bridge object with reason bitmap, virq, RAII lifecycle"
```

### Task 1.5: `virtio/virtq` —— 描述符链 + used 回写

**Files:**
- Create: `hypervisor/virtio/virtq/src/lib.rs`
- Create: `hypervisor/virtio/virtq/tests/desc_chain.rs`
- Gunyah 参考: `hyp/vm/virtio_virtq/src/virtio_virtq.c`（201 行，`virtio_virtq_read_desc_chain` 第 16–114 行 + `virtio_virtq_write_reply` 第 116–200 行）
- 依赖: `hypervisor/memory/useraccess`（已存在）

- [ ] **Step 1: 写描述符链遍历测试**（mock useraccess：read-only 段 + write-only 段 + write 后 read 即断链）

> **指引：** `useraccess` 的接口见 `hypervisor/memory/useraccess/src/lib.rs`（`copy_from_guest_ipa` / `copy_to_guest_ipa`）。测试用 trait 抽象出 mock，避免真 guest 内存。

- [ ] **Step 2: 写 used 回写测试**（写回复到 write-only desc → `fence(Release)` → 写 used ring head idx；验证顺序）
- [ ] **Step 3: 实现 `Virtq` struct**（持有 addrspace handle + queue 地址，方法 `read_desc_chain` / `write_reply`）

> **实现指引（对照 `virtio_virtq.c`）：**
> - `read_desc_chain`：从 `desc[idx]` 经 `useraccess_copy_from_guest_ipa` 读描述符，按 `VIRTQ_DESC_F_NEXT` 走链；split VQ 规则：read-only 段在前、write-only 段在后、write 后遇 read 即 break
> - `write_reply`：写回复到 write-only desc → `core::sync::atomic::fence(Ordering::Release)` → 写 used ring `{id, len}` + 自增 `used.idx`
> - **SAFETY 注释**：每个 `useraccess` 调用注明"IPA 已经过 memextent 校验、本上下文持有 addrspace 引用"

- [ ] **Step 4: 跑测试 + Commit**

```bash
git commit -s -m "virtio/virtq: add descriptor chain walker and used ring writeback with release fence"
```

### Task 1.6: hypercall 分发集成（管理面）

**Files:**
- Modify: `hypervisor/runtime/src/lib.rs`（在 hypercall 分发表加 5 个条目）
- Create: `hypervisor/runtime/tests/virtio_backend_abi.rs`
- Gunyah 参考: `hyp/interfaces/virtio_backend/virtio_backend.hvc`（0x49/0x4c/0x4d + partition_create + object_activate）
- xhyper 现成常量: `xhyper-abi/generated/hypercalls.rs` 的 `HYPERCALL_VIRTIO_BACKEND_*`

- [ ] **Step 1: 写 hypercall 分发测试**（create → configure → bind_virq → activate 完整链；错误码对齐 Gunyah `ERROR_*`）

参照 `hypervisor/runtime/src/lib.rs` 既有 `vrtc_abi_dispatch_completes_the_transactional_lifecycle` 测试（第 34114 行）的风格，为 virtio_backend 写同类测试。

- [ ] **Step 2: 跑测试确认失败**
- [ ] **Step 3: 在分发表加条目**

```rust
// hypervisor/runtime/src/lib.rs 的 hypercall dispatch match
xhyper_abi::HYPERCALL_PARTITION_CREATE_VIRTIO_BACKEND => objects.create_virtio_backend(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_CONFIGURE => objects.configure_virtio_backend(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_BIND_VIRQ => objects.bind_virtio_backend_virq(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_UNBIND_VIRQ => objects.unbind_virtio_backend_virq(registers),
// OBJECT_ACTIVATE 是通用的，已在既有分发表里，只需让 VirtioBackend kind 走自己的 activate 路径
```

> **指引：** 每个 handler 按 vRTC 的对应 handler 模式（`create_vrtc` / `configure_vrtc_internal` / `bind_vrtc_virq_internal` 见 lib.rs:18343+）逐条对照写。capability 校验用 `resolve_root_capability(cap, ObjectKind::VirtioBackend, Rights::*)`。错误码映射：`ERROR_ARGUMENT_INVALID` → `xhyper_abi::ERROR_ARGUMENT_INVALID` 等（对照 `vrtc_store_error` 映射函数 lib.rs:20839 的模式）。

- [ ] **Step 4: 跑测试 + 既有回归 + Commit**

```bash
git commit -s -m "runtime: add virtio_backend management hypercall dispatch (0x49/0x4c/0x4d)"
```

### Task 1.7: `phase-e1-probe` 门禁（stub backend，DRIVER_OK 闭环）

**Files:**
- Create: `scripts/phase-e1-probe.sh`
- Modify: `Makefile`
- 依赖: 一个 stub backend（在 EL2 内，收到 `queue_notify` 只回 `Emulated` 不做真 I/O——验证通知链路通）

- [ ] **Step 1: 写门禁脚本**——构建带 `CONFIG_HYPERVISOR_VIRTIO=y` + stub backend 的镜像；QEMU 启动 Secondary VM，guest DT 含 virtio_mmio 节点；验证 guest `dmesg` 出现 `virtio_mmio` 驱动 probe + `DRIVER_OK` 协商成功 + 队列 ready。
- [ ] **Step 2: 跑门禁**

Run: `make phase-e1-probe SMP=2 MEM=1g`
Expected: `PASS`（guest 日志含驱动初始化完成）

- [ ] **Step 3: 回归 `phase-e0-vdevice` + `phase-d4-verify`**
- [ ] **Step 4: Commit**

```bash
git commit -s -m "scripts: add phase-e1-probe gate for virtio framework DRIVER_OK milestone"
```

- [ ] **Stage 1 完成标志：** guest 标准 virtio_mmio 驱动 probe 到 `DRIVER_OK`、队列 ready，通知链路（queue_notify → reason → virq）验证通。

---

## Stage 2 · virtio-console backend（EL2 内闭环）

**目标：** Secondary guest `/dev/hvc0` 双向交互，全链路不依赖 crosvm、不依赖 Host Linux 内核改动。

### Task 2.1: console backend（EL2 内）

**Files:**
- Create: `hypervisor/virtio/console/src/lib.rs`
- Create: `hypervisor/virtio/console/tests/tx_rx.rs`
- Gunyah 参考: `hyp/vm/virtio_input/src/virtio_input.c`（EL2 内 backend 的形态参照——event 注入型设备）
- 依赖: `virtio/backend`、`virtio/virtq`、runtime `guest_uart`（xhyper 现有 PL011 桥接通路，见 `hypervisor/runtime/src/guest_uart.rs`）

- [ ] **Step 1: 写 tx 测试**（guest 写 `queue_notify` → backend 读描述符 → 字符流出到 mock UART → used 回写 + vIRQ）

```rust
#[test]
fn tx_drains_descriptors_and_writes_to_uart() {
    let mut be = ConsoleBackend::new(mock_uart(), mock_addrspace());
    be.notify_tx();  // 模拟 queue_notify
    assert_eq!(mock_uart.take_output(), b"hello\n");
    assert!(be.used_ring_has_entry(0));
}
```

- [ ] **Step 2: 写 rx 测试**（host 输入注入 → backend 投 rx 描述符 → vIRQ 唤醒 guest）
- [ ] **Step 3: 实现 `ConsoleBackend`**

> **实现指引：**
> - `device_id = VIRTIO_DEVICE_TYPE_CONSOLE (3)`，2 个 virtqueue（rx=0, tx=1）+ 极小 config
> - tx: `queue_notify` → `virtq.read_desc_chain`（read-only 段是 guest 发出的字符）→ 写到 `guest_uart` 出口 → `virtq.write_reply`（used ring）→ `backend.notify(0x4e)` 让 EL2 注入 vIRQ
> - rx: host 输入源（接 xhyper 现有 host 控制台输入路径）→ 投到 rx 队列的 write-only desc → vIRQ
> - **不经 0x4e–0x55 数据面 hypercall**（EL2 内直调 Backend API）——绕开 mgr 数据面 ABI 缺口

- [ ] **Step 4: 跑测试 + Commit**

```bash
git commit -s -m "virtio/console: add EL2-internal console backend with tx/rx queue handling"
```

### Task 2.2: xhyper-mgr `virtual_device` 域唤醒

**Files:**
- Modify: `xhyper-mgr/crates/xhyper-mgr-core/src/domains/platform.rs`（`virtio_mmio: Unavailable` → feature-gated `Available`）
- Modify: `xhyper-mgr/crates/xhyper-mgr-core/src/domains/virtual_device.rs`（唤醒 `realize_virtio_mmio` + console 配置项）
- Modify: `xhyper-mgr/crates/xhyper-abi/src/operation.rs`（确认管理面封装齐全——Stage 1 已在 EL2 实现对应 handler）

- [ ] **Step 1: 在 platform.rs 把 `virtio_mmio` 改成 feature-gated Available**

```rust
#[cfg(feature = "virtio-el2")]
pub const fn virtio_mmio_enabled() -> bool { true }
#[cfg(not(feature = "virtio-el2"))]
pub const fn virtio_mmio_enabled() -> bool { false }
```

- [ ] **Step 2: 在 `virtual_device.rs` 唤醒 `realize_virtio_mmio`**——对照 `realize_vrtc` 的模式（已验证），实现 console 设备的 create/configure/bind_virq/activate 编排
- [ ] **Step 3: 跑 mgr 单测**

Run: `cd /workspace/xhyper-mgr && cargo test -p xhyper-mgr-core --features virtio-el2`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
cd /workspace/xhyper-mgr
git commit -s -m "mgr: wake virtio_mmio orchestration domain for console backend"
```

### Task 2.3: `phase-e2-console` 门禁（闭环里程碑）

**Files:**
- Create: `scripts/phase-e2-console.sh`（参照 `scripts/qemu-secondary-console.sh` 骨架）
- Modify: `Makefile`

- [ ] **Step 1: 写门禁脚本**——构建带 `CONFIG_HYPERVISOR_VIRTIO=y` + `virtio-el2` mgr feature 的镜像；QEMU 启动 Secondary VM；guest 里 `echo hello > /dev/hvc0`，host 侧验证收到 `hello`；host 输入字符，guest 侧验证收到。两轮交互。
- [ ] **Step 2: 跑门禁**

Run: `make phase-e2-console SMP=2 MEM=1g`
Expected: `PASS`

- [ ] **Step 3: 回归全套**

Run: `make phase-e0-vdevice && make phase-e1-probe && make phase-d4-verify`
Expected: 全绿

- [ ] **Step 4: Commit**

```bash
git commit -s -m "scripts: add phase-e2-console gate — virtio-el2 closure milestone"
```

- [ ] **Stage 2 完成标志（= 本计划闭环）：** Secondary guest `/dev/hvc0` 双向交互，不依赖 crosvm、不依赖 Host Linux 内核改动。

---

## Stage 3 · 后续大纲（未完全分解）

**前置条件（必须先完成）：核实 Host Linux `/dev/xhyper` 驱动的数据面 ABI 现状。**

核实方式：拿到 Host Linux 内核源码仓（可能在 futlab gerrit `hv/linux` 之类的项目），grep `/dev/xhyper` 驱动的 ioctl 注册，检查是否覆盖：
1. 0x4e–0x55 数据面 hypercall 的 ioctl 入口
2. backend virq 的用户态投递（irqfd 等价物）
3. memextent 捐赠内存映射到用户态地址空间

根据核实结果分两种路径：

### 路径 A（驱动已齐）：只写用户态 backend daemon
- 写 `xhyper-virtio-backend` 进程：用现有 ioctl 调 0x4e–0x55 + 收 backend virq + 经捐赠映射读写 guest 内存 + 对接 HLOS 块设备驱动（`open`/`read`/`write`）
- 补异步通知（ioeventfd/irqfd 等价物）——若驱动已有则直接用；若驱动没有但 EL2 可加，在 EL2 + 驱动两侧补
- virtio-blk 协议（virtio-blk 请求头解析 + ext4 IOPS 基准）
- `phase-e3-blk` 门禁：guest ext4 读写回读

### 路径 B（驱动缺）：先改 Host Linux 内核，再写 daemon
- 在 `/dev/xhyper` 驱动加上述三项 ioctl/机制（跨仓协调）
- 其余同路径 A

### 待解决的开放问题（影响 Stage 3 拆法）
1. ~~xhyper-abi 数据面 immediate 编号~~ —— **已关闭**（0x49–0x55 与 Gunyah 一致）
2. **blk backend 进程宿主形态**：独立新进程 vs 扩展 `xhyper-vmm`（倾向独立进程，职责单一）
3. **阻塞复位的等待落点**：等 mgr 的哪条路径（hypercall 上下文 vs 独立线程）——涉及 xhyper 线程模型的挂起/唤醒 API 选型

Stage 3 在前置核实 + 开放问题决策后，另出一份独立计划（`docs/superpowers/plans/<date>-xhyper-virtio-stage3-blk.md`）。

---

## 自审

### Spec 覆盖检查

| Spec 节 | 覆盖任务 |
|---|---|
| §3 Stage 0（vdevice 派发 + RO mirror） | Task 0.1–0.7 |
| §3 Stage 1（virtio 四部件 + 管理面 hypercall） | Task 1.1–1.7 |
| §3 Stage 2（console backend + mgr 唤醒） | Task 2.1–2.3 |
| §3 Stage 3（blk + HLOS backend + 异步通知） | 大纲 + 前置条件（未分解） |
| §4.1 并发与内存序 | Task 1.1（status）、1.4（reason 位图 Release/Acquire）、1.5（used ring fence） |
| §4.2 共享 config cache 页 | Task 0.4（RO mirror）+ 1.3（mmio 写 handler 更新页） |
| §4.3 生命周期与错误处理 | Task 1.4（RAII guard）+ 1.6（错误码映射） |
| §4.4 feature-gate | Task 0.5（test_device cfg）+ 2.2（mgr feature） |
| §5 测试与门禁 | phase-e0/e1/e2 三个新门禁 |
| §6 风险 #1（核心路径回归） | Task 0.7 + 每个 Stage 末回归 |
| §6 风险 #2（ABI 语义漂移） | Task 1.6（错误码对齐 Gunyah） |
| §6 风险 #3（阻塞复位死锁） | Task 1.1（status 复位语义）—— 失败测试在 Stage 1；完整阻塞在 Stage 3 开放问题 #3 |

**未覆盖项（诚实声明）：**
- Stage 3 完整任务分解——因 `/dev/xhyper` 驱动现状未核实，无法精确拆解（已在 Stage 3 大纲声明前置条件）
- 阻塞复位的完整 runtime 实现（跨 VM 等待）——model 层状态机覆盖语义，runtime 层阻塞落在 Stage 3 开放问题 #3
- mgr 数据面 ABI 封装（`operation.rs` 补 0x4e–0x55）——Stage 2 console 不需要（EL2 内直调），Stage 3 前置

### 占位符扫描

- `todo!()` 出现在 Task 1.1 Step 5 的 `VirtioStatus::write` 实现中——这是**有意的**：该函数的完整逻辑在 Gunyah `frontend.c:269-368`（168 行 C），逐条翻译是 Task 1.1 的核心工作，plan 里给出了 enum/signature/测试，实现体参照 C 源。这不是"未定义工作"，是"已定义工作 + 已给参照源"。
- "对照 Gunyah XXX" 出现多次——每个都给了精确文件:行号，是移植参照，不是占位符。
- "实现指引"块——给出关键逻辑 + xhyper API 指针 + SAFETY 要求，不是"fill in details"。

### 类型一致性

- `VirtioStatus`（Task 1.1）→ Task 1.3 mmio 写 STATUS 寄存器调 `status.write()` → Task 1.4 backend 收 `driver_ok` 事件 → 一致
- `ReasonBitmap`（Task 1.4）→ Task 2.1 console `notify_tx` 调 `backend.notify()` → 一致
- `VmmioHandler` trait（Task 0.2）→ Task 1.3 `VirtioMmio` impl、Task 0.5 test_device impl → 一致
- `ObjectKind::VirtioBackend`（Task 1.4）→ Task 1.6 hypercall dispatch `resolve_root_capability(_, ObjectKind::VirtioBackend, _)` → 一致

---

## 执行交付

**本计划闭环标准：** `make phase-e2-console` PASS —— Secondary guest `/dev/hvc0` 双向交互，不依赖 crosvm、不依赖 Host Linux 内核改动。

**预估：** Stage 0（1.5–2.5 周）+ Stage 1（4–6 周）+ Stage 2（1–2 周）= **6.5–10.5 周**（一名熟悉 xhyper 的工程师全职，含测试与门禁）。

**下一步：** Stage 3 前置核实（`/dev/xhyper` 驱动现状）→ 开放问题 #2/#3 决策 → 另出 Stage 3 计划。
