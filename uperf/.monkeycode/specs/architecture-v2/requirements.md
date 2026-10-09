# Requirements Document -- uperf-linux Architecture V2

## Introduction

本文件定义 uperf-linux 从 Alpha (v0.1.0) 演进到 v1.0 生产就绪版本的完整需求规划。
当前项目架构完整但存在 4 个严重问题导致核心调度功能不可用。

规划分为四个阶段:
- **Phase 1 (v0.2 Beta)**: 修复阻塞性 bug，打通核心调度链路
- **Phase 2 (v0.3 Stable)**: 完善缺失功能，达成完整测试覆盖
- **Phase 3 (v0.5 Plugin Architecture)**: 插件化架构重构，热插拔调度策略
- **Phase 4 (v1.0 Production)**: 云端配置、远程管理、生产级加固

每个 Phase 的完成标准: 所有测试通过、代码 Review 达标、文档同步更新。

## Glossary

| 术语 | 定义 |
|------|------|
| **Daemon** | uperf-linux 主守护进程 (root 权限) |
| **FSM** | Finite State Machine, 7 场景状态机 |
| **P-F Curve** | Power-Frequency 曲线, 功耗模型核心数据 |
| **uClamp** | Utilization Clamping, Linux cgroup v2 的利用率限制机制 |
| **SM8550** | Snapdragon 8 Gen 2 SoC, 项目主要目标平台 |
| **Plugin** | 可动态加载的 `.so` 模块, 实现调度策略接口 |
| **Policy** | 调度策略, 定义频率/亲和性/uClamp 的调整规则 |
| **Cloud Config** | 从远程服务拉取的 JSON 配置, 支持 A/B 测试 |

---

## Phase 1: v0.2 Beta Readiness

### R1.1 -- Sysfs Writer Apply Implementation

**User Story:** AS a user, I want state machine transitions to trigger actual CPU/GPU frequency changes, so that the scheduler has real effect on system performance.

#### Acceptance Criteria

1. WHEN `sysfs_writer_apply()` is called with `ActionParams`, the system SHALL write mapped values to corresponding sysfs paths.
2. WHEN multiple keys map to the same sysfs path, the system SHALL apply the last value (deduplication).
3. IF a sysfs path does not exist or is not writable, the system SHALL log a WARN message and skip that path.
4. The system SHALL support 5 path types: `PERCPU`, `PERCLUSTER`, `DEVFREQ`, `UCLAMP`, `STRING`.

### R1.2 -- State Machine Preset Loading

**User Story:** AS a user, I want mode switches to load per-mode parameters from JSON config, so that balance/powersave/performance modes have distinct behavior.

#### Acceptance Criteria

1. WHEN `state_machine_set_mode()` is called, the system SHALL load `ActionParams` from `cfg->presets[mode]`.
2. WHEN a mode preset contains scene-specific overrides, the system SHALL use scene-specific values over global defaults.
3. IF the mode preset key does not exist in config, the system SHALL fall back to `initials` defaults.

### R1.3 -- DBus Mode Handler Integration

**User Story:** AS a GUI user, I want clicking power mode buttons to actually change daemon behavior, so that the GUI is not a decorative panel.

#### Acceptance Criteria

1. WHEN DBus `SetMode` method is called, the system SHALL invoke `state_machine_set_mode()` with the requested mode.
2. WHEN mode changes, the system SHALL emit `ModeChanged` signal with the new mode name.
3. IF an invalid mode name is provided, the system SHALL return a DBus error.

### R1.4 -- Touch Distance Calculation Fix

**User Story:** AS a user playing a touch-based game, I want swipe/gesture detection to be accurate, so that the scheduler responds correctly to touch input.

#### Acceptance Criteria

1. WHEN `dist()` computes Euclidean distance between two touch points, the system SHALL use correct y-coordinate subtraction.
2. The system SHALL apply `gesture_thd_x` and `gesture_thd_y` thresholds when classifying touch events.
3. The system SHALL pass all unit tests for touch distance and gesture classification.

### R1.5 -- Heavyload Detector Time Fix

**User Story:** AS a developer, I want heavyload detection to use monotonic time, so that load calculations are immune to system clock changes.

#### Acceptance Criteria

1. WHEN `heavyload_detector_sample()` records a timestamp, the system SHALL use `clock_gettime(CLOCK_MONOTONIC)`.
2. The system SHALL return per-CPU load values in `heavyload_detector_get_cpu_loads()`.
3. The system SHALL pass unit tests for heavyload detection timing and load accuracy.

---

## Phase 2: v0.3 Stable & Feature Complete

### R2.1 -- Config Hot-Reload via SIGHUP

**User Story:** AS an operator, I want to reload config without restarting the daemon, so that tuning changes take effect immediately.

#### Acceptance Criteria

1. WHEN daemon receives SIGHUP signal, the system SHALL re-parse the JSON config file.
2. IF parsing succeeds, the system SHALL atomically replace the active config pointer.
3. IF parsing fails, the system SHALL retain the previous valid config and log an ERROR.
4. The system SHALL validate new config in a child process before applying.

### R2.2 -- Per-PID uClamp Fallback

**User Story:** AS a user on a system without cgroup v2, I want uClamp to still work via per-thread prctl, so that CPU limiting functions on all kernels.

#### Acceptance Criteria

1. WHEN cgroup v2 is unavailable, the system SHALL apply uClamp via `prctl(PR_SCHED_CORE)` or `sched_setattr()`.
2. The system SHALL detect cgroup v2 availability at startup via `/proc/mounts`.
3. The system SHALL log which uClamp path is active at INFO level.

### R2.3 -- GUI Feature Completion

**User Story:** AS a tablet user, I want all GUI panels to be fully functional, so that I can control the scheduler completely from the touch interface.

#### Acceptance Criteria

1. WHEN settings Apply button is clicked, the system SHALL write values to daemon via DBus.
2. WHEN Logs panel is active, the system SHALL display real journald logs for the daemon.
3. WHEN Frequency Override Apply button is clicked, the system SHALL send manual frequency values to daemon via DBus.
4. The GUI SHALL subscribe to DBus signals for real-time updates instead of polling.

### R2.4 -- Module Test Coverage Completion

**User Story:** AS a developer, I want all modules to have complete test coverage, so that refactoring and feature additions are safe.

#### Acceptance Criteria

1. `cgroup_manager` SHALL have unit tests covering create/assign/cleanup/uClamp paths (minimum 15 tests).
2. `heavyload_detector` SHALL have unit tests covering sampling/EMA/averaging/state transitions (minimum 12 tests).
3. `dbus_interface` SHALL have unit tests covering method calls/property gets/signal emission (minimum 10 tests).
4. `input_monitor` SHALL have unit tests covering device discovery/event parsing/gesture classification (minimum 10 tests).
5. `cli` SHALL have integration tests covering all subcommands (minimum 8 tests).

### R2.5 -- Game Scanner Efficiency

**User Story:** AS a user, I want game process scanning to consume minimal CPU, so that the daemon has negligible overhead.

#### Acceptance Criteria

1. WHEN game scanner runs, the system SHALL use `inotify` on `/proc` to watch for new processes.
2. The system SHALL maintain a process cache to avoid re-scanning known processes.
3. The system SHALL batch process scans to run at most once per 10 seconds.

---

## Phase 3: v0.5 Plugin Architecture

### R3.1 -- Plugin Loading Framework

**User Story:** AS a developer, I want to write scheduling plugins as independent shared libraries, so that new SoCs and strategies can be added without modifying daemon core.

#### Acceptance Criteria

1. WHEN daemon starts, the system SHALL scan `/etc/uperf-linux/plugins/` for `.so` files.
2. The system SHALL load each plugin via `dlopen()` and call its init function.
3. IF a plugin fails to load, the system SHALL log an ERROR, skip it, and continue.
4. The system SHALL expose a C ABI plugin interface with version negotiation.
5. The system SHALL unload plugins on daemon shutdown via `dlclose()`.

### R3.2 -- Plugin Interface Definition

**User Story:** AS a plugin developer, I want a stable ABI interface so that my plugins work across uperf-linux versions.

#### Acceptance Criteria

1. The system SHALL define a `PluginVTable` struct with function pointers for lifecycle/strategy/query.
2. The system SHALL provide `get_plugin_info()` returning name, version, and capabilities.
3. The system SHALL provide `get_policy()` returning scheduling actions for current state.
4. The system SHALL provide `on_event()` for event-driven plugin activation.
5. The plugin ABI SHALL be versioned, and the daemon SHALL reject mismatched versions.

### R3.3 -- Core Pluggability

**User Story:** AS an advanced user, I want to swap scheduling strategies at runtime, so that I can experiment with different algorithms without restarting.

#### Acceptance Criteria

1. The system SHALL refactor `power_model`, `state_machine`, and `heavyload_detector` to implement the plugin interface.
2. WHEN a plugin provides a `get_policy()` function, the system SHALL use it as the primary scheduling source.
3. WHEN no plugin is loaded, the system SHALL use the built-in default policy.
4. The system SHALL support runtime plugin reload via DBus `ReloadPlugins` method.

### R3.4 -- Built-in Plugin: SM8550 Default

**User Story:** AS a Xiaomi Pad 6S Pro user, I want the default SM8550 scheduler to work out of the box as a built-in plugin.

#### Acceptance Criteria

1. The system SHALL ship `libuperf_sm8550.so` as the default scheduling plugin.
2. The plugin SHALL implement the SM8550 P-F curve and cluster topology from `config/sm8550.json`.
3. WHEN no other plugin is specified, the system SHALL activate `libuperf_sm8550.so`.
4. The system SHALL register SM8550-specific cpufreq/devfreq/thermal paths.

### R3.5 -- Plugin SDK & Templates

**User Story:** AS a community contributor, I want a plugin SDK with templates, so that I can add support for new SoCs quickly.

#### Acceptance Criteria

1. The system SHALL provide a `uperf-plugin.h` header with the full plugin ABI.
2. The system SHALL provide a `plugin-template/` directory with CMakeLists.txt and skeleton code.
3. The system SHALL provide a `plugin-validate` script that checks ABI compliance.
4. The system SHALL ship a `libuperf_example.so` minimal plugin for reference.

---

## Phase 4: v1.0 Production

### R4.1 -- Cloud Configuration Service

**User Story:** AS an operator, I want to push configuration updates from a remote server, so that I can apply A/B tested tuning profiles fleet-wide.

#### Acceptance Criteria

1. WHEN daemon starts, the system SHALL fetch config from a configured HTTPS endpoint.
2. The system SHALL cache the last successful config locally.
3. IF the remote fetch fails, the system SHALL fall back to the cached config.
4. The system SHALL support config versioning and rollback via DBus.

### R4.2 -- Perfetto Integration

**User Story:** AS a developer, I want to export scheduler traces to Perfetto, so that performance issues can be analyzed visually.

#### Acceptance Criteria

1. WHEN trace collection is enabled, the system SHALL emit scheduler events to a Perfetto-compatible trace file.
2. The system SHALL include timestamps, frequency changes, mode switches, and load data in traces.
3. The system SHALL support starting/stopping traces via DBus.
4. The system SHALL rotate trace files when they exceed 100MB.

### R4.3 -- Multi-Device Management

**User Story:** AS a fleet operator, I want to monitor and control multiple uperf-linux instances from a central dashboard.

#### Acceptance Criteria

1. The system SHALL expose a REST API on localhost for programmatic access.
2. The system SHALL support authentication via API tokens.
3. The system SHALL report device telemetry (temperature, load, power, frequency) via the API.

### R4.4 -- Production Hardening

**User Story:** AS a device manufacturer, I want uperf-linux to meet production quality standards.

#### Acceptance Criteria

1. The system SHALL pass valgrind/AddressSanitizer checks with zero leaks.
2. The system SHALL have a crash recovery mechanism (watchdog timer + auto-restart).
3. The system SHALL impose resource limits (max 50MB RSS, max 5% CPU).
4. The system SHALL provide a security audit report for CAP_SYS_NICE/CAP_SYS_RESOURCE usage.
5. The system SHALL pass 24-hour soak testing without crashes or memory growth.
