这份文档介绍了 OpenHarmony 环境下一个用于增量记录和重放 `malloc`/`free` 等内存分配操作的轻量级工具。以下是该文档的中文翻译：

---

# 极简 Malloc/Free 重放钩子 (Hook)

本目录包含 OpenHarmony 的增量重放实验代码。

## 内存空洞聚合统计

`malloc_hole_hook.c` 是独立于逐事件 replay hook 的低开销聚合统计版本。
它使用 160 个可变步长 size class 将请求向上取整：

* 8 到 256 Byte：步长 8 Byte；
* 256 到 512 Byte：步长 16 Byte；
* 512 到 1024 Byte：步长 32 Byte；
* 1024 到 2048 Byte：步长 64 Byte；
* 2048 到 8192 Byte：步长 128 Byte；
* 8192 到 16384 Byte：步长 256 Byte。

每个分段的起点只出现一次，例如 256 后的下一个桶是272，8192后的下一个桶
是8448。超过16384 Byte
的请求仍按 4096 Byte 向上对齐，并统一计入 `4K+` 报表桶，不再细分。
每次成功分配产生的内部碎片为
`rounded_size - requested_size`。v11 同时保留历史分配流量，并统计采样时刻
仍然存活的请求、对齐后大小和空洞：

```text
live_allocated = live_requested + live_hole
live_hole_rate = live_hole / live_allocated
```

Hook 不再在 `malloc/free` 热路径中逐一计算三套对比规则，而是记录177个
公共存活桶。前176个边界是 Mini160、dfmalloc1.0、dfmalloc2.0 和 jemalloc
全部显式 Size Class 的并集，最后一个桶记录 `>262144`。每个公共桶保存
存活数量和请求字节总量；16384以上的13个桶还保存已经由 Mini 规则计算出的
4 KiB 空洞。可视化后处理使用同一组存活原始请求精确重建四套规则：

* `dfmalloc1.0`：先使用 `src/df.txt` 的28个类到3968，3969～4096
  对齐到4096，4097以后采用与 jemalloc 相同的后续 Size Class，最大显式类
  为262144；
* `dfmalloc2.0`：使用截图给出的 36 个类，最大显式类为 14336；
* `jemalloc`：使用截图给出的 49 个类，其中 16384 后的下一个类为 40960，
  最大显式类为 262144。

三套规则超过各自最后一个显式类以后均按 4 KiB 对齐。特别是图片没有给出
jemalloc 超过 262144 Byte 的规则，因此这里的 4 KiB 行为属于显式假设；
CSV 元信息会记录 `comparison_fallback=4K` 和
`jemalloc_last_explicit_class=262144`。显式范围内每个规则的桶空洞按
`count * class_size - requested_sum` 计算，进入兜底范围后使用采集到的
`4k_hole`；因此采样率为1且没有存活表跟踪失败时，四套结果均为精确值。

真实分配器在构造阶段预解析。正式目标完全不使用 TLS、pthread key 或
pthread_atfork，也不从首次 `malloc` 中懒执行 `dlsym`，以兼容
`appspawndf` 的早期加载路径。累计流量按返回指针哈希到 256 个原子分片。
存活统计使用匿名 `mmap` 创建的固定容量、256 分片开放寻址表，槽位只保存
指针和原始请求大小；hook 自己不调用 `malloc`，也不逐事件写日志。

成功分配插入存活表；`free/free_sized` 查表并扣除对应存活值。成功
`realloc` 删除旧记录并插入新记录，失败则恢复旧记录。找不到的释放仍正常
传给真实分配器，但只增加 `untracked_free`，绝不影响业务进程。进程 PID
发生变化时会清空存活表，因此 appspawn 父进程继承下来的内存不计入子进程；
统计口径是“当前进程启动并完成 hook 初始化以后新分配且尚未释放的内存”。

正式目标不创建后台线程。当前每次成功分配或被跟踪释放都会检查
`CLOCK_MONOTONIC`；第一次事件立即产生初始快照，此后由到达周期后的事件
通过原始系统调用写快照。

四项采集参数直接定义在 `malloc_hole_hook.c` 中，运行时同名环境变量不会
被读取：

```c
#define MINI_HOLE_INTERVAL_SEC 10U
#define MINI_HOLE_LIVE_SAMPLE_RATE 1U
#define MINI_HOLE_CLOCK_CHECK_EVERY 1024U
#define MINI_HOLE_LIVE_CAPACITY (1024U * 1024U * 64U)
```

修改宏以后必须重新编译共享库。当前配置表示目标每 10 秒输出、精确跟踪全部
存活分配、每 1024 次相关事件检查时间、存活表 67108864 个槽位。

构建并运行：

```sh
cmake -S mini -B mini/build
cmake --build mini/build --target mini_malloc_hole_hook

MINI_HOLE_OUTPUT_DIR=/tmp \
LD_PRELOAD="$PWD/mini/build/libmini_malloc_hole_hook.so" \
/path/to/program
```

`MINI_HOLE_LIVE_CAPACITY` 必须是 1024 到 134217728 之间的 2 的幂。
67108864 个槽位对应约 1 GiB 虚拟地址空间；只有访问到的匿名页
才占用物理内存。如果存活表容量或单分片探测上限不足，插件不会阻塞或终止
用户进程，而是在 `total_live_track_failed` 中报告失败；此时存活统计会低估。

对于 RenderService/SceneBoard 这类长期低扰动采集，建议先验证下面的采样
编译配置，然后重新构建：

```c
#define MINI_HOLE_INTERVAL_SEC 3600U
#define MINI_HOLE_LIVE_SAMPLE_RATE 64U
#define MINI_HOLE_CLOCK_CHECK_EVERY 1024U
#define MINI_HOLE_LIVE_CAPACITY (1024U * 1024U)
```

`MINI_HOLE_LIVE_SAMPLE_RATE` 和 `MINI_HOLE_CLOCK_CHECK_EVERY` 都必须是
1 到 65536 之间的 2 的幂。采样率为 1 时存活值精确；采样率为 64 时只跟踪
哈希命中的约 1/64 分配，并将计数和字节乘 64 输出，因此存活值是统计估算，
CSV 元信息会写 `live_values=estimated`。采样只适用于分配数量足够大的长期
进程，小样本和很冷门的桶可能有较大误差。累计的 `period_alloc`、
`period_requested` 和 `period_hole` 仍然精确。

`MINI_HOLE_CLOCK_CHECK_EVERY=1024` 复用已有分片计数，降低时钟读取频率；
快照可能晚于严格的整点边界，直到下一个满足检查条件的分配或采样释放出现。
总量和存活状态不会因此丢失。分配器极限压力基准中，精确存活模式的吞吐开销
在旧版本中约为 30%，采样 1/64 且每 1024 次检查时钟后的配对中位开销
约为 7%。160桶和公共存活直方图都会增加工作量，这些旧数据不能直接作为 v11
结论；真实业务通常低于纯 malloc/free 压测，但必须在目标设备上重新对比
帧率、CPU 和时延后才能长期启用。

当前 v11 精确配置仍以存活表跟踪和分片锁为主要成本。纯 malloc/free 压测
只能用于发现代码级性能回退，不能替代鸿蒙目标机上的启动、帧率、CPU 和时延
评估；部署到 RenderService 前必须先用相同工具链构建并进行控制组对照。

本地对照基准会直接测试当前已经编译进 hook 的配置，并从 CSV 元信息读取
实际数值：

```sh
python3 mini/bench_hole_hook.py \
  --runs 10 --warmups 2 --cpus 0-15
```

日志名为 `mini_hole_<start-realtime-ns>_<pid>.csv`。默认输出目录在
OpenHarmony 上是 `/data/local/tmp`，Linux 上是 `/tmp`；当前周期为 10
秒。CSV 每行同时包含周期/累计流量、释放与跟踪诊断值、快照时刻的
`live_alloc/live_requested/live_allocated/live_hole`、历史 Mini96 桶，
以及177个公共存活桶的 `live_hist_count/live_hist_requested` 和大尺寸
`live_hist_4k_hole`。三套对比规则的空洞只在可视化阶段计算，不再由 Hook
直接写入。正常退出时还会写最终快照。初始命令行包含 `appspawndf`
时，fork server 父进程不统计；应用子进程首次分配或释放发现 PID 已变化后，
清空继承状态并创建以自身 PID 命名的独立 CSV。持续有分配时会按配置周期
产生数据行；没有满足时钟检查条件的分配或采样释放时不会写空行，后续检查
到达时会写出从上次快照至当时的完整累计区间。

### CSV 可视化

`visualize_hole_csv.py` 可将 CSV 转成一个自包含的交互式 HTML 报告，不需要
安装 Python 包，也不需要联网。工具只接受最新的 v11 CSV。顶部和趋势图会
并列比较 Mini160 变长桶、dfmalloc1.0、dfmalloc2.0 和 jemalloc 的当前存活
空洞、预计实际占用和空洞率，不再展示累计产生空洞图。明细区域带时间滑块，
每个时间点同时展示四规则对比；点击规则卡片或明细切换按钮，可以在 Mini160、
dfmalloc1.0、dfmalloc2.0 和 jemalloc 各自的 Size Class 明细之间切换，查看
每桶存活分配、存活空洞、平均空洞和平均空洞率：

```sh
python3 mini/visualize_hole_csv.py \
  mini_hole_<start-realtime-ns>_<pid>.csv
```

默认在 CSV 旁生成同名 `.html`，用浏览器打开即可。也可以指定输出文件：

```sh
python3 mini/visualize_hole_csv.py input.csv \
  -o hole_report.html \
  --summary-json hole_summary.json
```

长时间日志会流式读取并自动聚合，图表和滑块默认最多保留 1500 个点，每个
聚合点的桶明细取该组最后一个真实快照。若要逐条查看一个有 3000 行的 CSV，
使用 `--max-points 3000`；也可继续增大，但生成的 HTML 会随桶数和时间点数
增大。写到一半的不完整行或非数字行会跳过，并在报告底部显示异常行数。

GN 保留三个诊断/回退目标：

* `libhook_malloc_hole_probe`：只运行一个原始系统调用构造函数，不导出
  `malloc/free`；成功后生成
  `/data/local/tmp/mini_hole_probe_<pid>.log`。
* `libhook_malloc_hole_passthrough`：独立的已验证控制组，只透传
  `malloc/free`。
* `libhook_malloc_hole_all_api_passthrough`：透传正式版的全部分配接口，但
  不统计、不写文件、不访问 TLS，也不初始化 pthread/atfork；用于隔离接口
  转发层和统计生命周期层。
* `libhook_malloc_hole_atomic_no_writer`：全部接口和完整桶统计，使用共享原子
  计数；不含 TLS、pthread key、atfork 和 writer。该目标用于验证统计层，
  会在正常退出时写最终快照，但不生成周期行。
* `libhook_malloc_hole_no_writer`：旧 TLS 聚合诊断目标；已确认不兼容
  appspawndf，不应部署。

设备排查顺序为
`probe -> passthrough -> all_api_passthrough -> atomic_no_writer ->
libhook_malloc_hole`。

## 当前阶段

该共享库目前的功能包括：

* 导出并记录：`malloc`, `free`, `calloc`, `realloc`, `free_sized`, `posix_memalign`, `aligned_alloc`, `valloc`, `memalign`, 和 `pvalloc`。
* 记录 `pthread_create`、线程开始/结束、`pthread_join` 和 `pthread_detach`，并给每次线程生命周期分配唯一实例 ID。
* 默认创建一个名为 `/data/local/tmp/mini_replay_<realtime-ns>_<creator-pid>.bin` 的 3 GiB 稀疏文件；可在构建时调整容量，并用 `MINI_REPLAY_PATH` 指定输出路径。
* 使用 `MAP_SHARED` 映射该文件一次。
* 在 `appspawndf` 及其子进程间使用一个共享的原子槽位索引。
* 将每个事件直接写入其 48 字节的 mmap 槽位中。
* 最后通过 `sequence = index + 1` 提交槽位。
* 只用 TLS 保存逻辑线程 ID；不使用日志记录线程或单事件的 `write` 调用。

v3 版事件布局（48 字节）存储了时间戳、旧地址/输入地址、结果地址、大小、PID、逻辑线程实例 ID、函数类型、标志以及最终的提交序列。分配事件将返回的指针存储在 `result` 中；释放事件将释放的指针存储在 `address` 中。`realloc` 在同一个事件中同时存储旧指针和返回的指针。`calloc` 将其计数存储在原本未使用的分配 `address` 字段中；对齐分配函数将对齐方式存储在该字段中，以便离线重放时能还原出比仅记录总字节数更多的信息。

释放事件在调用真实的分配器之前提交。空指针释放和由早期引导分配器拥有的指针不会发送给真实分配器，也不会被记录。

64 字节的 v3 头文件包含了事件容量、共享 `next_index`、初始化状态和运行时标志。事件格式保持 48 字节不变。

3 GiB 的映射空间大约可容纳 6700 万个事件。该文件最初是稀疏的：`ftruncate` 设定其逻辑大小，而物理块在页面写入时分配。

没有任何事件会被静默丢弃。如果映射已满，钩子会设置溢出标志并以退出码 75 终止进程，而不允许在没有记录槽位的情况下继续执行。

初始化失败也是致命的。如果文件无法打开、调整大小、映射或验证，进程将直接退出，而不是在跟踪记录不完整的情况下运行。

OpenHarmony 默认构建使用 `O_EXCL`，因此它从不清除或附加到旧的实验文件。派生的应用进程继承创建者的映射，并继续写入同一个文件。不再需要的旧跟踪记录可以手动删除：

```sh
rm -f /data/local/tmp/mini_replay_*.bin

```

停止实验后，拉取文件并使用以下命令检查：

```sh
mini/build/mini_replay_dump mini_replay.bin

```

将二进制跟踪文件直接转换为文本文件：

```sh
mini/build/mini_replay_dump mini_replay.bin replay.txt

```

将单个进程从 BIN 格式转换为大型项目的 RPLY 格式：

```sh
mini/build/mini_bin_to_rply mini_replay.bin <pid|auto> trace.rply

```

只有记录的 PID 等于 `<pid>` 的分配器事件和线程生命周期事件才会被复制。对于一个独占 BIN，`auto` 会使用第一个 `PROCESS_START` 的 PID。不完整的槽位和未知事件类型会输出到 stderr，计入摘要并跳过，以便剩余的事件仍能被转换。物理上截断的事件是致命的，因为无法可靠地读取下一个记录边界。

将 RPLY 转换为可读文本：

```sh
mini/build/mini_rply_to_txt trace.rply replay.txt

```

RPLY 头文件以 64 位字长存储其长度，因此转换器写入 `idx = record_count * 6`，随后是 48 字节的重放条目。目前的 BIN 格式不记录 CPU 编号，因此转换后的 RPLY 条目使用 CPU 0。PID 过滤不会改变事件顺序。

## 重放

构建并重放 RPLY 跟踪记录，同时显示人类可读的统计信息：

```sh
cmake --build mini/build
mini/build/mini_replay_main -S trace.rply

```

在 OpenHarmony 上，请将所有选项置于跟踪路径之前：

```sh
./libmini_replay_main --stats --progress mini.rply

```

在重放运行时，不要覆盖、截断或重新发送同一个 RPLY 路径。跟踪文件是内存映射的，因此在进程运行时更改其文件长度可能会在 ARM64 上引发 `SIGBUS`。重放程序在映射前会验证头文件索引、整数边界和物理文件大小。

JSON 输出（也可被 `bench_replay.py` 使用）：

```sh
mini/build/mini_replay_main --json trace.rply

```

常用的模式：

```sh
# 当跟踪记录包含不匹配的 free/realloc 事件时报错。
mini/build/mini_replay_main -S --unknown-policy error trace.rply

# 不等待时间戳 epoch，立即运行所有重放工作线程。
mini/build/mini_replay_main -S --free-run trace.rply

# 若某个槽位依赖关系 10 秒无进展，则中止并进行诊断。
mini/build/mini_replay_main -S \
  --dependency-timeout-ms 10000 trace.rply

# 诊断任何重放停滞，包括分配器调用和 epoch 屏障。
mini/build/mini_replay_main -S \
  --stall-timeout-ms 10000 trace.rply

# 在重放期间触碰（touch）已分配的内存。
mini/build/mini_replay_main -S --touch alloc trace.rply

# 显示所有可用选项。
mini/build/mini_replay_main --help

```

## 离线编译的本地重放

### RPLY 驱动的 mstress 风格合成负载

如果目标是比较不同分配器的完成时间，而不是追赶原始时间戳，可以先把 RPLY
压缩成一个分阶段的统计负载：

```sh
python3 mini/rply_to_mstress.py --phases 64 trace.rply trace_mstress
./trace_mstress --json --touch first
```

生成的 ELF 不包含逐事件表。默认每个 phase、每种分配函数最多保留 256 个
真实大小样本，因此即使输入有几千万条记录，模型通常仍只有 KB 到数 MB。
离线分析仍需要维护当前存活地址表；它的内存需求取决于 trace 的峰值存活对象数。

运行逻辑如下：

* 如果 RPLY 含有完整的 `THREAD_CREATE/START/END/JOIN`，离线分析会恢复由
  join 分隔的线程 wave；历史上出现过的 TID 不会被误认为同时活跃。
* 每个 wave 启动时同步一次该 wave 的活跃 worker。wave 之间的 join 是原程序
  线程生命周期的一部分，不属于 phase barrier。缺少完整生命周期信息的旧 RPLY
  会安全地退化为单 wave。
* 每个 worker 独立执行各 phase 的精确函数调用配额，固定 PRNG 只负责打散顺序
  和选择该 phase 的真实大小样本。
* 一次 `malloc/free/realloc` 返回后才发出下一次调用；不按时间戳等待。
* worker 完成当前 phase 后直接进入下一 phase，没有 phase barrier，也不等待较慢
  worker。输出中的 `phase_barriers` 固定为 0。
* 离线分析会把每个匹配的跨线程 free 建成“分配线程 -> 释放线程”路由。
  分配线程把真实返回指针放入目标线程的无锁队列。目标对象暂时没有产生时，
  free 会被记为 debt 并稍后补发，而不是阻塞整个 phase。
* 当前 wave 发完后，只等待该 wave 尚未满足的跨线程对象依赖，然后 join 并进入
  下一 wave；来自早期 wave、由后续 wave 释放的对象会保留在目标移交队列中。

因此，较慢的 allocator 会直接增加总运行时间，不会被固定 trace 时间跨度掩盖。
同一 allocator 对比和不同 allocator 对比都应使用相同生成 ELF、相同 `--seed`
和相同 CPU/系统负载条件。例如：

```sh
./trace_mstress --json --seed 1 --touch none
LD_PRELOAD=/path/to/libjemalloc.so \
  ./trace_mstress --json --seed 1 --touch none
```

自动预热、重复运行、校验工作量并计算内部/整进程 CV：

```sh
python3 mini/synthetic_mstress_bench.py \
  --warmups 3 --runs 30 \
  --cpu-list 0-31 \
  --seed 1 --touch first \
  --output glibc.json \
  ./trace_mstress

python3 mini/synthetic_mstress_bench.py \
  --warmups 3 --runs 30 \
  --cpu-list 0-31 \
  --seed 1 --touch first \
  --allocator /path/to/libjemalloc.so \
  --output jemalloc.json \
  ./trace_mstress
```

`--max-cv-pct 1` 可以作为自动验收门槛，超过时返回状态 4。不同 allocator
必须使用相同 ELF、CPU 列表、seed、touch 模式、预热次数和测量次数。

内存触碰模式可以显式选择：

```sh
./trace_mstress --touch none   # 更集中地测 allocator 调用
./trace_mstress --touch first  # 默认，每个对象触碰首字节
./trace_mstress --touch pages  # 每页触碰一次
./trace_mstress --touch full   # 每个机器字触碰一次，更接近 mstress
```

函数调用次数、每线程/phase 配额和匹配到的跨线程 free 路由是精确的；大小分布是
有界 reservoir sample，因此属于统计近似。phase 表示归一化的操作进度，不表示
墙上时钟。同一 wave 的不同 worker 可以处在不同 phase，这正是无 barrier 模式
允许的真实并发漂移。运行输出中的 `historical_threads`/`threads` 表示历史 TID
总数，`max_wave_threads` 才表示恢复后的最大同时活跃线程数。当前第一版把
`calloc` 还原为 `calloc(1, total_size)`，对齐分配使用
64 字节默认对齐；跨线程 `realloc` 会在生成摘要中单独报告，但尚不按跨线程
对象依赖还原。遇到 descriptor 容量不足时，使用更大的离线余量重新生成：

```sh
python3 mini/rply_to_mstress.py --capacity-factor 1.5 \
  trace.rply trace_mstress
```

下面的逐记录本地回放是另一条路径，适合需要调用顺序、对象生命周期和时间桶
更接近原始 trace 的实验。

对于那些通用重放引擎过于沉重的分配器实验，可以将 RPLY 编译为独立的 ELF：

```sh
python3 mini/trace_to_native.py trace.rply trace_native
./trace_native

```

编译器离线解析跟踪地址、对象生命周期和跨线程所有权。生成的程序嵌入了一个紧凑的操作镜像，并为每个跟踪线程使用一个真实的 pthread。其计时热路径仅包含每个时间戳桶的绝对时间等待、记录的分配器调用以及当另一个跟踪线程消耗该对象时的原子移交。它不会构建实时对象哈希表、运行 epoch 控制器、更新逐操作全局统计信息，也不会启动进度/看门狗线程。

默认的 100 微秒时间戳量子在保持分配大小随时间变化的直方图的同时，避免了在每个密集事件上读取时钟的开销。可以在生成时选择不同的分辨率：

```sh
# 更高的时间保真度，伴随更多的运行时时钟开销。
python3 mini/trace_to_native.py --quantum-us 10 trace.rply trace_native

# 保留每个记录的独立时间戳。
python3 mini/trace_to_native.py --quantum-us 0 trace.rply trace_native

```

RPLY 本身不声明时间戳单位。由当前 `mini_bin_to_rply` 转换的文件包含纳秒（默认值）。对于时间戳为毫秒的旧大型项目 RPLY，请指定单位：

```sh
python3 mini/trace_to_native.py --timestamp-unit ms \
  old_trace.rply trace_native

```

运行时控制不会改变嵌入的操作序列：

```sh
# 尽可能快地执行精确的调用流。
./trace_native --no-timing

# 将记录的时间线拉伸 2 倍。
./trace_native --scale 2

# 在重放设备上使用记录的 CPU ID（如果它们有意义）。
./trace_native --recorded-cpu

```

为了提高可重复性，请按稳定的 worker 顺序把线程绑定到明确的 CPU 集合，
并把启动/控制线程放到另一个 CPU：

```sh
./trace_native --cpu-list 4-11 --controller-cpu 12 --spin-us 100
```

`--cpu-list` 接受逗号分隔的 CPU 和范围。worker 数多于 CPU 数时会循环
分配，并警告多个 worker 共享 CPU。CPU ID 会根据进程允许的 affinity
mask 校验，无效或无权限的 CPU 会直接报错；`--controller-cpu` 不能同时
出现在 `--cpu-list` 中。

不要对当前 mini BIN 转换出的 RPLY 使用 `--recorded-cpu`：BIN 格式没有
记录 CPU ID，转换器会为所有事件写入 CPU 0。此时应使用 `--cpu-list`。

输出的 `elapsed_ms` 是纯回放区间：从所有 worker 的统一释放时刻开始，
到最后一个 worker 完成最后一次 allocator 操作为止。线程创建、镜像 mmap、
`pthread_join`、结果格式化和进程退出均不计入。时间诊断会输出等待过的时间桶
总数、迟到至少 10 us 的时间桶、迟到比例以及平均/最大迟到。自动化测试可以
使用 JSON：

```sh
./trace_native --cpu-list 4-11 --controller-cpu 12 --json
```

使用全新进程反复执行、丢弃预热轮次并进行可重复性验收：

```sh
python3 mini/native_replay_bench.py \
  --warmups 2 --runs 10 \
  --max-cv-pct 1 --max-deviation-pct 1 \
  --max-late-bucket-pct 5 \
  --output native_repeatability.json \
  ./trace_native -- \
  --cpu-list 4-11 --controller-cpu 12 --spin-us 100
```

该工具会输出每轮回放时间，计算样本标准差、CV 和相对均值的最大偏差。
任何验收阈值未满足时返回退出码 4。通过表示当前机器和负载配置下的实测结果，
并不构成硬实时保证。对于很短的 mstress trace，可以尝试把 `--spin-us`
从 100 提高到 500；这会减少唤醒抖动，但会消耗更多专用 CPU 时间。

默认情况下，不匹配的 free/realloc 输入会导致生成失败：将记录进程的原始地址传递给 `free` 是无效的，而静默丢弃调用会破坏所要求的比率。`--allow-unknown` 是一个显式的低保真逃生舱口；它用空输入替换，并标记生成的镜像，以便运行时发出警告。

对于其他目标编译器，使用 `--cc`，并为所需的 sysroot/目标标志重复使用 `--cflag`：

```sh
python3 mini/trace_to_native.py trace.rply trace_native \
  --cc /path/to/aarch64-linux-ohos-clang \
  --cflag=--target=aarch64-linux-ohos

```

本地路径重现了记录的分配器调用次数、函数类型、大小、线程内顺序、跨线程对象转移以及桶状墙上时钟分布。它特意不重现应用计算、非分配器内存访问、锁、I/O 或分配器事件之间的调度器压力。运行时加载器/pthread 启动在定时区域之前也可能进行少量的分配器内部调用；此开销不会随跟踪长度增长。

新的跟踪记录将原始 `calloc` 计数和分配对齐存储在 RPLY `address` 字段（原本未使用）中。旧的 RPLY 文件仍然有效：总的 calloc 字节数被保留，缺失的对齐方式默认为 64 字节。

重放程序检查每个 `pthread_create` 和 `pthread_join`。因此，资源耗尽会产生一个包含工作线程 TID 的错误，而不是让 epoch 调度器无限等待。槽位生成等待默认为 30 秒诊断超时。一个独立的 30 秒无进展看门狗会报告每个未完成工作线程的状态、操作索引、操作类型、槽位、生成周期和分配大小。仅在明确需要无界等待时才传递 `--dependency-timeout-ms 0` 或 `--stall-timeout-ms 0`。

## 分配器基准测试

`bench_replay.py` 会使用 glibc 和 `mini/allocator_dir` 中找到的每个 `.so` 反复运行同一个 RPLY。其默认重放二进制文件是 `mini/build/mini_replay_main`。

```sh
mkdir -p mini/allocator_dir
python3 mini/bench_replay.py trace.rply

```

仅运行一次 glibc 基准：

```sh
python3 mini/bench_replay.py trace.rply -n 1 \
  -a /tmp/empty-allocator-dir

```

运行选定的分配器库并将选项转发给重放程序：

```sh
python3 mini/bench_replay.py trace.rply \
  --only jemalloc \
  --replay-arg=--free-run \
  --replay-arg=--touch \
  --replay-arg=alloc

```

为了进行可重复性测量，基准测试会丢弃两次预热，运行十次测量，并报告 `replay_time_ms` 的样本标准差和变异系数 (CV)。它还在定时运行期间禁用了 200 毫秒的停滞看门狗扫描；依赖项超时保持启用。必要时可以覆盖运行次数：

```sh
python3 mini/bench_replay.py trace.rply --warmups 3 -n 20

```

在异构 OpenHarmony 设备上，显式地将工作线程映射到固定的同类 CPU 集合上，而不是依赖跟踪记录中的 CPU。如果拓扑允许，请将 epoch 控制器保持在单独的 CPU 上：

```sh
python3 mini/bench_replay.py trace.rply --warmups 3 -n 20 \
  --replay-arg=--cpu-list --replay-arg=4-6 \
  --replay-arg=--controller-cpu --replay-arg=7

```

`--cpu-list` 接受逗号分隔的 CPU 和范围。工作线程以稳定的索引顺序轮询分配。亲和性错误是致命的，并包含工作线程角色、TID 和目标 CPU，因此受限的 OpenHarmony 产品不会静默运行未绑定的基准测试。

如果 CV 依然很高，尝试将 `--auto-epoch-target` 设置为 1000, 500, 250 和 100。较少的 epoch 意味着较少的全工作线程条件变量屏障，但时间线还原的粗糙度也会增加。选择满足 CV 目标的最大值，而不是无条件最小化它。

对于受控测试，在编译时定义 `REPLAY_LOG_PATH` 可以保持之前的固定路径行为。CMake 测试构建使用此模式。

解析器仅读取 `next_index` 槽位，而不是扫描整个 3 GiB。只有当存储的序列等于其槽位索引加一时，事件才有效。

## Linux 构建与测试

```sh
cmake -S mini -B mini/build
cmake --build mini/build
ctest --test-dir mini/build --output-on-failure

```

对于本地跟踪：

```sh
rm -f /tmp/mini_replay.bin
cc -std=gnu11 -fPIC -shared \
  -DREPLAY_LOG_PATH='"/tmp/mini_replay.bin"' \
  mini/malloc_free_hook.c -ldl \
  -o /tmp/libmini_replay.so
LD_PRELOAD=/tmp/libmini_replay.so /bin/true
mini/build/mini_replay_dump /tmp/mini_replay.bin

```

## OpenHarmony GN 构建

```gn
"//path/to/mini:libhook_anymem"

```

从 OpenHarmony 源码根目录使用产品的正常构建命令进行构建。共享库目标定义在 `BUILD.gn` 中。


## 稳定机器上的 CV 测试流程

建议在新机器上重新生成 ELF，避免架构和 libc 差异。

### 1. 检查机器环境

```bash
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
nproc
uptime

cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
```

如果允许，测试期间固定为 performance：

```bash
sudo cpupower frequency-set -g performance
cpupower frequency-info
```

优先选择同一 NUMA 节点、同类型的 32 个 CPU。下面假设使用 `0-31`。

### 2. 从 8640 万 RPLY 生成 workload

```bash
TRACE_PATH=log/mstress_68540_0/20260617155029.rply
WORKLOAD_BIN=./mstress_86m_synthetic

python3 mini/rply_to_mstress.py \
  --phases 64 \
  --samples-per-function 256 \
  --capacity-factor 1.25 \
  "$TRACE_PATH" \
  "$WORKLOAD_BIN"
```

正常情况下摘要应接近：

```text
operations=86402085
historical_threads=311
waves=10
remote_free=3985631
```

### 3. 单次完整性检查

```bash
taskset -c 0-31 \
  "$WORKLOAD_BIN" \
  --json --seed 1 --touch first
```

重点检查：

```text
actual_operations == modeled_operations
max_wave_threads == 32
remote_frees == 3985631
phase_barriers == 0
allocation_failures == 0
local_free_underflows == 0
failed == false
```

### 4. 正式 CV 压测

我增加了 [synthetic_mstress_bench.py](/home/whq/Desktop/code_list/test_profiler/mini/synthetic_mstress_bench.py)。

先跑 10 次短测试：

```bash
python3 mini/synthetic_mstress_bench.py \
  --warmups 3 \
  --runs 10 \
  --cpu-list 0-31 \
  --seed 1 \
  --touch first \
  --output glibc_short.json \
  "$WORKLOAD_BIN"
```

再跑 30 次正式测试：

```bash
python3 mini/synthetic_mstress_bench.py \
  --warmups 5 \
  --runs 30 \
  --cpu-list 0-31 \
  --seed 1 \
  --touch first \
  --output glibc_30.json \
  "$WORKLOAD_BIN"
```

强制要求 CV 不超过 1%：

```bash
python3 mini/synthetic_mstress_bench.py \
  --warmups 5 \
  --runs 30 \
  --cpu-list 0-31 \
  --seed 1 \
  --touch first \
  --max-cv-pct 1 \
  --output glibc_30.json \
  "$WORKLOAD_BIN"
```

超过阈值时返回状态码 4。

### 5. 对比其他 allocator

例如 jemalloc：

```bash
python3 mini/synthetic_mstress_bench.py \
  --warmups 5 \
  --runs 30 \
  --cpu-list 0-31 \
  --seed 1 \
  --touch first \
  --allocator /path/to/libjemalloc.so \
  --output jemalloc_30.json \
  "$WORKLOAD_BIN"
```

建议采用以下顺序，检查机器状态是否漂移：

```text
glibc 第一次 → jemalloc → mimalloc → glibc 第二次
```

如果两次 glibc 平均时间相差很大，说明测试期间机器频率、温度或后台负载不稳定。

### 6. 两种推荐负载模式

集中测试 allocator：

```bash
--touch none
```

更接近原始 mstress 的内存读写：

```bash
--touch full
```

建议两套都跑，并且不同 allocator 之间必须保持以下参数完全相同：

```text
同一个 ELF
同一 CPU 列表
同一 seed
同一 touch 模式
同样的 warmups/runs
相同的频率策略
```

最终主要看输出 JSON 中的：

```text
internal.cv_pct
internal.mean_ms
internal.median_ms
internal.robust_cv_pct
internal.trimmed_cv_pct
wall.cv_pct
```

## 只使用 mini 采集 mstress

下面整条链路都在 `mini/` 中完成，不依赖项目根目录的 replay 模块。mini hook 会记录分配事件、线程唯一实例 ID，以及 `THREAD_CREATE/START/END/JOIN/DETACH`。

对于约 8640 万条事件，3 GiB 默认映射不够；构建时使用 6 GiB，最多约可容纳 1.34 亿条事件：

```bash
cmake -S mini -B mini/build-rply \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINI_REPLAY_MAPPING_SIZE=6442450944ULL

cmake --build mini/build-rply --parallel "$(nproc)" --target \
  mini_mstress mini_malloc_free_hook mini_bin_to_rply mini_replay_dump
```

采集 `mini/mstress.c`。`MINI_REPLAY_PATH` 指向一个尚不存在的文件；若文件已存在，hook 会拒绝覆盖：

```bash
BIN_PATH="$PWD/mstress_$(date +%Y%m%d_%H%M%S).bin"

MINI_REPLAY_PATH="$BIN_PATH" \
LD_PRELOAD="$PWD/mini/build-rply/libmini_malloc_free_hook.so" \
"$PWD/mini/build-rply/mini_mstress" 32 50 10
```

把 BIN 转成 RPLY。单进程独占文件可以直接使用 `auto` 自动选择 PID：

```bash
RPLY_PATH="$PWD/mstress_86m.rply"
mini/build-rply/mini_bin_to_rply "$BIN_PATH" auto "$RPLY_PATH"
```

检查线程生命周期和记录数量：

```bash
mini/build-rply/mini_replay_dump "$BIN_PATH" | tail -n 20
mini/build-rply/mini_rply_to_txt "$RPLY_PATH" | tail -n 5
```

最后生成并检查合成负载：

```bash
python3 mini/rply_to_mstress.py \
  --phases 64 \
  --samples-per-function 256 \
  --capacity-factor 1.25 \
  "$RPLY_PATH" \
  ./mstress_synthetic

taskset -c 0-31 ./mstress_synthetic \
  --json --seed 1 --touch first
```

注意预留磁盘空间：8640 万个 48 字节事件的有效 BIN 数据约 4.15 GB，转换后的 RPLY 也约 4.15 GB；同时保留两者时建议至少准备 10 GB 可用空间。


对，后续完全只使用 `mini/`。

你之前报错是因为 `hook_anymem` 不是 mini 的 CMake 目标；mini 中正确目标是 `mini_malloc_free_hook`。

我已经补齐 mini 的采集链：

- 记录 malloc/free 等分配事件
- 记录线程 create/start/end/join/detach
- 使用唯一线程实例 ID
- 支持 `MINI_REPLAY_PATH`
- 支持 `auto` PID 转换
- 6 GiB BIN 可容纳约 1.34 亿条记录
- 新增 `mini_mstress` 构建目标

从 `mini` 目录执行：

```bash
cd /home/whq/Desktop/code_list/test_profiler/mini

cmake -S . -B build-rply \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINI_REPLAY_MAPPING_SIZE=6442450944ULL

cmake --build build-rply --parallel "$(nproc)" --target \
  mini_mstress \
  mini_malloc_free_hook \
  mini_bin_to_rply \
  mini_replay_dump \
  mini_rply_to_txt
```

采集 `mstress 32 50 10`：

```bash
BIN_PATH="$PWD/mstress_$(date +%Y%m%d_%H%M%S)_$$.bin"

MINI_REPLAY_PATH="$BIN_PATH" \
LD_PRELOAD="$PWD/build-rply/libmini_malloc_free_hook.so" \
"$PWD/build-rply/mini_mstress" 32 50 10
```

转换为 RPLY：

```bash
RPLY_PATH="$PWD/mstress_86m.rply"

build-rply/mini_bin_to_rply \
  "$BIN_PATH" auto "$RPLY_PATH"
```

生成合成 workload：

```bash
python3 rply_to_mstress.py \
  --phases 64 \
  --samples-per-function 256 \
  --capacity-factor 1.25 \
  "$RPLY_PATH" \
  "$PWD/mstress_synthetic"
```

运行检查：

```bash
taskset -c 0-31 \
  "$PWD/mstress_synthetic" \
  --json --seed 1 --touch first
```

我已经做过小规模完整测试：`mstress 2 1 2` 成功识别出 2 个 wave、完整的两组线程生命周期、44 次跨线程 free，1822 次操作全部完成，无 underflow 和 allocation failure。全部 3 个自动测试也通过。

完整说明已经更新在 [README.md](/home/whq/Desktop/code_list/test_profiler/mini/README.md:576)。8640 万条记录同时保存 BIN 和 RPLY，建议至少预留 10 GB 磁盘空间。
