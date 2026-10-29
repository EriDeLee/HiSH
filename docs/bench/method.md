# 历史基准方法

单文件源码保留在 `bench/hishbench.c`，日常HAP不带基准程序。实验比较同一工作量下CPU、内存和可用的EGL/GLES离屏GPU操作；不同侧必须保持编译优化、scale和工作量一致。本文不是压力运行授权。

结果保留在 [原生基线](results-native-2026-10-23.md) 和 [同机对比](results-compare-2026-10-23.md)。这些是2026-10-23旧版本，不代表当前mksh或最小底座性能。

GPU可用性取决于各运行时的动态库依赖和命名空间。旧OHOS路径能加载系统GL；Linux ABI路径缺libhilog_encode.so时如实SKIP，不将其概括成“容器没有GPU”。同机GPU多轮差异很大，不能支持个位数百分比性能结论。

旧说明中“HAP随包提供基准”和“HOME编译后不能执行”已经失效。新版本不因保留基准源码而恢复它到发行包；普通ELF执行与内存编译是独立路径，见 [执行兼容层](../elf-runtime.md)。
