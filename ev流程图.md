根据对 Gunyah 代码库的彻底探索，我已经摸清了它的事件订阅发布机制。下面用一组流程图来说明它的运行方式。

## 核心结论

Gunyah 的「事件订阅发布」**不是运行时动态注册表**，而是一套**编译期 DSL 驱动的静态 pub/sub**：

- 订阅在 `.ev` 文件里声明（编译期绑定）
- 构建时由 Python 代码生成器生成 `trigger_<event>_event()` dispatch 函数
- 运行时业务代码调用 `trigger_*_event()`，按 priority 顺序调用各模块 handler

---

## 图 1：整体架构（构建期 + 运行期）

```mermaid
flowchart TB
    subgraph DSL["① DSL 声明层（手写）"]
        IF["interface *.ev<br/>定义事件名/参数/返回语义"]
        MOD["module *.ev<br/>subscribe 订阅 + priority + handler"]
        OBJ["object.ev.tmpl<br/>对象生命周期事件模板"]
    end

    subgraph GEN["② 代码生成层 tools/events/"]
        PARSE["parser.py<br/>Lark 语法解析"]
        IR["ir.py<br/>IR + 优先级/锁校验"]
        PICKLE["build/events.pickle"]
        TC["c.tmpl → trigger_*.c"]
        TH["triggers.h.tmpl → trigger_*.h"]
        TEH["handlers.h.tmpl → event_handlers.h"]
    end

    subgraph RUN["③ 运行时"]
        PUB["业务代码<br/>trigger_<event>_event()"]
        DISP["生成的 dispatch 函数<br/>按 priority 降序调用"]
        H["各模块 *_handle_*() 实现"]
    end

    IF --> PARSE
    MOD --> PARSE
    OBJ --> PARSE
    PARSE --> IR --> PICKLE
    PICKLE --> TC --> DISP
    PICKLE --> TH
    PICKLE --> TEH --> H
    PUB --> DISP --> H
```

---

## 图 2：五种事件类型的 dispatch 语义

```mermaid
flowchart LR
    T["trigger_<event>_event()"]

    T --> S["Simple Event<br/>event"]
    T --> HE["Handled Event<br/>handled_event"]
    T --> ME["Multi Event<br/>multi_event"]
    T --> SE["Setup Event<br/>setup_event"]
    T --> SL["Selector Event<br/>selector_event"]

    S --> S1["按 priority 顺序<br/>调用所有 handler<br/>返回值忽略"]
    HE --> HE1["链式调用<br/>首个返回值≠default 即停止<br/>并向上游返回"]
    ME --> ME1["各 handler 返回值<br/>从 count 中递减<br/>count=0 时停止"]
    SE --> SE1["顺序执行<br/>任一失败 → 逆序调用 unwinder<br/>回滚已执行的 handler"]
    SL --> SL1["switch(selector)<br/>分发到对应 handler"]
```

---

## 图 3：Boot 冷启动完整链路（最典型的订阅发布场景）

```mermaid
sequenceDiagram
    participant ASM as init_el2.S
    participant BC as boot.c（手写 orchestrator）
    participant GEN as boot.c（生成 dispatch）
    participant P1 as partition_standard
    participant P2 as power/rcu/pgtable/gicv3/...
    participant P3 as boot 模块自身

    ASM->>GEN: trigger_boot_runtime_first_init_event()
    GEN->>P1: partition_standard_handle_boot_runtime_first_init()
    GEN->>P3: idle_handle_boot_runtime_first_init()

    ASM->>BC: boot_cold_init(cpu)
    BC->>GEN: trigger_boot_cpu_early_init_event()
    BC->>GEN: trigger_boot_cold_init_event(cpu)
    Note over GEN: 按 priority 降序 dispatch
    GEN->>P1: partition_standard_handle_boot_cold_init() [priority first]
    GEN->>P3: boot_handle_boot_cold_init() [priority 1000]
    GEN->>P2: power / rcu / pgtable / gicv3 / scheduler / ... [中间各优先级]
    GEN->>P1: partition_standard_boot_create_root_partition() [priority last]
    BC->>GEN: trigger_boot_cpu_cold_init_event(cpu)
    BC->>GEN: trigger_boot_hypervisor_start_event()
    BC->>GEN: trigger_boot_cpu_start_event()
    GEN->>P3: thread_boot_set_idle() 等
```

---

## 图 4：对象生命周期事件链（hypercall 触发路径）

```mermaid
sequenceDiagram
    participant G as Guest VM
    participant HC as hypercall / API
    participant PART as partition_standard
    participant GEN as object.c（生成 dispatch）
    participant SUB as 各模块 handler<br/>thread/scheduler/vcpu/...

    G->>HC: hypercall_object_activate()
    HC->>PART: object_activate()
    PART->>GEN: trigger_object_init_<type>_event()
    PART->>GEN: trigger_object_create_<type>_event()
    Note over GEN: setup_event: 任一失败 → 逆序 unwind
    GEN->>SUB: thread_standard_handle_object_create_thread()
    GEN->>SUB: scheduler_fprr_handle_object_create_thread()
    GEN->+>SUB: vcpu_handle_object_create_thread()
    PART->>GEN: trigger_object_activate_<type>_event()
    GEN->>SUB: 各订阅者 handle_object_activate_<type>()
    Note over PART: deactivate 时: trigger_object_deactivate_<type>_event()<br/>RCU 后: trigger_object_cleanup_<type>_event()
```

---

## 图 5：Guest 交互场景（VirtIO / SMCCC / IPI）

```mermaid
flowchart TB
    subgraph GUEST["Guest VM"]
        GA["执行指令"]
    end

    subgraph TRAP["陷入 Hypervisor"]
        MMIO["MMIO trap<br/>VirtIO queue notify"]
        HVC["SMCCC/HVC 陷阱"]
        IPIHW["硬件 IPI 中断"]
    end

    subgraph BIZ["业务函数（Publisher）"]
        VQ["virtio_queue_notify()"]
        SD["smccc_dispatch_fast_64()"]
        IP["ipi_handle_pending()"]
    end

    subgraph TRIG["trigger_*_event()"]
        TV["trigger_virtio_queue_notify_event()"]
        TS["trigger_smccc_dispatch_fast_64_event()"]
        TI["trigger_ipi_received_event()"]
    end

    subgraph HAND["模块 handler（Subscriber）"]
        VB["virtio_backend / virtio_mmio handler"]
        PSCI["PSCI handler → 再 trigger 子事件"]
        VGIC["vgic_handle_ipi_received_sync()"]
    end

    GA --> MMIO --> VQ --> TV --> VB
    GA --> HVC --> SD --> TS --> PSCI
    GA -.核间.-> IPIHW --> IP --> TI --> VGIC
```

---

## 图 6：模块发布者 / 订阅者全景

```mermaid
flowchart LR
    subgraph PUB["主要 Publisher（事件触发源）"]
        B[boot]
        O[object_standard / partition_standard]
        TH[thread_standard]
        SC[scheduler_fprr / trivial]
        V[vcpu / vcpu_run]
        VF[virtio frontend]
        ME[memextent]
        PW[power]
        IPI[ipi]
        VG[vgic]
        SM[smccc]
    end

    subgraph EV["事件域（interface）"]
        E1[boot_*]
        E2[object_*]
        E3[thread_*]
        E4[scheduler_*]
        E5[vcpu_*]
        E6[virtio_*]
        E7[memextent_*]
        E8[ipi_received]
        E9[virq_check_pending]
        E10[smccc_dispatch_*]
    end

    subgraph SUB["主要 Subscriber"]
        S1[partition_standard / power / pgtable / gicv3 / scheduler / idle / rcu]
        S2[thread_standard / scheduler / vcpu]
        S3[vcpu / vgic / arm_vm_timer / arm_vm_pmu]
        S4[virtio_mmio / virtio_pci / virtio_backend / virtio_iommu]
        S5[memextent / hyp_aspace / addrspace / pgtable]
        S6[doorbell / msgqueue 经 virq_check_pending]
        S7[trace_standard / log_standard / debug]
    end

    B --> E1 --> S1
    O --> E2 --> S2
    TH --> E3 --> S2
    SC --> E4 --> S2
    V --> E5 --> S3
    VF --> E6 --> S4
    ME --> E7 --> S5
    IPI --> E8 --> S6
    VG --> E9 --> S6
    SM --> E10 --> S2
```

---

## 关键要点总结

| 维度 | 说明 |
|------|------|
| **绑定时机** | 编译期（`.ev` DSL 静态绑定），非运行时注册 |
| **触发 API** | `trigger_<event>_event(...)`，由代码生成器产出 |
| **handler 命名** | 默认 `<module>_handle_<event>`，可在 DSL 中用 `handler <func>` 覆盖 |
| **执行顺序** | 按 `priority` 降序；`first`/`last` 为特殊极值 |
| **失败处理** | `setup_event` 类型有 `unwinder`，失败时逆序回滚 |
| **跨 VM？** | 否。事件是 hypervisor 内部回调链；VM 间通信走 hypercall/MMIO trap → 业务函数 → trigger 事件 → handler → virq/msgqueue/共享内存 |
| **`build_preview/events/boot.c`** | 自动生成的 dispatch 预览代码，展示 `trigger_boot_*` 的完整 handler 调用顺序，用于学习/文档 |
| **`hyp/core/boot/src/boot.c`** | 手写的 boot orchestrator，是 `trigger_boot_*_event()` 的实际调用入口 |

如果你想针对某一条链路（例如完整的 `boot_cold_init` handler 优先级表，或 VirtIO hypercall 后端全链路）进一步细化，告诉我具体接口名即可。