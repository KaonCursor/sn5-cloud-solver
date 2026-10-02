#!/usr/bin/env bash
# Ensure a Rust compiler is available; Python 3 is assumed present.
set -euo pipefail
if ! command -v rustc >/dev/null 2>&1; then
  if [ -x "$HOME/.cargo/bin/rustc" ]; then
    echo "rustc at $HOME/.cargo/bin (add to PATH)"
  else
    curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal >/dev/null
  fi
fi
export PATH="$HOME/.cargo/bin:$PATH"
rustc --version
python3 --version
