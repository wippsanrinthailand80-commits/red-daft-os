#!/usr/bin/env python3
"""
stress_3b.py — Kaggle/Linux stress benchmark for Red Daft VRAM Engine
Simulates a 3B LLM training/inference footprint with 10 pools.

Usage:
  python stress_3b.py --layers 28 --hidden 3072 --seq-len 2048 --iters 3
  RD_USE_CUDA=1 python stress_3b.py  # with GPU
  python stress_3b.py --torch  # also test torch integration if torch installed
"""
import argparse
import sys

def main():
    parser = argparse.ArgumentParser(description="Red Daft VRAM — 3B stress")
    parser.add_argument("--layers", type=int, default=28)
    parser.add_argument("--hidden", type=int, default=3072)
    parser.add_argument("--seq-len", type=int, default=2048)
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--torch", action="store_true", help="also run torch integration check")
    parser.add_argument("--budget-mib", type=int, default=2048, help="VRAM budget MiB (0=auto)")
    args = parser.parse_args()

    try:
        import red_daft_vram as rdv
    except ImportError as e:
        print(f"[error] import red_daft_vram failed: {e}")
        print("Build first:  pip install pybind11 && pip install -e .  (in vram-engine/)")
        sys.exit(1)

    print("="*70)
    print(" Red Daft VRAM Engine — 3B Stress Benchmark (Kaggle/Linux)")
    print("="*70)

    # Initialize
    if args.budget_mib:
        cfg = rdv.EngineConfig()
        cfg.vram_budget_bytes = args.budget_mib << 20
        cfg.host_budget_bytes = 8 << 30
        cfg.num_streams = 4
        cfg.enable_double_buffer = True
        rdv.initialize(cfg)
    else:
        rdv.initialize()

    # Quick 10-pool smoke
    print("\n[1/3] 10-pool smoke (1 MiB per pool):")
    print(rdv.stress_all_pools())

    # Full 3B stress
    print(f"\n[2/3] 3B LLM stress: {args.layers} layers, hidden={args.hidden}, seq={args.seq_len}, iters={args.iters}")
    res = rdv.stress_3b_benchmark(
        layers=args.layers,
        hidden=args.hidden,
        seq_len=args.seq_len,
        iterations=args.iters,
        verbose=True,
    )
    print("\n" + res.report)
    rdv.print_pool_stats()

    if res.passed:
        print("✅ PASS — VRAM engine survived 3B stress (peak %.1f MiB, borrows %d)" % (res.peak_vram_mib, res.emergency_borrows))
    else:
        print("❌ FAIL")
        sys.exit(1)

    # Optional torch integration
    if args.torch:
        print("\n[3/3] Torch integration:")
        try:
            import torch
            print(f"  torch {torch.__version__} found")
            try:
                t = rdv.allocate_torch(pool=0, shape=[1024, 1024], dtype="bf16")
                print(f"  allocate_torch ok: {t.shape} {t.dtype} {t.device}")
                del t
                print("  torch integration OK")
            except Exception as e:
                print(f"  allocate_torch failed (expected on CPU fallback): {e}")
                # Still try torch-only alloc
                t = torch.randn(1024, 1024, dtype=torch.bfloat16)
                print(f"  torch.randn fallback ok: {t.shape}")
        except ImportError:
            print("  torch not installed — skipping")

    print("\nDone.")

if __name__ == "__main__":
    main()
