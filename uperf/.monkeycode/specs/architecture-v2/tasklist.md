# 需求实施计划 -- Phase 1: v0.2 Beta Readiness

- [x] 1. 实现 sysfs_writer_apply 核心写入逻辑
   - 将 ActionParams 映射到 Config 中定义的 sysfs knob 路径
   - 支持 PERCPU / PERCLUSTER / DEVFREQ / UCLAMP / STRING 五种路径类型
   - 对同一路径的多次写入进行去重 (last-value-wins)
   - ENOENT/EACCES 错误容错处理
   - 对应需求: R1.1

- [x] 2. 实现状态机预设参数加载
  - [x] 2.1 修改 `get_action_params()` 从 `cfg->presets[mode].presets` 加载 ActionParams
  - [x] 2.2 修改 `state_machine_new()` 初始化所有 mode×scene 的 ActionParams

- [x] 3. 集成 DBus 模式处理器

- [x] 4. 修正触摸距离计算 bug 并完善手势分类
  - [x] 4.1 为距离计算和手势分类编写单元测试

- [x] 5. 修复 heavyload_detector 时间与 per-CPU 负载
  - [x] 5.1 将 `time(NULL)` 替换为 `clock_gettime(CLOCK_MONOTONIC)`
  - [x] 5.2 修改 `heavyload_detector_get_cpu_loads()` 返回真实 per-CPU 负载
  - [x] 5.3 为 heavyload_detector 编写单元测试

- [x] 6. 检查点 - 确保所有测试通过并验证功能正确
  - 构建整个项目并运行所有现有测试
  - 验证 sysfs_writer_apply 正确写入模拟 sysfs 路径
  - 验证状态机模式切换后 ActionParams 正确加载
  - 验证 DBus SetMode 真正触发模式变更
  - 验证触摸距离计算正确性
  - 验证 heavyload 检测器时间戳使用 monotonic 时间
