# 原生基线 2026-10-23

配置：`--scale 1`（历史方法见 [基准说明](method.md)）。

设备：FMR0223825041853，HarmonyOS HongMeng Kernel 1.13.0 aarch64，8 核，4 KiB 页。
运行：`hish-bench-ohos --label native`（OHOS 运行时，3 次取中位数）。
GPU：Mali-G910 | HUAWEI | OpenGL ES 3.2 B312。

| 项 | 数值 | 单位 |
| --- | ---: | --- |
| int64-mix | 419.6 | Mops/s |
| int32-scan | 6012.6 | Mops/s |
| double-chain | 652.8 | Mops/s |
| float32-wide | 522.3 | Mops/s（4 宽 NEON，每次迭代算 1） |
| branch-mix | 1302.7 | Mops/s |
| int64-mt（8 线程） | 1979.3 | Mops/s |
| mem-read | 24.8 | GB/s |
| mem-write | 42.0 | GB/s |
| mem-copy | 30.7 | GB/s（读+写） |
| mem-latency | 125.7 | ns/访问 |
| mem-fault | 1445 | MB/s |
| malloc-small | 325.4 | Mops/s（64 B） |
| gpu-fill | 1.915 | Gpix/s |
| gpu-vertex | 293.5 | Mtris/s |

同一次安装里的 `hish-bench`（Linux musl 运行时）CPU/内存量级一致，GPU 因
`libEGL.so` 依赖链缺 `libhilog_encode.so` 记 SKIP。容器一侧用同一份
`bench/hishbench.c` 编译后对照这张表。
