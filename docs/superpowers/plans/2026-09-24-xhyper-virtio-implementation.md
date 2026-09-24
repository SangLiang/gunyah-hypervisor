# XHyper EL2 Virtio 实现计划

> **需求文档：** `docs/superpowers/specs/2026-09-24-xhyper-virtio-requirements.md`
> （定义"做成什么样子"，本文档定义"怎么做成这个样子"）

**Goal:** 实现 Stage 0–2，达到 Secondary guest `/dev/hvc0` 双向交互闭环（不依赖 crosvm、不依赖 Host Linux 内核改动）。

**Architecture:** Stage 0 泛化既有 MMIO 三态派发为多设备注册表 + RO mirror；Stage 1 移植 Gunyah 四部件（model/mmio/backend/virtq）+ 管理面 hypercall；Stage 2 console backend + mgr 域唤醒。详细架构、ABI 合同、机制决策见需求文档。

**Tech Stack:** Rust 1.95 no_std、xhyper crates、`xhyper-dev:2026.08` 容器、QEMU 10.2.1 AArch64 virt、Kconfig + xkmake。

**Baseline:** xhyper `main@d67e6f5e`、xhyper-mgr `main@0a39089`、xhyper-abi `main@cba464c`

**实施约定：** `unsafe` 配 `SAFETY:` 注释；提交 `子系统: 祈使句` + `git commit -s`（DCO）；提交前 `cargo fmt --all && make clippy && make unittest`；model 层纯逻辑先行。

**Gunyah C 移植源：** 本地 `hyp/vm/virtio*` 或服务器 `10.42.27.86:.../gunyah-src/hyp/vm/virtio*`

---

## 文件结构

### 新建 crate（xhyper 仓）

| 路径 | 职责 | 依赖 |
|---|---|---|
| `hypervisor/virtio/model/src/lib.rs` | 纯状态机：VirtioStatus / FeatureNegotiation / QueueState / ConfigUpdate | 无（纯逻辑） |
| `hypervisor/virtio/mmio/src/lib.rs` | MMIO transport：寄存器布局 + 写派发 + 读 mirror | `virtio/model`、`address_space` |
| `hypervisor/virtio/backend/src/lib.rs` | 桥对象：ObjectKind::VirtioBackend + reason 位图 + virq + RAII | `virtio/model`、`object/model`、`irq/vic` |
| `hypervisor/virtio/virtq/src/lib.rs` | 描述符链遍历 + used 回写 | `memory/useraccess` |
| `hypervisor/virtio/console/src/lib.rs` | console backend：rx/tx + guest_uart 出口 | `virtio/backend`、`virtio/virtq` |
| `hypervisor/virtio/test_device/src/lib.rs` | trivial 测试设备（Stage 0 验证用） | `address_space` |

### 修改（xhyper 仓）

| 路径 | 改动 | 阶段 |
|---|---|---|
| `hypervisor/memory/address_space/src/vmmio_registry.rs`（新建） | VMMIO handler 注册表 | 0 |
| `hypervisor/memory/address_space/src/lib.rs` | pub mod + RO 映射 API | 0 |
| `hypervisor/runtime/src/lib.rs` | fault 路径接入注册表；virtio_backend hypercall 分发 | 0/1 |
| `hypervisor/object/model/src/lib.rs` | ObjectKind::VirtioBackend + rights | 1 |
| `hypervisor/runtime/src/object_cleanup.rs` | VirtioBackend prepared/cleanup | 1 |
| `Kconfig`、`platforms/kplat-aarch64/qemu_defconfig` | CONFIG_HYPERVISOR_VIRTIO | 0 |
| `scripts/phase-e0-vdevice.sh`、`phase-e1-probe.sh`、`phase-e2-console.sh` | 新门禁 | 各阶段 |

### 修改（xhyper-mgr 仓，Stage 2）

| 路径 | 改动 |
|---|---|
| `crates/xhyper-mgr-core/src/domains/platform.rs` | virtio_mmio: Unavailable → Available（feature-gated） |
| `crates/xhyper-mgr-core/src/domains/virtual_device.rs` | 唤醒 realize_virtio_mmio + console 配置 |

---

## 前置准备

- [ ] **0.1 建工作分支**

```bash
ssh root@10.42.27.86
cd /media/test/USR-DATA/work/2030/gunyah/xhyper && git checkout -b virtio-el2 origin/main
cd ../xhyper-mgr && git checkout -b virtio-el2 origin/main
```

- [ ] **0.2 容器内验证基线可构建**

```bash
docker run --rm -it --network host \
    -v /media/test/USR-DATA/work/2030/gunyah:/workspace \
    -v xhyper-cargo-cache:/cache -w /workspace/xhyper xhyper-dev:2026.08 bash
cp platforms/kplat-aarch64/qemu_defconfig .config
make defconfig && make build
```
Expected: 构建成功。

- [ ] **0.3 确认 xhyper-abi 快照含 0x49–0x55**

```bash
grep -c HYPERCALL_VIRTIO_BACKEND /workspace/xhyper-abi/generated/hypercalls.rs
```
Expected: `11`。

- [ ] **0.4 提交基线快照**

```bash
git commit --allow-empty -s -m "virtio-el2: baseline snapshot"
```

---

## Stage 0 · EL2 MMIO 派发框架泛化

### Task 0.1: 调研既有派发机制（产出笔记，不写代码）

**Files:** Read `hypervisor/device/vrtc/src/store.rs`（`access_mmio` + `VrtcMmioResult`）、`runtime/src/lib.rs`（grep `vrtc` 派发点）、`memory/address_space/src/fault.rs`（`FaultKind::Vmmio`/`ResumeRequest`）、`device/vrtc/docs/design.md`（`ADD_VMMIO`）

- [ ] **1:** 记录 vRTC 如何注册窗口、runtime 如何在 fault 路径调到 `access_mmio`、三态结果如何映射 `ResumeRequest`。笔记存 `hypervisor/virtio/docs/stage0-survey.md`
- [ ] **2:** 识别泛化点：固定地址特判 → 按 `(addrspace handle, IPA range)` 查注册表
- [ ] **3:** `git add` + `git commit -s -m "virtio: document existing MMIO dispatch for Stage 0"`

### Task 0.2: VMMIO handler 注册表（model 层，TDD）

**Files:** Create `hypervisor/memory/address_space/src/vmmio_registry.rs`、`tests/vmmio_registry.rs`；Modify `src/lib.rs`（pub mod）

- [ ] **1: 写失败测试**

```rust
// tests/vmmio_registry.rs
use hypervisor_address_space::vmmio_registry::*;

struct Dummy;
impl VmmioHandler for Dummy {
    fn handle(&self, _a: VmmioAccess) -> VmmioResult { VmmioResult::Emulated(0xbeef) }
}

#[test]
fn registered_handler_found_by_ipa() {
    let mut r = VmmioRegistry::new();
    r.insert(1, 0x4a000000..0x4a000200, Dummy);
    assert!(r.lookup(1, 0x4a000100).is_some());
}

#[test]
fn unregistered_returns_none() {
    assert!(VmmioRegistry::new().lookup(1, 0x4a000100).is_none());
}

#[test]
fn different_addrspace_isolated() {
    let mut r = VmmioRegistry::new();
    r.insert(1, 0x4a000000..0x4a000200, Dummy);
    assert!(r.lookup(2, 0x4a000100).is_none());
}

#[test]
fn overlapping_insert_rejected() {
    let mut r = VmmioRegistry::new();
    r.insert(1, 0x4a000000..0x4a000200, Dummy).unwrap();
    assert!(r.insert(1, 0x4a000100..0x4a000300, Dummy).is_err());
}
```

- [ ] **2:** `cargo test -p hypervisor-address-space --test vmmio_registry` → FAIL
- [ ] **3: 实现**

```rust
// vmmio_registry.rs
use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::ops::Range;

pub struct VmmioAccess { pub ipa: u64, pub size: u8, pub write: bool, pub write_value: u64 }
pub enum VmmioResult { Unhandled, Fault, Emulated(u64) }

pub trait VmmioHandler { fn handle(&self, access: VmmioAccess) -> VmmioResult; }

pub struct VmmioRegistry { entries: BTreeMap<u64, Vec<(Range<u64>, Box<dyn VmmioHandler>)>> }
impl VmmioRegistry {
    pub const fn new() -> Self { Self { entries: BTreeMap::new() } }
    pub fn insert(&mut self, addrspace: u64, range: Range<u64>, h: impl VmmioHandler + 'static)
        -> Result<(), OverlapError> { /* 检查重叠，插入 */ }
    pub fn lookup(&self, addrspace: u64, ipa: u64) -> Option<&dyn VmmioHandler> { /* 线性搜 */ }
    pub fn remove(&mut self, addrspace: u64, range: &Range<u64>) { /* ... */ }
}
```

- [ ] **4:** 跑测试 → PASS
- [ ] **5:** `git commit -s -m "address_space: add VMMIO handler registry for multi-device dispatch"`

### Task 0.3: runtime fault 路径接入注册表

**Files:** Modify `hypervisor/runtime/src/lib.rs`（VMMIO fault 路径，在 exit-to-VMM 前插查表）

- [ ] **1: 写失败测试**——注册 dummy handler 到某 IPA，构造该 IPA 的 VMMIO fault，断言返回 `ResumeRequest::vmmio_read(value, Default)` 而非 exit-to-VMM。参照 `runtime/tests/vcpu_run_dispatch.rs` 既有 fixture 风格。
- [ ] **2:** `cargo test -p hypervisor-runtime --test vcpu_run_dispatch` → FAIL
- [ ] **3: 集成**——runtime 持有 `VmmioRegistry` 实例；VMMIO fault 处先查注册表，命中调 `handler.handle(access)`，按 `VmmioResult` 映射 `ResumeRequest`（`Emulated(v)` → `vmmio_read(v, Default)` / `vmmio_write(Default)`；`Fault` → `ResumeAction::Fault`；`Unhandled` → 原 exit-to-VMM 路径）。vRTC 既有路径暂不动（保留特判，注册表仅服务新设备）。
- [ ] **4:** 跑测试 → PASS
- [ ] **5:** `make unittest BLK=n` → 全绿（vRTC/crosvm 不受影响）
- [ ] **6:** `git commit -s -m "runtime: consult VMMIO registry before exiting to VMM"`

### Task 0.4: 只读 mirror 映射

**Files:** Modify `address_space/src/lib.rs` + `transaction.rs`

- [ ] **1: 写失败测试**——把 EL2 拥有的页以 RO 映射进 addrspace：读该 IPA → `Retry`（不 fault）；写该 IPA → `FaultKind::Page`（permission fault）。参照 `address_space/tests/fault.rs`
- [ ] **2:** 跑测试 → FAIL
- [ ] **3: 实现**——`transaction.rs` mapping 操作加 `permissions: Stage2Perms::ReadOnly` 选项（对照 `78065eff` 的 private MAP 模式）。物理页来自 memextent 捐赠，EL2 经 hyp 映射 RW 维护页内容。
- [ ] **4:** 跑测试 → PASS
- [ ] **5:** `git commit -s -m "address_space: support read-only mirror mapping for vdevice config pages"`

### Task 0.5: trivial 测试设备

**Files:** Create `hypervisor/virtio/test_device/src/lib.rs`、`tests/model.rs`；Modify `runtime/src/lib.rs`（cfg-gated 注册）

- [ ] **1: 写 model 测试**（写 offset X 后读返回写入值；读 magic offset 返回已知 pattern）
- [ ] **2: 实现** `impl VmmioHandler for TestDevice`
- [ ] **3: runtime 加 `#[cfg(feature = "test_device")]` 注册**
- [ ] **4:** 跑单测 → PASS
- [ ] **5:** `git commit -s -m "virtio: add trivial test device for dispatch validation"`

### Task 0.6: `phase-e0-vdevice` 门禁

**Files:** Create `scripts/phase-e0-vdevice.sh`；Modify `Makefile`

- [ ] **1: 写脚本**——构建带 `CONFIG_TEST_DEVICE=y` 镜像，QEMU 启动到 `xhyper>`，从测试设备读 magic、写计数、读回，断言匹配。参照 `scripts/phase-d4-2-lifecycle.sh` 骨架。
- [ ] **2:** `make phase-e0-vdevice SMP=2 MEM=1g` → PASS
- [ ] **3:** `git commit -s -m "scripts: add phase-e0-vdevice gate"`

### Task 0.7: 回归验证

- [ ] **1:** `make phase-d4-verify` → 全绿（路径 B 零回归）
- [ ] **2:** `make clippy && cargo fmt --all --check` → 无 warning
- [ ] **Stage 0 完成：** `phase-e0-vdevice` PASS + `phase-d4-verify` PASS

---

## Stage 1 · virtio 框架核心

### Task 1.1: `virtio/model` — VirtioStatus 状态机

**Files:** Create `hypervisor/virtio/model/src/lib.rs`、`tests/status.rs`
**Gunyah:** `hyp/vm/virtio/src/frontend.c:269-368`（`virtio_write_status`）

- [ ] **1: 写测试——合法递进**

```rust
#[test]
fn ack_driver_features_ok_driver_ok() {
    let mut s = VirtioStatus::reset();
    assert_eq!(s.write(VirtioStatus::ACKNOWLEDGE.bits()), Ok(StatusWriteEvent::none()));
    assert_eq!(s.write(VirtioStatus::DRIVER.bits()), Ok(_));
    assert_eq!(s.write(VirtioStatus::FEATURES_OK.bits()), Ok(_));
    assert_eq!(s.write(VirtioStatus::DRIVER_OK.bits()), Ok(ev));
    assert!(ev.driver_ok_set);
}
```

- [ ] **2: 写测试——非法递进被拒**

```rust
#[test]
fn driver_without_acknowledge_rejected() {
    let mut s = VirtioStatus::reset();
    assert_eq!(s.write(VirtioStatus::DRIVER.bits()), Err(StatusWriteError::ArgumentInvalid));
}
#[test]
fn unknown_bits_rejected() {
    assert_eq!(VirtioStatus::reset().write(0x80), Err(StatusWriteError::ArgumentInvalid));
}
```

- [ ] **3: 写测试——写 0 触发复位**

```rust
#[test]
fn write_zero_requests_reset() {
    let mut s = VirtioStatus::reset();
    s.write(VirtioStatus::ACKNOWLEDGE.bits()).unwrap();
    s.write(VirtioStatus::DRIVER.bits()).unwrap();
    s.write(VirtioStatus::FEATURES_OK.bits()).unwrap();
    s.write(VirtioStatus::DRIVER_OK.bits()).unwrap();
    let ev = s.write(0).unwrap();
    assert!(ev.reset_requested);
    assert!(s.is_needs_reset());
}
```

- [ ] **4:** `cargo test -p hypervisor-virtio-model --test status` → FAIL
- [ ] **5: 实现**——`bitflags!` 定义 status 位（ACK=1/DRIVER=2/DRIVER_OK=4/FEATURES_OK=8/NEEDS_RESET=0x40/FAILED=0x80）；`write(bits) -> Result<StatusWriteEvent, StatusWriteError>` 按 `frontend.c:269-368` 逐条翻译递进校验
- [ ] **6:** 跑测试 → PASS
- [ ] **7:** `git commit -s -m "virtio/model: add VirtioStatus state machine"`

### Task 1.2: `virtio/model` — Feature/Queue/Config

**Files:** Modify `model/src/lib.rs`；Create `tests/features.rs`、`tests/queue.rs`、`tests/config.rs`
**Gunyah:** `frontend.c`（`virtio_get_dev_features`/`set_drv_features`/`get_queue_info_ptr`/`config_update_begin/end`）

- [ ] **1: Feature 测试**——banked sel：`dev_feat_sel=1` 后 `dev_feat[1]` 可读；`features_ok` 前 `drv_feat` 不生效
- [ ] **2: Queue 测试**——`queue_sel` 选队列、`queue_num` 写入 min(size,max)、`queue_desc` 高低 32 位拼 64 位
- [ ] **3: Config 测试**——`begin` 置 update 标志、generation 读自增；`end` 时 `gen++` + 清标志
- [ ] **4: 实现**——banked 寄存器用 struct + sel 索引；64 位地址 `desc_addr = (high as u64) << 32 | low as u64`
- [ ] **5:** 跑测试 → PASS
- [ ] **6:** `git commit -s -m "virtio/model: add feature negotiation, queue state, config update"`

### Task 1.3: `virtio/mmio` — MMIO transport

**Files:** Create `hypervisor/virtio/mmio/src/lib.rs`、`tests/dispatch.rs`
**Gunyah:** `virtio_mmio/src/virtio_mmio.c`（布局）+ `vdevice.c:280-359`（写派发）

- [ ] **1: 寄存器偏移常量**——`MAGIC_VALUE=0x000`、`STATUS=0x070`、`QUEUE_NOTIFY=0x050`、`DEVICE_CONFIG_BASE=0x100` 等（对照 `virtio_mmio_regs_t`）
- [ ] **2: 写派发测试**——写 `STATUS` → 调 `model.status.write()`；写 `QUEUE_NOTIFY` → 产生通知事件；写 `DRIVER_FEATURES` → 调 `model.features.set_drv()`
- [ ] **3: 读 mirror 测试**——写 `DEVICE_FEATURES_SEL=1` 后读 `DEVICE_FEATURES` 返回 `dev_feat[1]`；写 `STATUS` 后读 `STATUS` 返回新值
- [ ] **4: 实现**——`VirtioMmio` 持有 `&mut VirtioModel` + config cache 页引用，`impl VmmioHandler`；`handle(access)` 按 offset match 派发；写 → 调 model + 更新页内字段；读 → 从页读（fault 路径读返回 `Unhandled`，正常走 RO mirror 不进 handler）
- [ ] **5:** 跑测试 → PASS
- [ ] **6:** `git commit -s -m "virtio/mmio: add MMIO transport with register dispatch and read mirror"`

### Task 1.4: `virtio/backend` — 桥对象

**Files:** Create `hypervisor/virtio/backend/src/lib.rs`、`tests/{lifecycle,notify}.rs`；Modify `object/model/src/lib.rs`（`ObjectKind::VirtioBackend` + rights）；Modify `runtime/src/object_cleanup.rs`（`PreparedVirtioBackendRemove`）
**Gunyah:** `virtio_backend/src/virtio_backend.c`（223 行）

- [ ] **1: object model 加 kind + rights**——`ObjectKind::VirtioBackend`；rights 对照 `hyp/include/hyprights.h` 的 `CAP_RIGHTS_VIRTIO_BACKEND_*`（CONFIG/BIND_MMIO_FRONTEND_VIRQ/BIND_VIRQ 等）
- [ ] **2: reason 位图测试**

```rust
#[test]
fn reason_bitmap_release_acquire() {
    let b = ReasonBitmap::new();
    b.fetch_or(NotifyReason::NEW_BUFFER, Ordering::Release);
    b.fetch_or(NotifyReason::DRIVER_OK, Ordering::Release);
    let r = b.get_and_clear(Ordering::Acquire);
    assert!(r.contains(NEW_BUFFER | DRIVER_OK));
    assert!(b.get_and_clear(Ordering::Acquire).is_empty());
}
```

- [ ] **3: 生命周期测试**——create → configure → bind_virq → activate → deactivate → cleanup；activate 失败 RAII guard 回滚
- [ ] **4: 实现**——`VirtioBackend` 持有 `VirtioModel` + `ReasonBitmap`(AtomicU64) + virq binding + lifecycle 状态；`notify(reason)` → `fetch_or(Release)` + 若新位置位 `virq_assert`；`ActivateGuard` Drop 逆序回滚（对照 `unwind_object_activate`）；`object_cleanup.rs` 加 `PreparedVirtioBackendRemove`（对照 `PreparedVrtcRemove` 模式）
- [ ] **5:** 跑测试 → PASS
- [ ] **6:** `git commit -s -m "virtio/backend: add bridge object with reason bitmap, virq, RAII lifecycle"`

### Task 1.5: `virtio/virtq` — 描述符链 + used 回写

**Files:** Create `hypervisor/virtio/virtq/src/lib.rs`、`tests/desc_chain.rs`
**Gunyah:** `virtio_virtq/src/virtio_virtq.c:16-200`
**依赖:** `hypervisor/memory/useraccess`

- [ ] **1: 描述符链测试**（mock useraccess：read-only 段 + write-only 段 + write 后 read 即断链）
- [ ] **2: used 回写测试**（写回复 → `fence(Release)` → 写 used head idx；验证顺序）
- [ ] **3: 实现**——`Virtq` 持有 addrspace handle + queue 地址；`read_desc_chain`：经 `useraccess_copy_from_guest_ipa` 读描述符，按 `VIRTQ_DESC_F_NEXT` 走链，split VQ 规则；`write_reply`：写回复 → `core::sync::atomic::fence(Ordering::Release)` → 写 used ring `{id,len}` + 自增 `used.idx`。每个 useraccess 调用配 `SAFETY:` 注释
- [ ] **4:** 跑测试 → PASS
- [ ] **5:** `git commit -s -m "virtio/virtq: add descriptor chain walker and used ring writeback"`

### Task 1.6: hypercall 分发集成（管理面）

**Files:** Modify `runtime/src/lib.rs`（分发表加 5 条目）；Create `runtime/tests/virtio_backend_abi.rs`
**Gunyah:** `hyp/interfaces/virtio_backend/virtio_backend.hvc`
**xhyper 现成常量:** `xhyper-abi/generated/hypercalls.rs`

- [ ] **1: 写分发测试**——create → configure(0x49) → bind_virq(0x4c) → activate 完整链；错误码对齐 Gunyah `ERROR_*`。参照 `runtime/src/lib.rs` 既有 `vrtc_abi_dispatch_completes_the_transactional_lifecycle`（第 34114 行）风格
- [ ] **2:** `cargo test -p hypervisor-runtime --test virtio_backend_abi` → FAIL
- [ ] **3: 加分发表条目**

```rust
xhyper_abi::HYPERCALL_PARTITION_CREATE_VIRTIO_BACKEND => objects.create_virtio_backend(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_CONFIGURE => objects.configure_virtio_backend(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_BIND_VIRQ => objects.bind_virtio_backend_virq(registers),
xhyper_abi::HYPERCALL_VIRTIO_BACKEND_UNBIND_VIRQ => objects.unbind_virtio_backend_virq(registers),
```

> handler 按 vRTC 对应 handler 模式（`create_vrtc`/`configure_vrtc_internal`/`bind_vrtc_virq_internal` 见 lib.rs:18343+）对照写。capability 校验 `resolve_root_capability(cap, ObjectKind::VirtioBackend, Rights::*)`。错误码映射对照 `vrtc_store_error`（lib.rs:20839）

- [ ] **4:** 跑测试 + 既有回归 → PASS
- [ ] **5:** `git commit -s -m "runtime: add virtio_backend management hypercall dispatch"`

### Task 1.7: `phase-e1-probe` 门禁

**Files:** Create `scripts/phase-e1-probe.sh`；Modify `Makefile`

- [ ] **1: 写脚本**——构建带 `CONFIG_HYPERVISOR_VIRTIO=y` + stub backend 镜像；QEMU 启动 Secondary VM；验证 guest `dmesg` 出现 virtio_mmio 驱动 probe + `DRIVER_OK` + 队列 ready
- [ ] **2:** `make phase-e1-probe SMP=2 MEM=1g` → PASS
- [ ] **3:** 回归 `phase-e0-vdevice` + `phase-d4-verify` → 全绿
- [ ] **4:** `git commit -s -m "scripts: add phase-e1-probe gate for DRIVER_OK milestone"`
- [ ] **Stage 1 完成：** guest 标准 virtio_mmio 驱动 probe 到 DRIVER_OK、队列 ready

---

## Stage 2 · virtio-console backend

### Task 2.1: console backend（EL2 内）

**Files:** Create `hypervisor/virtio/console/src/lib.rs`、`tests/tx_rx.rs`
**Gunyah:** `hyp/vm/virtio_input/src/virtio_input.c`（EL2 内 backend 形态参照）
**依赖:** `virtio/backend`、`virtio/virtq`、`runtime/src/guest_uart.rs`

- [ ] **1: tx 测试**

```rust
#[test]
fn tx_drains_to_uart() {
    let mut be = ConsoleBackend::new(mock_uart(), mock_addrspace());
    be.notify_tx();
    assert_eq!(mock_uart.take_output(), b"hello\n");
    assert!(be.used_ring_has_entry(0));
}
```

- [ ] **2: rx 测试**——host 输入注入 → 投 rx 描述符 → vIRQ
- [ ] **3: 实现**——`device_id=3`(CONSOLE)，2 virtqueue（rx=0/tx=1）；tx: `queue_notify` → `virtq.read_desc_chain`(read-only 段) → 写 `guest_uart` 出口 → `virtq.write_reply` → `backend.notify(0x4e)`；rx: host 输入 → 投 rx write-only desc → vIRQ。**不经 0x4e–0x55 数据面 hypercall**（EL2 内直调 Backend API）
- [ ] **4:** 跑测试 → PASS
- [ ] **5:** `git commit -s -m "virtio/console: add EL2-internal console backend"`

### Task 2.2: xhyper-mgr 域唤醒

**Files:** Modify `xhyper-mgr/crates/xhyper-mgr-core/src/domains/{platform.rs,virtual_device.rs}`

- [ ] **1:** `platform.rs` 把 `virtio_mmio: Unavailable` 改 feature-gated `Available`
- [ ] **2:** `virtual_device.rs` 唤醒 `realize_virtio_mmio`（对照 `realize_vrtc` 模式）+ console 配置项
- [ ] **3:** `cargo test -p xhyper-mgr-core --features virtio-el2` → PASS
- [ ] **4:** `git commit -s -m "mgr: wake virtio_mmio orchestration for console backend"`

### Task 2.3: `phase-e2-console` 门禁（闭环里程碑）

**Files:** Create `scripts/phase-e2-console.sh`；Modify `Makefile`

- [ ] **1: 写脚本**——构建带 `CONFIG_HYPERVISOR_VIRTIO=y` + `virtio-el2` mgr feature 镜像；QEMU 启动 Secondary VM；guest `echo hello > /dev/hvc0` → host 验证收到；host 输入 → guest 验证收到。两轮。参照 `scripts/qemu-secondary-console.sh` 骨架
- [ ] **2:** `make phase-e2-console SMP=2 MEM=1g` → PASS
- [ ] **3:** 回归 `phase-e0-vdevice && phase-e1-probe && phase-d4-verify` → 全绿
- [ ] **4:** `git commit -s -m "scripts: add phase-e2-console gate — virtio-el2 closure"`
- [ ] **Stage 2 完成（= 本计划闭环）：** `/dev/hvc0` 双向交互，不依赖 crosvm、不依赖 Host Linux 内核改动

---

## Stage 3 · 后续大纲（未分解）

**前置（必须先完成）：** 核实 Host Linux `/dev/xhyper` 驱动数据面 ABI——是否暴露 0x4e–0x55 ioctl + backend virq 投递 + memextent 捐赠映射。决定路径 A（只写 daemon）还是路径 B（改内核 + 写 daemon）。

**待解决：** 开放问题 #2（backend 宿主形态）、#3（阻塞复位落点）。详见需求文档 §9。

Stage 3 前置核实 + 开放问题决策后，另出独立计划。

---

## 自审

### 需求覆盖

| 需求文档节 | 覆盖任务 |
|---|---|
| §2 Stage 0 | Task 0.1–0.7 |
| §2 Stage 1 | Task 1.1–1.7 |
| §2 Stage 2 | Task 2.1–2.3 |
| §5 ABI 合同 | Task 1.6（分发 0x49/0x4c/0x4d） |
| §6.3 机制决策 | 1.1(status)/1.4(reason Release/Acquire)/1.5(used fence)/0.4(RO mirror)/1.4(RAII) |
| §7 约束（feature-gate/零回归/Rust 规范/model 先行） | Task 0.5(cfg)/0.7(回归)/每任务(SAFETY+commit)/model 层纯逻辑 |
| §8 验收 | phase-e0/e1/e2 三门禁 |
| §9 开放问题 | Stage 3 大纲声明 |

### 占位符

- `todo!()` 无（Task 1.1 的 `write()` 实现参照 `frontend.c:269-368` 逐条翻译，给了签名+测试+参照源——是已定义工作）
- "对照 Gunyah XXX" 均给精确文件:行号——移植参照，非占位符
- "实现指引" 块给关键逻辑 + xhyper API 指针——非 "fill in details"

### 类型一致性

`VirtioStatus`(1.1) → mmio 写 STATUS 调 `status.write()`(1.3) → backend 收 `driver_ok` 事件(1.4) ✓
`ReasonBitmap`(1.4) → console `notify_tx` 调 `backend.notify()`(2.1) ✓
`VmmioHandler`(0.2) → `VirtioMmio` impl(1.3) + `TestDevice` impl(0.5) ✓
`ObjectKind::VirtioBackend`(1.4) → dispatch `resolve_root_capability(_, VirtioBackend, _)`(1.6) ✓
