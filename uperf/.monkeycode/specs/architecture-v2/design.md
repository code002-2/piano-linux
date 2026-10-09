# Architecture V2 -- uperf-linux 插件化架构升级

Feature Name: architecture-v2
Updated: 2026-07-15

## Description

将 uperf-linux 从单体 systemd 守护进程重构为插件化调度框架, 使调度策略可独立开发、测试和热插拔, 同时修复当前版本的核心功能缺陷, 最终达到 v1.0 生产就绪标准。

升级路径: Alpha (v0.1) -> Beta (v0.2) -> Stable (v0.3) -> Plugin (v0.5) -> Production (v1.0)

---

## Architecture

### v0.1 当前架构 (单体)

```
┌──────────────────────┐
│     main.c (mono)    │
│  ┌──────────────────┐│
│  │config_parser     ││
│  │state_machine     ││
│  │power_model       ││
│  │sysfs_writer      ││
│  │heavyload_detector││
│  │cgroup_manager    ││
│  │input_monitor     ││
│  │game_scanner      ││
│  │dbus_interface    ││
│  │thermal           ││
│  └──────────────────┘│
└──────────────────────┘
```

### v0.5 目标架构 (插件化)

```
┌──────────────────────────────────────────────────────────┐
│                     main.c (Core Engine)                 │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │              Plugin Manager                       │   │
│  │  ┌────────┐  ┌────────┐  ┌────────┐              │   │
│  │  │sm8550.so│  │rk3588.so│  │custom.so│  ...       │   │
│  │  └────────┘  └────────┘  └────────┘              │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │sysfs_wr  │ │cgroup_mgr│ │input_mon │ │dbus_iface│  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │thermal   │ │game_scan │ │config_par│ │   log    │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
└──────────────────────────────────────────────────────────┘
```

插件管理器作为核心引擎和调度策略之间的抽象层, 策略逻辑全部移入 `.so` 插件中。

---

## Components and Interfaces

### 1. Plugin ABI (VTable Interface)

```c
#define UPERF_PLUGIN_ABI_VERSION 1

typedef struct {
    const char *(*get_name)(void);
    uint32_t   (*get_version)(void);
    uint32_t   (*get_abi_version)(void);
    const char *(*get_capabilities)(void);  // JSON string
} PluginInfo;

typedef struct {
    double cpu_margin;
    double cpu_base_sample_time;
    double cpu_burst_slack;
    double gpu_boost_ratio;
    uint32_t heavy_load_threshold;
    uint32_t idle_load_threshold;
} ActionParams;

typedef struct {
    ActionParams params;
    int          cpu_freq_override[8];   // -1 = no override
    uint32_t     gpu_freq_override;      // 0 = no override
} PluginPolicy;

typedef struct {
    // Lifecycle
    int  (*init)(const char *config_path);
    void (*fini)(void);

    // Strategy: given current state, return scheduling decisions
    PluginPolicy *(*get_policy)(
        uint32_t    cpu_loads[8],
        uint32_t    gpu_load,
        const char *current_scene,    // "idle", "touch", "boost", etc.
        const char *current_mode,     // "balance", "powersave", "performance"
        uint32_t    temperature_c
    );

    // Event-driven notifications
    void (*on_scene_change)(const char *from, const char *to);
    void (*on_mode_change)(const char *new_mode);
    void (*on_game_detected)(uint32_t pid, const char *comm);
    void (*on_game_lost)(uint32_t pid);

    // Info
    PluginInfo *(*get_info)(void);
} PluginVTable;
```

### 2. Plugin Manager

```c
typedef struct {
    GList      *plugins;          // loaded plugin handles
    PluginVTable *active_plugin;   // currently active policy source
    const char *plugin_dir;       // /etc/uperf-linux/plugins/
} PluginManager;

PluginManager *plugin_manager_new(const char *plugin_dir);
int            plugin_manager_load_all(PluginManager *pm);
int            plugin_manager_set_active(PluginManager *pm, const char *name);
PluginPolicy  *plugin_manager_get_policy(PluginManager *pm, PluginContext *ctx);
void           plugin_manager_free(PluginManager *pm);
GStrv          plugin_manager_list_plugins(PluginManager *pm);
```

### 3. Core Engine (main.c 改造)

```
main loop (50ms tick):
  config_reload_check()     -- inotify on config file
  heavyload_sample()        -- /proc/stat polling
  input_event_process()     -- evdev epoll events
  thermal_update()          -- thermal zone polling
  game_scanner_tick()       -- 5s interval

  state_machine_tick()      -- FSM transition + timeout
  plugin_context_fill()     -- collect CPU loads, GPU load, scene, mode, temp
  policy = plugin_manager_get_policy(ctx)
  sysfs_writer_apply(policy)  -- write to sysfs
  cgroup_manager_apply(policy) -- update cgroup settings

  dbus_stats_broadcast()    -- push to GUI
```

---

## Data Models

### Plugin Manifest (per-plugin metadata)

```json
{
    "name": "sm8550",
    "version": "1.0.0",
    "abi_version": 1,
    "description": "SM8550 (Snapdragon 8 Gen 2) default scheduler",
    "author": "uperf-linux team",
    "capabilities": ["power_model", "state_machine", "thermal_policy"],
    "config_required": "/etc/uperf-linux/config.json",
    "compatible_socs": ["sm8550", "sm8550-ac", "sm8550-ab"]
}
```

### Plugin Directory Layout

```
/etc/uperf-linux/
├── config.json
├── perapp_powermode
├── plugins/
│   ├── libuperf_sm8550.so
│   ├── libuperf_sm8550.json     (optional manifest)
│   ├── libuperf_rk3588.so
│   └── libuperf_custom.so
└── plugin-cache/
    └── last_good_config.json
```

---

## Correctness Properties

### Invariants

1. **Single active plugin**: 任意时刻只有一个 plugin 提供策略, PluginManager 保证原子切换
2. **Fallback safety**: plugin 加载失败时回退到无操作 (safe default), 不会 panic
3. **ABI version match**: 主版本号不匹配的 plugin 拒绝加载
4. **Policy idempotency**: 连续调用 `get_policy()` 不产生副作用 (pure function)
5. **Memory ownership**: PluginPolicy 由 plugin 分配, 调用者负责释放; PluginManager 不缓存策略

### Constraints

1. Plugin `.so` 不允许链接外部依赖, 仅允许 libc + uperf-plugin.h 声明的接口
2. Plugin 内不允许调用 fork/exec/system
3. Plugin 内不允许创建线程
4. Policy 计算必须在 1ms 内完成 (50ms tick 的安全余量)
5. Plugin 必须通过 `plugin-validate` 脚本的 ABI 合规检查

---

## Error Handling

| 场景 | 处理策略 |
|------|---------|
| plugin `.so` dlopen 失败 | ERROR 日志, 跳过, 继续加载其他插件 |
| plugin `get_info()` 返回 NULL | ERROR 日志, 跳过 |
| ABI 版本不匹配 | ERROR 日志, 告知用户更新 plugin |
| `get_policy()` 返回 NULL | WARN 日志, 使用前一次有效策略 |
| `init()` 失败 | ERROR 日志, 跳过加载 |
| `fini()` 中 crash | 由 dlclose 捕获, 不传播 |
| 所有 plugin 加载失败 | WARN, 使用内置 minimal 策略 (no-op) |
| 运行时 plugin 崩溃 | 信号处理器捕获 SIGSEGV, 禁用该 plugin, 回退默认策略 |
| 运行时 reload 新 plugin 失败 | 保留当前活跃 plugin, 不变更 |

---

## Test Strategy

### Phase 1-2: 缺陷修复 + 单元测试

| 目标模块 | 测试类型 | 用例数 | 关键覆盖 |
|---------|---------|--------|---------|
| sysfs_writer_apply | Unit | 15+ | PERCPU/PERCLUSTER/DEVFREQ/UCLAMP/STRING, 去重, ENOENT/EACCES |
| state_machine presets | Unit | 10+ | 模式加载, 场景覆盖, initials 回退 |
| dbus mode handler | Unit | 8+ | SetMode/GetMode/无效模式/ModeChanged 信号 |
| input_monitor dist() | Unit | 6+ | 距离计算, 手势分类, 边缘检测 |
| heavyload_detector | Unit | 12+ | 采样/EMA/状态转换/per-CPU负载/CLOCK_MONOTONIC |
| cgroup_manager | Unit | 15+ | 层级创建/任务分配/uClamp/prctl fallback/不可用路径 |
| dbus_interface | Unit | 10+ | 方法调用/属性获取/信号发射/对象注册 |
| cli | Integration | 8+ | 所有子命令端到端 |

### Phase 3: 插件系统测试

| 目标 | 测试类型 | 说明 |
|------|---------|------|
| PluginManager load/unload | Unit | 正常加载, 加载失败, 卸载 |
| ABI version check | Unit | 匹配, 不匹配, 缺失 |
| Active plugin switch | Unit | 热切换, 切换失败回退 |
| Plugin sandbox | Unit | dlopen 隔离, 符号冲突 |
| sm8550 plugin | Integration | 端到端: 加载 -> init -> get_policy -> apply |
| example plugin | Integration | 模板生成 -> 编译 -> 加载 -> 验证 |

### Phase 4: 生产测试

| 目标 | 测试类型 | 说明 |
|------|---------|------|
| valgrind/ASan | Static+Dynamic | 零泄漏 |
| Crash recovery | Integration | watchdog 超时 -> 自动重启 |
| 24h soak test | E2E | 持续运行无 crash/内存增长 |
| Cloud config fetch | Integration | HTTP fetch -> parse -> apply -> cache -> fallback |

---

## Implementation Sequence

```
Phase 1 (v0.2):  5 tasks, ~1200 LOC changed
  R1.1 sysfs_writer_apply  -> R1.2 state_machine presets
  -> R1.3 dbus mode handler -> R1.4 dist() fix -> R1.5 heavyload time fix

Phase 2 (v0.3):  5 tasks, ~2000 LOC changed
  R2.1 SIGHUP hot-reload -> R2.2 per-PID uClamp -> R2.3 GUI completion
  -> R2.4 test coverage -> R2.5 game scanner efficiency

Phase 3 (v0.5):  5 tasks, ~2500 LOC new
  R3.1 plugin manager -> R3.2 plugin interface -> R3.3 core refactoring
  -> R3.4 sm8550 plugin -> R3.5 plugin SDK

Phase 4 (v1.0):  4 tasks, ~2000 LOC new
  R4.1 cloud config -> R4.2 Perfetto integration -> R4.3 REST API
  -> R4.4 production hardening
```
