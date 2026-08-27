#!/usr/bin/env python3
"""
benchmark_vram.py — VRAM Engine microbenchmarks

Measures allocation throughput, offload/prefetch latency, pool pressure,
borrow timing, and per-pool throughput for the Red Daft VRAM Engine.

Usage:
  python benchmark_vram.py                          # defaults
  python benchmark_vram.py --budget 1024 --iters 5  # custom
  RD_USE_CUDA=1 python benchmark_vram.py            # GPU path
  RD_USE_ROCM=1 python benchmark_vram.py            # AMD path
"""
import argparse, json, sys, time

def _ns():
    return time.perf_counter_ns()

def _us(ns):
    return ns / 1000.0

def _ms(ns):
    return ns / 1_000_000.0

POOL_NAMES = [
    "ModelWeights", "KvCache", "ActivationsTensors", "WorkspaceScratchpad",
    "HostSwapStaging", "EmbeddingBuffers", "QuantizationMetadata",
    "AsyncStreamQueue", "SystemIpcShared", "EmergencyOverflow",
]

def bench_alloc_throughput(rdv, pool, size_bytes, iters):
    """Allocate + deallocate in a tight loop, return ns/op."""
    times = []
    for _ in range(iters):
        t0 = _ns()
        h = rdv.allocate_handle(pool, size_bytes, tag="bm")
        t1 = _ns()
        if h:
            rdv.deallocate(h)
        times.append(t1 - t0)
    return times

def bench_offload_prefetch(rdv, pool, size_bytes, iters):
    """Offload + prefetch round-trip, return list of ns."""
    h = rdv.allocate_handle(pool, size_bytes, tag="bm_op")
    if not h:
        return []
    times = []
    for _ in range(iters):
        t0 = _ns()
        rdv.offload_to_ddr(h, False)
        t1 = _ns()
        rdv.prefetch_to_vram(h, False)
        t2 = _ns()
        times.append((t1 - t0, t2 - t1))
    rdv.deallocate(h)
    return times

def bench_double_buffer(rdv, pool, size_bytes, iters):
    """Double-buffered offload+prefetch, return list of ns."""
    h = rdv.allocate_handle(pool, size_bytes, tag="bm_db")
    if not h:
        return []
    times = []
    for _ in range(iters):
        t0 = _ns()
        rdv.offload_to_ddr(h, True)
        rdv.prefetch_to_vram(h, True)
        t1 = _ns()
        times.append(t1 - t0)
    rdv.deallocate(h)
    return times

def bench_borrow(rdv, pool, borrow_bytes, iters):
    """Borrow memory from emergency pool, return list of ns."""
    times = []
    for _ in range(iters):
        t0 = _ns()
        rdv.borrow_memory(pool, borrow_bytes)
        t1 = _ns()
        times.append(t1 - t0)
    return times

def bench_pool_fill(rdv, pool, chunk_bytes, max_chunks):
    """Fill a pool chunk-by-chunk until OOM, return (chunks_filled, total_bytes, ns)."""
    handles = []
    t0 = _ns()
    for i in range(max_chunks):
        h = rdv.allocate_handle(pool, chunk_bytes, tag=f"fill_{i}")
        if not h:
            break
        handles.append(h)
    t1 = _ns()
    for h in handles:
        rdv.deallocate(h)
    return len(handles), len(handles) * chunk_bytes, t1 - t0

def bench_pool_sweep(rdv, size_bytes, iters):
    """Allocate 1 block in each of 10 pools, measure total time."""
    times = []
    for _ in range(iters):
        handles = []
        t0 = _ns()
        for p in range(10):
            h = rdv.allocate_handle(p, size_bytes, tag="sweep")
            if h:
                handles.append(h)
        t1 = _ns()
        for h in handles:
            rdv.deallocate(h)
        times.append(t1 - t0)
    return times

def percentile(sorted_ns, p):
    idx = int(len(sorted_ns) * p / 100)
    idx = min(idx, len(sorted_ns) - 1)
    return sorted_ns[idx]

def fmt_us(ns_val):
    if ns_val < 1000:
        return f"{ns_val:.0f}ns"
    elif ns_val < 1_000_000:
        return f"{_us(ns_val):.1f}us"
    else:
        return f"{_ms(ns_val):.2f}ms"

def run_vram_benchmarks(rdv, budget_mib, iters, size_kib):
    results = {}
    size_bytes = size_kib * 1024

    print(f"\n{'='*72}")
    print(f" VRAM Engine Benchmarks — budget={budget_mib} MiB, iters={iters}, block={size_kib} KiB")
    print(f"{'='*72}")

    # ── 1. Per-pool allocation throughput ────────────────────────────
    print(f"\n[1/7] Allocation throughput ({size_kib} KiB blocks, {iters} iters)")
    print(f"  {'Pool':<22s} {'median':>8s} {'p95':>8s} {'p99':>8s} {'ops/s':>10s}")
    print(f"  {'-'*22} {'-'*8} {'-'*8} {'-'*8} {'-'*10}")

    for p in range(10):
        times = bench_alloc_throughput(rdv, p, size_bytes, iters)
        if not times:
            print(f"  {POOL_NAMES[p]:<22s} {'SKIP':>8s}")
            continue
        s = sorted(times)
        med = percentile(s, 50)
        p95 = percentile(s, 95)
        p99 = percentile(s, 99)
        ops = 1_000_000_000.0 / med if med > 0 else 0
        print(f"  {POOL_NAMES[p]:<22s} {fmt_us(med):>8s} {fmt_us(p95):>8s} {fmt_us(p99):>8s} {ops:>10.0f}")
        results[f"alloc_{POOL_NAMES[p]}"] = {
            "median_ns": med, "p95_ns": p95, "p99_ns": p99, "ops_per_sec": ops
        }

    # ── 2. Offload / prefetch latency ────────────────────────────────
    print(f"\n[2/7] Offload + Prefetch latency ({size_kib} KiB, {iters} iters)")
    print(f"  {'Pool':<22s} {'off median':>10s} {'off p95':>8s} {'pf median':>10s} {'pf p95':>8s}")
    print(f"  {'-'*22} {'-'*10} {'-'*8} {'-'*10} {'-'*8}")

    for p in [0, 1, 2, 5, 9]:
        trips = bench_offload_prefetch(rdv, p, size_bytes, min(iters, 20))
        if not trips:
            print(f"  {POOL_NAMES[p]:<22s} {'SKIP':>10s}")
            continue
        off = sorted([t[0] for t in trips])
        pf = sorted([t[1] for t in trips])
        print(f"  {POOL_NAMES[p]:<22s} {fmt_us(percentile(off,50)):>10s} {fmt_us(percentile(off,95)):>8s} "
              f"{fmt_us(percentile(pf,50)):>10s} {fmt_us(percentile(pf,95)):>8s}")
        results[f"offload_{POOL_NAMES[p]}"] = {"median_ns": percentile(off,50), "p95_ns": percentile(off,95)}
        results[f"prefetch_{POOL_NAMES[p]}"] = {"median_ns": percentile(pf,50), "p95_ns": percentile(pf,95)}

    # ── 3. Double-buffer throughput ──────────────────────────────────
    print(f"\n[3/7] Double-buffer round-trip ({size_kib} KiB, {iters} iters)")
    for p in [0, 1, 5]:
        times = bench_double_buffer(rdv, p, size_bytes, min(iters, 20))
        if not times:
            print(f"  {POOL_NAMES[p]:<22s} SKIP")
            continue
        s = sorted(times)
        print(f"  {POOL_NAMES[p]:<22s} median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
              f"bandwidth={size_bytes*2/percentile(s,50)*1e9/1024/1024:.0f} MiB/s (off+pf)")
        results[f"doublebuf_{POOL_NAMES[p]}"] = {
            "median_ns": percentile(s,50), "p95_ns": percentile(s,95),
            "bandwidth_mibs": size_bytes*2/percentile(s,50)*1e9/1024/1024
        }

    # ── 4. Borrow latency ────────────────────────────────────────────
    print(f"\n[4/7] Emergency borrow latency (1 MiB chunks, {min(iters,10)} iters)")
    times = bench_borrow(rdv, 1, 1<<20, min(iters, 10))
    if times:
        s = sorted(times)
        print(f"  borrow 1 MiB: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results["borrow_1mib"] = {"median_ns": percentile(s,50), "p95_ns": percentile(s,95)}

    # ── 5. Pool fill (capacity test) ─────────────────────────────────
    print(f"\n[5/7] Pool fill capacity (64 KiB chunks)")
    for p in [0, 1, 9]:
        handles = []
        t0 = _ns()
        try:
            for i in range(10000):
                h = rdv.allocate_handle(p, 64<<10, tag=f"fill_{i}")
                if not h:
                    break
                handles.append(h)
        except Exception as e:
            print(f"  {POOL_NAMES[p]}: stopped at cap ({e})")
        ns = _ns() - t0
        mib = len(handles) * 64 / 1024
        rate = mib / (_ms(ns) / 1000) if ns > 0 else 0
        print(f"  {POOL_NAMES[p]:<22s} {len(handles):>6d} chunks  {mib:>8.1f} MiB  {fmt_us(ns):>8s}  {rate:>8.0f} MiB/s")
        results[f"fill_{POOL_NAMES[p]}"] = {"chunks": len(handles), "total_mib": mib, "time_ns": ns, "rate_mibs": rate}

    # ── 6. Full 10-pool sweep ────────────────────────────────────────
    print(f"\n[6/7] 10-pool sweep ({size_kib} KiB/pool, {iters} iters)")
    times = bench_pool_sweep(rdv, size_bytes, iters)
    if times:
        s = sorted(times)
        print(f"  sweep 10 pools: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
              f"p99={fmt_us(percentile(s,99))}")
        results["sweep_10pool"] = {
            "median_ns": percentile(s,50), "p95_ns": percentile(s,95), "p99_ns": percentile(s,99)
        }

    # ── 7. Stress all pools ──────────────────────────────────────────
    print(f"\n[7/7] Stress all pools (1 MiB each)")
    report = rdv.stress_all_pools()
    print(f"  {report}")
    results["stress_all_pools"] = {"report": str(report)}

    return results

def main():
    parser = argparse.ArgumentParser(description="Red Daft VRAM Engine — Microbenchmarks")
    parser.add_argument("--budget", type=int, default=512, help="VRAM budget MiB (default 512)")
    parser.add_argument("--iters", type=int, default=20, help="iterations per benchmark (default 20)")
    parser.add_argument("--size-kib", type=int, default=64, help="block size KiB (default 64)")
    parser.add_argument("--json", type=str, default=None, help="write results to JSON file")
    parser.add_argument("--csv", type=str, default=None, help="write results to CSV file")
    args = parser.parse_args()

    try:
        import red_daft_vram as rdv
    except ImportError as e:
        print(f"[error] import red_daft_vram failed: {e}")
        print("Build:  pip install pybind11 && pip install -e .  (in vram-engine/)")
        sys.exit(1)

    cfg = rdv.EngineConfig()
    cfg.vram_budget_bytes = args.budget << 20
    cfg.host_budget_bytes = 2048 << 20
    cfg.num_streams = 4
    cfg.enable_double_buffer = True
    rdv.initialize(cfg)

    results = run_vram_benchmarks(rdv, args.budget, args.iters, args.size_kib)

    rdv.print_pool_stats()
    rdv.shutdown()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nResults written to {args.json}")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("benchmark,metric,value\n")
            for k, v in results.items():
                if isinstance(v, dict):
                    for m, val in v.items():
                        if isinstance(val, (int, float)):
                            f.write(f"{k},{m},{val}\n")
                elif isinstance(v, str):
                    f.write(f"{k},report,{v}\n")
        print(f"Results written to {args.csv}")

    print("\nDone.")

if __name__ == "__main__":
    main()
