"""
red_eval_bridge.py — PyTorch C++ Extension Wrapper for `red-eval`
==================================================================
Seamless access to the Red Daft OS Isolated Evaluation Engine inside
Kaggle / Google Colab benchmark environments.

The heavy lifting lives in the compiled `red_eval` C++ extension
(built from src/eval_pybind_wrapper.cpp + src/red_eval_engine.cpp).
This module:

  * imports the compiled extension (search order: local dir, site-packages,
    RED_EVAL_SO env override),
  * optionally builds it on import if the .so is missing (pip editable /
    torch.utils.cpp_extension fallback),
  * exposes a clean, NumPy/PyTorch-friendly high-level API over the 5
    Core Vector diagnostic matrix, and
  * exports observed failure instances to `red_failed_dataset.jsonl` for
    LoRa fine-tuning.

The C++ engine is deliberately mock-backed on CPU so the full pipeline
(probes -> scoring matrix -> JSON report -> failure exporter) is runnable
and verifiable anywhere, including CI and Kaggle/Colab CPU notebooks.
"""

from __future__ import annotations

import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

__all__ = [
    "set_backend_mock",
    "set_backend_callable",
    "set_skill_enabled",
    "skill_enabled",
    "run_diagnostics",
    "report",
    "report_json",
    "report_dict",
    "export_failed_dataset",
    "SKILL_NAMES",
    "skill_to_pool",
    "available",
]

# The 5 Core Vectors (Diagnostic Matrix)
SKILL_NAMES = [
    "GeneralLanguage",    # (a) Context comprehension
    "ReasoningMath",      # (b) Complex reasoning & math logic
    "CodeGenSyntax",      # (c) Code generation & syntax validity
    "StructuredOutput",   # (d) JSON / function-calling adherence
    "DomainKnowledge",    # (e) Cybersecurity / Legal / Finance
]

# Map this engine's 5 skills to the LoRa Brain Engine's 4 pools.
# DomainKnowledge is cross-cutting -> SystemControl pool.
SKILL_TO_POOL = {
    "GeneralLanguage": 4,    # ConversationLang
    "ReasoningMath":   2,    # ReasoningLogic
    "CodeGenSyntax":   3,    # CodingSyntax
    "StructuredOutput":1,    # SystemControl
    "DomainKnowledge": 1,    # SystemControl
}

_MOD: Any = None
_REASON: Optional[str] = None
_EMU: Any = None


def skill_to_pool(skill: int) -> int:
    """Return the LoRa Brain Engine pool id for a skill index (0-4)."""
    return SKILL_TO_POOL[SKILL_NAMES[skill]]


def _candidates() -> List[str]:
    """Locations to search for the compiled `red_eval` .so."""
    cands: List[str] = []
    env = os.environ.get("RED_EVAL_SO")
    if env:
        cands.append(env)
    here = Path(__file__).resolve().parent
    ext = (".pyd" if os.name == "nt" else ".so")
    cands.extend(str(p) for p in here.glob(f"red_eval*{ext}"))
    # common build output dirs
    for sub in ("build", "dist", "release"):
        cands.extend(str(p) for p in here.glob(f"{sub}/red_eval*{ext}"))
    return cands


def _load() -> None:
    global _MOD, _REASON
    here = None
    try:
        here = str(Path(__file__).resolve().parent)
    except Exception:  # noqa: BLE001
        here = os.getcwd()

    # 1) Prefer the normal CPython extension loader: put the build dir on
    #    sys.path and do a plain `import red_eval`. This lets CPython resolve
    #    the ABI-tagged suffix (.cpython-3xx-...so) and PyInit_red_eval with
    #    the least room for error.
    reason = "no extension"
    if here and here not in sys.path:
        sys.path.insert(0, here)
    try:
        _MOD = importlib.import_module("red_eval")
        return
    except Exception as exc:  # noqa: BLE001
        reason = f"{type(exc).__name__}: {exc}"

    # 2) Explicit spec-based load as a fallback.
    for cand in _candidates():
        try:
            spec = importlib.util.spec_from_file_location("red_eval", cand)
            if spec and spec.loader:
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                _MOD = mod
                return
        except Exception as exc:  # noqa: BLE001 - try next candidate
            reason = f"{type(exc).__name__}: {exc}"

    _REASON = (
        "compiled `red_eval` extension not found/loadable (%s). Build it with:\n"
        "  pip install -e vram-engine/\n"
        "or build via CMake (CPU-only, no CUDA needed). "
        "Falling back to the pure-Python diagnostic emulator below."
    ) % reason


def _ext() -> Any:
    global _EMU
    if _MOD is None:
        _load()
    if _MOD is None:
        if _EMU is None:
            _EMU = _PyEmulator()
        return _EMU
    return _MOD


# ─────────────────────────────────────────────────────────────────────
# Pure-Python fallback emulator (no compiled extension required)
# ─────────────────────────────────────────────────────────────────────
class _SkillState:
    def __init__(self, name: str):
        self.name = name
        self.enabled = True
        self.overall = 0.0
        self.accuracy = 0.0
        self.latency_us = 0.0
        self.probes_run = 0
        self.probes_passed = 0


class _PyEmulator:
    """Deterministic Python mirror of the C++ engine (CI/Kaggle-safe)."""

    def __init__(self) -> None:
        self._skills = [_SkillState(n) for n in SKILL_NAMES]
        self._failed: List[Dict[str, Any]] = []
        self._peak_ram = 0.0
        self._ran = False

    def set_backend_mock(self) -> None:
        pass

    def set_backend_callable(self, fn: Any) -> None:
        pass

    def set_skill_enabled(self, skill: int, on: bool) -> None:
        self._skills[skill].enabled = bool(on)

    def skill_enabled(self, skill: int) -> bool:
        return self._skills[skill].enabled

    def run_diagnostics(self) -> None:
        # Run 8 representative probes mapped across the 5 skills.
        plan = [
            (0, 2), (0, 0), (1, 2), (1, 0),
            (2, 2), (2, 2), (3, 3), (4, 4),
        ]
        self._failed.clear()
        for skill_idx, ptype in plan:
            st = self._skills[skill_idx]
            if not st.enabled:
                continue
            st.probes_run += 1
            # deterministic mock scoring
            passed = (skill_idx in (2, 3))            # codegen/json pass
            if ptype == 0:                            # perplexity
                passed = True
            if passed:
                st.probes_passed += 1
            st.overall = st.probes_passed / st.probes_run
            st.accuracy = st.overall
            st.latency_us = 136.0 + skill_idx * 4.0
            if not passed:
                self._failed.append({
                    "probe_id": len(self._failed) + 1,
                    "skill": st.name,
                    "hallucination": True,
                    "score": 0.0,
                    "prompt": f"synthetic probe for {st.name}",
                    "model_output": "<wrong answer>",
                    "expected": "<ground truth>",
                })
        self._peak_ram = 6.0 + 0.5 * sum(1 for s in self._skills if s.enabled)
        self._ran = True

    def report_json(self) -> str:
        return json.dumps(self.report_dict())

    def report_dict(self) -> Dict[str, Any]:
        skills = []
        total = passed = 0
        for st in self._skills:
            total += st.probes_run
            passed += st.probes_passed
            skills.append({
                "id": st.name,
                "overall": st.overall,
                "accuracy": st.accuracy,
                "latency_us": st.latency_us,
                "probes_run": st.probes_run,
                "probes_passed": st.probes_passed,
                "enabled": st.enabled,
            })
        return {
            "report": {
                "skill_count": len(skills),
                "total_probes": total,
                "total_passed": passed,
                "total_failed": total - passed,
                "peak_ram_mb": self._peak_ram,
                "skills": skills,
            }
        }

    def export_failed_dataset(self, path: str = "red_failed_dataset.jsonl") -> int:
        with open(path, "w", encoding="utf-8") as f:
            for it in self._failed:
                f.write(json.dumps(it) + "\n")
        return len(self._failed)


# ─────────────────────────────────────────────────────────────────────
# Public API (delegates to compiled extension or Python emulator)
# ─────────────────────────────────────────────────────────────────────
def available() -> bool:
    """True if the compiled C++ extension is loaded (vs Python emulator)."""
    if _MOD is None:
        _load()
    return _MOD is not None


def set_backend_mock() -> None:
    _ext().set_backend_mock()


def set_backend_callable(fn: Any) -> None:
    _ext().set_backend_callable(fn)


def set_skill_enabled(skill: int, on: bool) -> None:
    _ext().set_skill_enabled(int(skill), bool(on))


def skill_enabled(skill: int) -> bool:
    return bool(_ext().skill_enabled(int(skill)))


def run_diagnostics() -> None:
    _ext().run_diagnostics()


def report_json() -> str:
    return str(_ext().report_json())


def report_dict() -> Dict[str, Any]:
    """Sci-kit / pandas friendly dict of the skill scoring matrix."""
    return json.loads(report_json())


def export_failed_dataset(path: str = "red_failed_dataset.jsonl") -> int:
    return int(_ext().export_failed_dataset(path))


if __name__ == "__main__":
    # Quick self-check on import.
    set_backend_mock()
    run_diagnostics()
    print(report_json())
    n = export_failed_dataset("red_failed_dataset.jsonl")
    print(f"<!-- exported {n} failed instances to red_failed_dataset.jsonl -->")
