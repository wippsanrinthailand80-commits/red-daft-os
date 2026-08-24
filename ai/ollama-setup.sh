#!/usr/bin/env bash
# ollama-setup.sh — bootstrap local AI on Red Daft OS
set -euo pipefail

MODEL_DIR="${DAFT_AI_DIR:-/opt/daft/ai}"

install_ollama() {
  if ! command -v ollama >/dev/null 2>&1; then
    echo "Installing Ollama ..."
    curl -fsSL https://ollama.com/install.sh | sh
  fi
}

pull_default() {
  echo "Pulling default model qwen2.5:3b (8GB-RAM optimized) ..."
  ollama pull qwen2.5:3b
  ollama create reddaft -f "$MODEL_DIR/Modelfile.reddaft"
}

enable_guard() {
  # daft-ai-guard proxies the Ollama API and filters content.
  systemctl enable --now daft-ai-guard.service 2>/dev/null || \
    echo "enable daft-ai-guard manually (no systemd in chroot)"
}

scale_model() {
  local size="${1:-3b}"
  case "$size" in
    3b)  ollama pull qwen2.5:3b;;
    7b)  ollama pull qwen2.5:7b;;
    14b) ollama pull qwen2.5:14b;;
    32b) ollama pull qwen2.5:32b;;
    *) echo "unknown size $size" >&2; exit 1;;
  esac
  echo "switched default to qwen2.5:$size (requires adequate RAM/GPU)"
}

cmd="${1:-bootstrap}"; shift || true
case "$cmd" in
  bootstrap) install_ollama; pull_default; enable_guard;;
  scale) scale_model "${1:-3b}";;
  *) echo "usage: ollama-setup.sh [bootstrap|scale <3b|7b|14b|32b>]";;
esac
