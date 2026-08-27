#!/usr/bin/env python3
"""
benchmark_lora.py — LoRa Brain Engine microbenchmarks

Measures hot-swap latency, registration speed, LRU eviction timing,
SGMV throughput, and adapter capacity for the Red Daft LoRa Brain Engine.

Usage:
  python benchmark_lora.py                          # defaults
  python benchmark_lora.py --rank 32 --in-feat 512 # larger adapters
  RD_USE_CUDA=1 python benchmark_lora.py            # GPU path
  RD_USE_ROCM=1 python benchmark_lora.py            # AMD path
"""
import argparse, json, random, sys, time

def _ns():
    return time.perf_counter_ns()

def _us(ns):
    return ns / 1000.0

def _ms(ns):
    return ns / 1_000_000.0

POOL_NAMES = {1: "SystemControl", 2: "ReasoningLogic", 3: "CodingSyntax", 4: "ConversationLang"}

def fill_data(count, seed):
    """Generate deterministic float data."""
    return [float(((seed * 1337 + i * 7 + 42) % 1000) / 1000.0 - 0.5) for i in range(count)]

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

def bench_register(lora, name, pool, rank, in_feat, out_feat, a_data, b_data, iters):
    """Register + unregister in a tight loop, return list of ns."""
    times = []
    for i in range(iters):
        t0 = _ns()
        aid = lora.register_lora(f"{name}_{i}", pool, rank, in_feat, out_feat, a_data, b_data)
        t1 = _ns()
        if aid:
            lora.unregister(aid)
        times.append(t1 - t0)
    return times

def bench_hot_swap(lora, adapters, iters):
    """Hot-swap between adapters in round-robin, return list of ns."""
    if len(adapters) < 2:
        return []
    times = []
    idx = 0
    for _ in range(iters):
        aid = adapters[idx % len(adapters)]
        t0 = _ns()
        lora.swap_to_id(aid)
        t1 = _ns()
        lora.synchronize()
        times.append(t1 - t0)
        idx += 1
    return times

def bench_hot_swap_named(lora, iters):
    """Hot-swap using named convenience methods, return dict of ns lists."""
    pools = [
        ("system", lambda: lora.swap_to_system_lora()),
        ("reasoning", lambda: lora.swap_to_reasoning_lora()),
        ("coding", lambda: lora.swap_to_coding_lora()),
        ("conversation", lambda: lora.swap_to_conversation_lora()),
    ]
    results = {}
    for name, fn in pools:
        times = []
        for _ in range(iters):
            t0 = _ns()
            fn()
            t1 = _ns()
            lora.synchronize()
            times.append(t1 - t0)
        results[name] = times
    return results

def bench_lru_eviction(lora, adapters, iters):
    """Evict LRU adapter, re-swap, measure eviction time."""
    times = []
    for i in range(iters):
        # Ensure we have multiple adapters in VRAM by swapping through them
        for aid in adapters:
            lora.swap_to_id(aid)
            lora.synchronize()

        t0 = _ns()
        lora.evict_lru()
        t1 = _ns()
        lora.synchronize()
        times.append(t1 - t0)
    return times

def bench_evict_all(lora, adapters, iters):
    """Evict all non-active adapters, measure time."""
    times = []
    for _ in range(iters):
        # Swap through all to put them in VRAM
        for aid in adapters:
            lora.swap_to_id(aid)
            lora.synchronize()

        t0 = _ns()
        lora.evict_all()
        t1 = _ns()
        lora.synchronize()
        times.append(t1 - t0)
    return times

def bench_sgmv_throughput(lora, adapters, rank, in_feat, out_feat, iters):
    """Apply LoRA weights (SGMV) on each adapter, measure time + throughput."""
    input_data = fill_data(in_feat, 99)
    results = {}
    for aid in adapters:
        info = lora.find_by_id(aid)
        if not info:
            continue
        times = []
        bytes_per_op = (rank * in_feat + out_feat * rank) * 4  # A + B matrices, float32
        for _ in range(iters):
            output = [0.0] * out_feat
            t0 = _ns()
            lora.apply_lora_weights(aid, output, input_data, 1)
            t1 = _ns()
            times.append(t1 - t0)
        s = sorted(times)
        med = percentile(s, 50)
        gbps = bytes_per_op / med * 1e9 / 1e9 if med > 0 else 0
        results[info.name] = {
            "median_ns": med, "p95_ns": percentile(s, 95),
            "bytes_per_op": bytes_per_op, "throughput_gbps": gbps
        }
    return results

def bench_adapter_capacity(lora, rank, in_feat, out_feat, max_adapters):
    """Register adapters until we run out of host memory, measure total time."""
    a_elems = rank * in_feat
    b_elems = out_feat * rank
    ids = []
    times = []
    for i in range(max_adapters):
        a_data = fill_data(a_elems, i * 3 + 1)
        b_data = fill_data(b_elems, i * 3 + 2)
        t0 = _ns()
        aid = lora.register_lora(f"cap_{i}", 1 + (i % 4), rank, in_feat, out_feat, a_data, b_data)
        t1 = _ns()
        if not aid:
            break
        ids.append(aid)
        times.append(t1 - t0)

    # Report
    total_ns = sum(times)
    avg_ns = total_ns / len(times) if times else 0
    per_adapter_kb = (a_elems + b_elems) * 4 / 1024

    for aid in ids:
        lora.unregister(aid)

    return len(ids), per_adapter_kb, avg_ns, total_ns

def run_lora_benchmarks(lora, rank, in_feat, out_feat, adapters_count, iters):
    results = {}
    a_elems = rank * in_feat
    b_elems = out_feat * rank

    print(f"\n{'='*72}")
    print(f" LoRa Brain Engine Benchmarks — rank={rank} in={in_feat} out={out_feat} iters={iters}")
    print(f"{'='*72}")

    # ── 1. Register adapters ─────────────────────────────────────────
    print(f"\n[1/8] Register {adapters_count} adapters (rank={rank}, {in_feat}x{out_feat})")
    adapter_ids = []
    reg_times = []
    for i in range(adapters_count):
        pool = 1 + (i % 4)
        a_data = fill_data(a_elems, i * 5 + 1)
        b_data = fill_data(b_elems, i * 5 + 2)
        name = f"bench_adpt_{i}"
        t0 = _ns()
        aid = lora.register_lora(name, pool, rank, in_feat, out_feat, a_data, b_data)
        t1 = _ns()
        if aid:
            adapter_ids.append(aid)
            reg_times.append(t1 - t0)
            info = lora.find_by_id(aid)
            kb = info.total_size / 1024 if info else 0
            print(f"  [{i+1:>2d}/{adapters_count}] id={aid:>3d} pool={POOL_NAMES.get(pool,'?'):<18s} "
                  f"size={kb:>8.1f} KiB  reg={fmt_us(t1-t0)}")
            results[f"register_{name}"] = {"pool": pool, "size_kb": kb, "time_ns": t1 - t0}

    if reg_times:
        s = sorted(reg_times)
        print(f"  Registration: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
              f"total={fmt_us(sum(reg_times))}")
        results["register_summary"] = {
            "median_ns": percentile(s,50), "p95_ns": percentile(s,95), "count": len(reg_times)
        }

    # ── 2. Hot-swap by ID ───────────────────────────────────────────
    print(f"\n[2/8] Hot-swap by ID ({len(adapter_ids)} adapters, {iters} cycles)")
    times = bench_hot_swap(lora, adapter_ids, iters)
    if times:
        s = sorted(times)
        print(f"  swap by ID: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
              f"p99={fmt_us(percentile(s,99))}  min={fmt_us(s[0])}  max={fmt_us(s[-1])}")
        results["swap_by_id"] = {
            "median_ns": percentile(s,50), "p95_ns": percentile(s,95), "p99_ns": percentile(s,99),
            "min_ns": s[0], "max_ns": s[-1]
        }

    # ── 3. Hot-swap by name ──────────────────────────────────────────
    print(f"\n[3/8] Hot-swap by named pool ({iters} cycles each)")
    named = bench_hot_swap_named(lora, iters)
    for pool_name, t in named.items():
        if t:
            s = sorted(t)
            print(f"  {pool_name:<16s} median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
            results[f"swap_named_{pool_name}"] = {"median_ns": percentile(s,50), "p95_ns": percentile(s,95)}

    # ── 4. LRU eviction ─────────────────────────────────────────────
    print(f"\n[4/8] LRU eviction ({iters} rounds)")
    times = bench_lru_eviction(lora, adapter_ids, iters)
    if times:
        s = sorted(times)
        print(f"  evict LRU: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}")
        results["lru_evict"] = {"median_ns": percentile(s,50), "p95_ns": percentile(s,95)}

    # ── 5. Evict all ────────────────────────────────────────────────
    print(f"\n[5/8] Evict all non-active ({iters} rounds)")
    times = bench_evict_all(lora, adapter_ids, iters)
    if times:
        s = sorted(times)
        print(f"  evict all: median={fmt_us(percentile(s,50))}  p95={fmt_us(percentile(s,95))}  "
              f"evicted={len(adapter_ids)-1} adapters/round")
        results["evict_all"] = {
            "median_ns": percentile(s,50), "p95_ns": percentile(s,95),
            "adapters_per_round": len(adapter_ids) - 1
        }

    # ── 6. SGMV throughput ───────────────────────────────────────────
    print(f"\n[6/8] SGMV weight patching throughput ({iters} iters)")
    print(f"  {'Adapter':<20s} {'median':>8s} {'p95':>8s} {'throughput':>12s}")
    print(f"  {'-'*20} {'-'*8} {'-'*8} {'-'*12}")
    sgmv = bench_sgmv_throughput(lora, adapter_ids, rank, in_feat, out_feat, iters)
    for name, v in sgmv.items():
        print(f"  {name:<20s} {fmt_us(v['median_ns']):>8s} {fmt_us(v['p95_ns']):>8s} "
              f"{v['throughput_gbps']:>10.2f} GB/s")
        results[f"sgmv_{name}"] = v

    # ── 7. Adapter capacity ──────────────────────────────────────────
    print(f"\n[7/8] Adapter capacity test (max 50 adapters)")
    count, kb_per, avg_ns, total_ns = bench_adapter_capacity(lora, rank, in_feat, out_feat, 50)
    print(f"  Registered: {count} adapters  per adapter: {kb_per:.1f} KiB  "
          f"avg reg: {fmt_us(avg_ns)}  total: {fmt_us(total_ns)}")
    results["capacity"] = {
        "max_adapters": count, "per_adapter_kb": kb_per,
        "avg_register_ns": avg_ns, "total_ns": total_ns
    }

    # ── 8. Final stats ───────────────────────────────────────────────
    print(f"\n[8/8] Final engine stats")
    stats = lora.lora_stats()
    print(f"  adapters={stats.total_adapters} active={stats.active_adapters} vram={stats.vram_adapters} "
          f"swaps={stats.total_swaps} evictions={stats.total_evictions} "
          f"avg_swap={stats.avg_swap_time_us:.1f}us")
    results["final_stats"] = {
        "adapters": stats.total_adapters, "active": stats.active_adapters,
        "vram": stats.vram_adapters, "swaps": stats.total_swaps,
        "evictions": stats.total_evictions, "avg_swap_us": stats.avg_swap_time_us
    }

    return results

def main():
    parser = argparse.ArgumentParser(description="Red Daft LoRa Brain Engine — Microbenchmarks")
    parser.add_argument("--rank", type=int, default=16, help="LoRA rank (default 16)")
    parser.add_argument("--in-feat", type=int, default=256, help="input features (default 256)")
    parser.add_argument("--out-feat", type=int, default=256, help="output features (default 256)")
    parser.add_argument("--adapters", type=int, default=4, help="number of adapters (default 4)")
    parser.add_argument("--iters", type=int, default=20, help="iterations per benchmark (default 20)")
    parser.add_argument("--json", type=str, default=None, help="write results to JSON file")
    parser.add_argument("--csv", type=str, default=None, help="write results to CSV file")
    args = parser.parse_args()

    try:
        import red_daft_lora as lora
    except ImportError as e:
        print(f"[error] import red_daft_lora failed: {e}")
        print("Build:  pip install pybind11 && pip install -e .  (in vram-engine/)")
        sys.exit(1)

    lora.initialize()

    results = run_lora_benchmarks(lora, args.rank, args.in_feat, args.out_feat, args.adapters, args.iters)

    lora.print_stats()
    lora.shutdown()

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
        print(f"Results written to {args.csv}")

    print("\nDone.")

if __name__ == "__main__":
    main()
