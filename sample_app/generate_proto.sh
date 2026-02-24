#!/usr/bin/env bash
# Regenerate zmk_ipc_pb2.py from the proto schema.
# Run from the sample_app directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROTO_DIR="$SCRIPT_DIR/../app/proto"

protoc \
  --proto_path="$PROTO_DIR" \
  --python_out="$SCRIPT_DIR" \
  "$PROTO_DIR/zmk_ipc.proto"

echo "Generated: $SCRIPT_DIR/zmk_ipc_pb2.py"
