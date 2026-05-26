#!/usr/bin/env bash
# Manually regenerate api_subset.pb.{c,h}. CMake does this automatically
# during `idf.py build`, but this script is useful for iterating on the
# .proto/.options files outside of an IDF build.

set -euo pipefail
cd "$(dirname "$0")"

if [ ! -f nanopb/generator/nanopb_generator.py ]; then
    echo "[+] cloning nanopb runtime + generator into ./nanopb"
    git clone --depth 1 https://github.com/nanopb/nanopb.git nanopb
fi

python3 nanopb/generator/nanopb_generator.py api_subset.proto
echo "[+] generated api_subset.pb.{c,h}"
