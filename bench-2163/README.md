# Measurements for eclipse-iceoryx/iceoryx2#2163 (prototype, not for merge)

Prefaulting shared memory pages to avoid one page fault per 4 KiB on the first use of a sample.

* `shm_prefault.c`: raw POSIX shm micro-benchmark that compares none / `posix_madvise(WILLNEED)` /
  `madvise(MADV_POPULATE_*)` / `MAP_POPULATE` / `mlock` / manual touch.
  Build: `gcc -O2 -o shm_prefault shm_prefault.c && ./shm_prefault 15`
* `iox_bench/`: end-to-end iceoryx2 publisher/subscriber benchmark.
  Run: `IOX2_PREFAULT={none|willneed|populate} cargo run --release -- <payload MiB> <samples>`
* The prototype lives in `iceoryx2-bb/posix/src/shared_memory.rs` (`prototype_prefault`) and is gated by
  the `IOX2_PREFAULT` env var.
* `*.csv`: raw results (Linux 6.18, x86_64 VM, 2 vCPUs).
  e2e columns: mode,size_mb,metric,round,us,minor_faults
