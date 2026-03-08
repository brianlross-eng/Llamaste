#!/bin/bash
echo "=== sherpa-diag.log ==="
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"fs.read_file","arguments":{"path":"/data/llamaste/sherpa-diag.log"}}' \
  http://172.18.208.1:8080/llamaste/tool
echo
echo "=== sherpa-child.log ==="
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"fs.read_file","arguments":{"path":"/data/llamaste/sherpa-child.log"}}' \
  http://172.18.208.1:8080/llamaste/tool
echo
