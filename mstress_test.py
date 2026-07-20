import os
import statistics
import subprocess
import time

warmups = 5
runs = 30
cpu_list = "0-31"

cmd = [
    "taskset", "-c", cpu_list,
    "./build-rply/mini_mstress",
    "32", "50", "10",
]

env = os.environ.copy()
env.pop("MINI_REPLAY_PATH", None)
env.pop("LD_PRELOAD", None)  # 测试系统默认分配器，不加载采集 hook

values = []

for index in range(warmups + runs):
    begin = time.monotonic_ns()
    subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=True,
    )
    elapsed_ms = (time.monotonic_ns() - begin) / 1_000_000

    if index < warmups:
        print(f"warmup {index + 1}/{warmups}: {elapsed_ms:.3f} ms")
    else:
        values.append(elapsed_ms)
        print(f"measured {len(values)}/{runs}: {elapsed_ms:.3f} ms")

mean = statistics.mean(values)
stdev = statistics.stdev(values)
cv = stdev / mean * 100

print()
print(f"mean   = {mean:.3f} ms")
print(f"median = {statistics.median(values):.3f} ms")
print(f"stdev  = {stdev:.3f} ms")
print(f"CV     = {cv:.3f}%")
print(f"min    = {min(values):.3f} ms")
print(f"max    = {max(values):.3f} ms")