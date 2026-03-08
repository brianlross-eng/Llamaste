#!/bin/bash
# call-tool.sh <tool_name> [args_json]
TOOL="$1"
ARGS="${2:-{}}"
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"$TOOL\",\"arguments\":$ARGS}" \
  http://172.18.208.1:8080/llamaste/tool
echo
