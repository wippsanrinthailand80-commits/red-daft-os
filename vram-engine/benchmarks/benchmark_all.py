#!/usr/bin/env python3
"""
benchmark_all.py — Unified benchmark runner for Red Daft VRAM + LoRa engines

Runs VRAM microbenchmarks, LoRa microbenchmarks, and a combined VRAM+LoRa
pipeline benchmark. Outputs machine-readable CSV and/or JSON alongside
human-readable console output.

Usage:
  python benchmark_all.py                             # run everything
  python benchmark_all.py --vram-only                 # VRAM only
  python benchmark_all.py --lora-only                 # LoRa only
  python benchmark_all.py --json results.json         # JSON output
  python benchmark_all.py --csv results.csv           # CSV output
  python benchmark_all.py --budget 1024 --rank 32     # custom params
  RD_USE_CUDA=1 python benchmark_all.py               # GPU path
  RD_USE_ROCM=1 python benchmark_all.py               # AMD path

Benchmark suites:
  VRAM (7 tests):   alloc throughput, offload/prefetch, double-buffer, borrow,
                     pool fill, 10-pool sweep, stress all pools
  LoRa (8 tests):   register, hot-swap by ID, hot-swap by name, LRU evict,
                     evict all, SGMV throughput, capacity, stats
  Combined (3 tests): VRAM→LoRa pipeline, concurrent pool+adapter stress,
                     full teardown/rebuild cycle
"""
import argparse, json, os, sys, time

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

LORA_POOL_NAMES = {1: "SystemControl", 2: "ReasoningLogic", 3: "CodingSyntax", 4: "ConversationLang"}

def percentile(sorted_vals, p):
    idx = int(len(sorted_vals) * p / 100)
    return sorted_vals[min(idx, len(sorted_vals) - 1)]

def fmt_us(ns_val):
    if ns_val < 1000:
        return f"{ns_val:.0f}ns"
    elif ns_val < 1_000_000:
        return f"{_us(ns_val):.1f}us"
    else:
        return f"{_ms(ns_val):.2f}ms"

def fill_data(count, seed):
    return [float(((seed * 1337 + i * 7 + 42) % 1000) / 1000.0 - 0.5) for i in range(count)]

# ═══════════════════════════════════════════════════════════════════
#  VRAM Benchmarks
# ═══════════════════════════════════════════════════════════════════

def run_vram_suite(rdv, budget_mib, iters, size_kib):
    results = {}
    size_bytes = size_kib * 1024

    print(f"\n{'='*72}")
    print(f" SUITE 1: VRAM Engine — budget={budget_mib} MiB, block={size_kib} KiB, iters={iters}")
    print(f"{'='*72}")

    # 1. Per-pool alloc throughput
    print(f"\n[1/7] Allocation throughput")
    print(f"  {'Pool':<22s} {'median':>8s} {'p95':>8s} {'ops/s':>10s}")
    print(f"  {'-'*22} {'-'*8} {'-'*8} {'-'*10}")
    for p in range(10):
        times = []
        for _ in range(iters):
            t0 = _ns()
            h = rdv.allocate_handle(p, size_bytes, tag="bm")
            t1 = _ns()
            if h:
                rdv.deallocate(h)
            times.append(t1 - t0)
        s = sorted(times)
        med = percentile(s, 50)
        ops = 1e9 / med if med > 0 else 0
        print(f"  {POOL_NAMES[p]:<22s} {fmt_us(med):>8s} {fmt_us(percentile(s,95)):>8s} {ops:>10.0f}")
        results[f"alloc_{POOL_NAMES[p]}_median_ns"] = med
        results[f"alloc_{POOL_NAMES[p]}_ops"] = ops

    # 2. Offload/prefetch
    print(f"\n[2/7] Offload + Prefetch")
    for p in [0, 1, 2, 9]:
        h = rdv.allocate_handle(p, size_bytes, tag="bm_op")
        if not h:
            continue
        off_times, pf_times = [], []
        for _ in range(min(iters, 20)):
            t0 = _ns(); rdv.offload_to_ddr(h, False); t1 = _ns()
            rdv.prefetch_to_vram(h, False); t2 = _ns()
            off_times.append(t1 - t0)
            pf_times.append(t2 - t1)
        rdv.deallocate(h)
        off_s = sorted(off_times)
        pf_s = sorted(pf_times)
        bw = size_bytes * 2 / percentile(off_s, 50) * 1e9 / 1024 / 1024 if percentile(off_s, 50) > 0 else 0
        print(f"  {POOL_NAMES[p]:<22s} off={fmt_us(percentile(off_s,50)):>8s} pf={fmt_us(percentile(pf_s,50)):>8s} bw={bw:>8.0f} MiB/s")
        results[f"offload_{POOL_NAMES[p]}_ns"] = percentile(off_s, 50)
        results[f"prefetch_{POOL_NAMES[p]}_ns"] = percentile(pf_s, 50)
        results[f"bandwidth_{POOL_NAMES[p]}_mibs"] = bw

    # 3. Double-buffer
    print(f"\n[3/7] Double-buffer round-trip")
    for p in [0, 1]:
        h = rdv.allocate_handle(p, size_bytes, tag="bm_db")
        if not h:
            continue
        times = []
        for _ in range(min(iters, 20)):
            t0 = _ns()
            rdv.offload_to_ddr(h, True)
            rdv.prefetch_to_vram(h, True)
            times.append(_ns() - t0)
        rdv.deallocate(h)
        s = sorted(times)
        print(f"  {POOL_NAMES[p]:<22s} median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results[f"doublebuf_{POOL_NAMES[p]}_ns"] = percentile(s, 50)

    # 4. Borrow
    print(f"\n[4/7] Emergency borrow (1 MiB)")
    times = []
    for _ in range(min(iters, 10)):
        t0 = _ns(); rdv.borrow_memory(1, 1<<20); times.append(_ns() - t0)
    if times:
        s = sorted(times)
        print(f"  borrow: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results["borrow_1mib_ns"] = percentile(s, 50)

    # 5. Pool fill
    print(f"\n[5/7] Pool fill capacity (64 KiB chunks)")
    for p in [0, 1, 9]:
        handles = []
        t0 = _ns()
        for i in range(10000):
            h = rdv.allocate_handle(p, 64<<10, tag=f"fill_{i}")
            if not h: break
            handles.append(h)
        ns = _ns() - t0
        mib = len(handles) * 64 / 1024
        for h in handles: rdv.deallocate(h)
        print(f"  {POOL_NAMES[p]:<22s} {len(handles):>5d} chunks  {mib:>6.1f} MiB  {fmt_us(ns):>8s}")
        results[f"fill_{POOL_NAMES[p]}_chunks"] = len(handles)
        results[f"fill_{POOL_NAMES[p]}_mib"] = mib

    # 6. 10-pool sweep
    print(f"\n[6/7] 10-pool sweep")
    sweep_times = []
    for _ in range(iters):
        hs = []
        t0 = _ns()
        for p in range(10):
            h = rdv.allocate_handle(p, size_bytes, tag="sweep")
            if h: hs.append(h)
        sweep_times.append(_ns() - t0)
        for h in hs: rdv.deallocate(h)
    s = sorted(sweep_times)
    print(f"  sweep: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
    results["sweep_10pool_ns"] = percentile(s, 50)

    # 7. Stress all pools
    print(f"\n[7/7] Stress all pools")
    report = rdv.stress_all_pools()
    print(f"  {report}")
    results["stress_all_pools"] = str(report)

    return results

# ═══════════════════════════════════════════════════════════════════
#  LoRa Benchmarks
# ═══════════════════════════════════════════════════════════════════

def run_lora_suite(lora, rank, in_feat, out_feat, adapter_count, iters):
    results = {}
    a_elems = rank * in_feat
    b_elems = out_feat * rank

    print(f"\n{'='*72}")
    print(f" SUITE 2: LoRa Brain Engine — rank={rank} in={in_feat} out={out_feat} adapters={adapter_count}")
    print(f"{'='*72}")

    # 1. Register adapters
    print(f"\n[1/8] Register adapters")
    adapter_ids = []
    for i in range(adapter_count):
        pool = 1 + (i % 4)
        a_data = fill_data(a_elems, i * 5 + 1)
        b_data = fill_data(b_elems, i * 5 + 2)
        t0 = _ns()
        aid = lora.register_lora(f"bm_{i}", pool, rank, in_feat, out_feat, a_data, b_data)
        t1 = _ns()
        if aid:
            adapter_ids.append(aid)
            info = lora.find_by_id(aid)
            kb = info.total_size / 1024 if info else 0
            print(f"  [{i+1:>2d}] id={aid:>3d} {LORA_POOL_NAMES.get(pool,'?'):<18s} {kb:>7.1f} KiB  {fmt_us(t1-t0)}")
            results[f"register_{i}_ns"] = t1 - t0
            results[f"register_{i}_kb"] = kb
    results["registered_count"] = len(adapter_ids)

    # 2. Hot-swap by ID
    print(f"\n[2/8] Hot-swap by ID")
    swap_times = []
    for _ in range(iters):
        for aid in adapter_ids:
            t0 = _ns()
            lora.swap_to_id(aid)
            t1 = _ns()
            lora.synchronize()
            swap_times.append(t1 - t0)
    if swap_times:
        s = sorted(swap_times)
        print(f"  swap: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  p99={fmt_us(percentile(s,99))}")
        results["swap_by_id_median_ns"] = percentile(s, 50)
        results["swap_by_id_p95_ns"] = percentile(s, 95)

    # 3. Hot-swap by name
    print(f"\n[3/8] Hot-swap by name")
    for pool_id, pool_name in LORA_POOL_NAMES.items():
        swap_fn = {
            1: lora.swap_to_system_lora,
            2: lora.swap_to_reasoning_lora,
            3: lora.swap_to_coding_lora,
            4: lora.swap_to_conversation_lora,
        }[pool_id]
        times = []
        for _ in range(iters):
            t0 = _ns(); swap_fn(); t1 = _ns(); lora.synchronize()
            times.append(t1 - t0)
        s = sorted(times)
        print(f"  {pool_name:<20s} median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results[f"swap_{pool_name}_ns"] = percentile(s, 50)

    # 4. LRU eviction
    print(f"\n[4/8] LRU eviction")
    evict_times = []
    for _ in range(min(iters, 10)):
        for aid in adapter_ids:
            lora.swap_to_id(aid)
            lora.synchronize()
        t0 = _ns(); lora.evict_lru(); t1 = _ns(); lora.synchronize()
        evict_times.append(t1 - t0)
    if evict_times:
        s = sorted(evict_times)
        print(f"  evict LRU: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results["lru_evict_ns"] = percentile(s, 50)

    # 5. Evict all
    print(f"\n[5/8] Evict all")
    evict_all_times = []
    for _ in range(min(iters, 10)):
        for aid in adapter_ids:
            lora.swap_to_id(aid)
            lora.synchronize()
        t0 = _ns(); lora.evict_all(); t1 = _ns(); lora.synchronize()
        evict_all_times.append(t1 - t0)
    if evict_all_times:
        s = sorted(evict_all_times)
        print(f"  evict all: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results["evict_all_ns"] = percentile(s, 50)

    # 6. SGMV throughput
    print(f"\n[6/8] SGMV throughput")
    input_data = fill_data(in_feat, 99)
    for aid in adapter_ids:
        info = lora.find_by_id(aid)
        if not info: continue
        times = []
        for _ in range(iters):
            output = [0.0] * out_feat
            t0 = _ns()
            lora.apply_lora_weights(aid, output, input_data, 1)
            times.append(_ns() - t0)
        s = sorted(times)
        bytes_per_op = (rank * in_feat + out_feat * rank) * 4
        gbps = bytes_per_op / percentile(s, 50) * 1e9 / 1e9 if percentile(s, 50) > 0 else 0
        print(f"  {info.name:<20s} median={fmt_us(percentile(s,50))}  {gbps:.2f} GB/s")
        results[f"sgmv_{info.name}_ns"] = percentile(s, 50)
        results[f"sgmv_{info.name}_gbps"] = gbps

    # 7. Capacity
    print(f"\n[7/8] Capacity test (max 50)")
    cap_ids = []
    for i in range(50):
        a_data = fill_data(a_elems, i * 3 + 1)
        b_data = fill_data(b_elems, i * 3 + 2)
        aid = lora.register_lora(f"cap_{i}", 1 + (i % 4), rank, in_feat, out_feat, a_data, b_data)
        if not aid: break
        cap_ids.append(aid)
    print(f"  Max adapters: {len(cap_ids)}  per adapter: {(a_elems+b_elems)*4/1024:.1f} KiB")
    for aid in cap_ids: lora.unregister(aid)
    results["max_adapters"] = len(cap_ids)

    # 8. Stats
    print(f"\n[8/8] Final stats")
    stats = lora.lora_stats()
    print(f"  swaps={stats.total_swaps} evictions={stats.total_evictions} avg_swap={stats.avg_swap_time_us:.1f}us")
    results["total_swaps"] = stats.total_swaps
    results["total_evictions"] = stats.total_evictions

    return results

# ═══════════════════════════════════════════════════════════════════
#  Combined VRAM + LoRa Pipeline
# ═══════════════════════════════════════════════════════════════════

def run_combined_suite(rdv, lora_mod, rank, in_feat, out_feat, iters):
    results = {}

    print(f"\n{'='*72}")
    print(f" SUITE 3: Combined VRAM + LoRa Pipeline")
    print(f"{'='*72}")

    a_elems = rank * in_feat
    b_elems = out_feat * rank

    # 1. VRAM→LoRa pipeline: allocate VRAM pool, register adapter, swap, apply, deallocate
    print(f"\n[1/3] VRAM→LoRa pipeline (allocate→register→swap→apply→deallocate)")
    pipeline_times = []
    for i in range(iters):
        t0 = _ns()

        # Allocate in VRAM pool
        h = rdv.allocate_handle(0, 64<<10, 0, f"pipe_{i}")

        # Register LoRa adapter
        a_data = fill_data(a_elems, i * 7 + 1)
        b_data = fill_data(b_elems, i * 7 + 2)
        aid = lora_mod.register_lora(f"pipe_{i}", 1 + (i % 4), rank, in_feat, out_feat, a_data, b_data)

        # Swap to adapter
        if aid:
            lora_mod.swap_to_id(aid)
            lora_mod.synchronize()

            # Apply SGMV
            input_data = fill_data(in_feat, i)
            output = [0.0] * out_feat
            lora_mod.apply_lora_weights(aid, output, input_data, 1)

            # Cleanup
            lora_mod.unregister(aid)

        if h:
            rdv.deallocate(h)

        pipeline_times.append(_ns() - t0)

    s = sorted(pipeline_times)
    print(f"  pipeline: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
          f"min={fmt_us(s[0])}  max={fmt_us(s[-1])}")
    results["pipeline_median_ns"] = percentile(s, 50)
    results["pipeline_p95_ns"] = percentile(s, 95)

    # 2. Concurrent stress: fill all 10 VRAM pools + register 4 LoRa adapters simultaneously
    print(f"\n[2/3] Concurrent pool + adapter stress")
    t0 = _ns()

    # Fill VRAM pools
    vram_handles = []
    for p in range(10):
        h = rdv.allocate_handle(p, 128<<10, tag=f"conc_{p}")
        if h: vram_handles.append(h)

    # Register LoRa adapters
    lora_ids = []
    for i in range(4):
        a_data = fill_data(a_elems, i * 11 + 1)
        b_data = fill_data(b_elems, i * 11 + 2)
        aid = lora_mod.register_lora(f"conc_{i}", 1 + i, rank, in_feat, out_feat, a_data, b_data)
        if aid: lora_ids.append(aid)

    # Swap through all adapters
    for aid in lora_ids:
        lora_mod.swap_to_id(aid)
        lora_mod.synchronize()

    # Cleanup
    for aid in lora_ids: lora_mod.unregister(aid)
    for h in vram_handles: rdv.deallocate(h)

    concurrent_ns = _ns() - t0
    print(f"  concurrent: {fmt_us(concurrent_ns)} ({len(vram_handles)} pools + {len(lora_ids)} adapters)")
    results["concurrent_ns"] = concurrent_ns

    # 3. Full teardown + rebuild
    print(f"\n[3/3] Teardown + rebuild cycle")
    times = []
    for _ in range(min(iters, 5)):
        t0 = _ns()
        rdv.shutdown()
        rdv.initialize()
        t1 = _ns()
        times.append(t1 - t0)
    s = sorted(times)
    print(f"  rebuild: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
    results["rebuild_median_ns"] = percentile(s, 50)

    return results

# ═══════════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Red Daft VRAM + LoRa — Unified Benchmark Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python benchmark_all.py                      # run all suites
  python benchmark_all.py --vram-only          # VRAM suite only
  python benchmark_all.py --lora-only          # LoRa suite only
  python benchmark_all.py --json out.json      # machine-readable output
  python benchmark_all.py --csv out.csv        # spreadsheet-friendly
  RD_USE_CUDA=1 python benchmark_all.py        # NVIDIA GPU
  RD_USE_ROCM=1 python benchmark_all.py        # AMD GPU
""")
    parser.add_argument("--budget", type=int, default=512, help="VRAM budget MiB (default 512)")
    parser.add_argument("--iters", type=int, default=20, help="iterations per benchmark (default 20)")
    parser.add_argument("--size-kib", type=int, default=64, help="VRAM block size KiB (default 64)")
    parser.add_argument("--rank", type=int, default=16, help="LoRA rank (default 16)")
    parser.add_argument("--in-feat", type=int, default=256, help="LoRA input features (default 256)")
    parser.add_argument("--out-feat", type=int, default=256, help="LoRA output features (default 256)")
    parser.add_argument("--adapters", type=int, default=4, help="LoRA adapter count (default 4)")
    parser.add_argument("--vram-only", action="store_true", help="run VRAM suite only")
    parser.add_argument("--lora-only", action="store_true", help="run LoRa suite only")
    parser.add_argument("--json", type=str, default=None, help="write JSON results to file")
    parser.add_argument("--csv", type=str, default=None, help="write CSV results to file")
    args = parser.parse_args()

    run_vram = not args.lora_only
    run_lora = not args.vram_only

    # ── Header ───────────────────────────────────────────────────────
    print("=" * 72)
    print(" Red Daft OS — Unified Benchmark Suite")
    print(f" Budget: {args.budget} MiB  Block: {args.size_kib} KiB  Rank: {args.rank}  Iters: {args.iters}")
    print(f" Suites: {'VRAM + ' if run_vram else ''}{'LoRa + ' if run_lora else ''}Combined" if (run_vram and run_lora) else f" Suite: {'VRAM' if run_vram else 'LoRa'}")
    print("=" * 72)

    all_results = {}
    t_start = _ns()

    # ── Import modules ───────────────────────────────────────────────
    rdv = None
    lora_mod = None

    if run_vram:
        try:
            import red_daft_vram as rdv_mod
            rdv = rdv_mod
        except ImportError as e:
            print(f"[error] red_daft_vram not available: {e}")
            if run_vram and not run_lora:
                sys.exit(1)
            run_vram = False

    if run_lora:
        try:
            import red_daft_lora as lora_pkg
            lora_mod = lora_pkg
        except ImportError as e:
            print(f"[error] red_daft_lora not available: {e}")
            if run_lora and not run_vram:
                sys.exit(1)
            run_lora = False

    if not run_vram and not run_lora:
        print("[error] Neither red_daft_vram nor red_daft_lora available.")
        print("Build:  pip install pybind11 && pip install -e .  (in vram-engine/)")
        sys.exit(1)

    # ── Initialize engines ───────────────────────────────────────────
    if run_vram:
        cfg = rdv.EngineConfig()
        cfg.vram_budget_bytes = args.budget << 20
        cfg.host_budget_bytes = 2048 << 20
        cfg.num_streams = 4
        cfg.enable_double_buffer = True
        rdv.initialize(cfg)
        print("[init] VRAM engine initialized")

    if run_lora:
        lora_mod.initialize()
        print("[init] LoRa engine initialized")

    # ── Run suites ───────────────────────────────────────────────────
    if run_vram:
        vram_results = run_vram_suite(rdv, args.budget, args.iters, args.size_kib)
        all_results["vram"] = vram_results

    if run_lora:
        lora_results = run_lora_suite(lora_mod, args.rank, args.in_feat, args.out_feat, args.adapters, args.iters)
        all_results["lora"] = lora_results

    if run_vram and run_lora:
        combined_results = run_combined_suite(rdv, lora_mod, args.rank, args.in_feat, args.out_feat, args.iters)
        all_results["combined"] = combined_results

    # ── Summary ──────────────────────────────────────────────────────
    total_ns = _ns() - t_start
    print(f"\n{'='*72}")
    print(f" SUMMARY")
    print(f"{'='*72}")
    print(f"  Total time:     {fmt_us(total_ns)} ({_ms(total_ns)/1000:.1f}s)")
    if run_vram:
        print(f"  VRAM pools:     10 (alloc/offload/prefetch/borrow/fill/sweep/stress)")
    if run_lora:
        print(f"  LoRa adapters:  {args.adapters} (register/swap/evict/SGMV/capacity)")
    if run_vram and run_lora:
        print(f"  Combined:       pipeline/concurrent/rebuild")

    # ── Print pool stats ─────────────────────────────────────────────
    if run_vram:
        rdv.print_pool_stats()
    if run_lora:
        lora_mod.print_stats()

    # ── Shutdown ─────────────────────────────────────────────────────
    if run_lora:
        lora_mod.shutdown()
    if run_vram:
        rdv.shutdown()

    # ── Output ───────────────────────────────────────────────────────
    if args.json:
        with open(args.json, "w") as f:
            json.dump(all_results, f, indent=2, default=str)
        print(f"\nJSON → {args.json}")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("suite,benchmark,metric,value\n")
            for suite, benchs in all_results.items():
                if isinstance(benchs, dict):
                    for k, v in benchs.items():
                        if isinstance(v, dict):
                            for m, val in v.items():
                                if isinstance(val, (int, float)):
                                    f.write(f"{suite},{k},{m},{val}\n")
                        elif isinstance(v, (int, float)):
                            f.write(f"{suite},{k},value,{v}\n")
        print(f"CSV  → {args.csv}")

    print("\nDone.")

if __name__ == "__main__":
    main()
