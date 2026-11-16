#!/bin/bash
# Host test of the online-pack delta decoder. Optional: DELTA BASE EXPECTED for a real netpack.py delta.
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -o "$out/test_net_delta" test_net_delta.cpp -lz
"$out/test_net_delta" "$@"
