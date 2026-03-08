#!/bin/bash
curl -s -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"fs.list_directory","arguments":{"path":"/usr/share/espeak-ng-data"}}' \
  http://172.18.208.1:8080/llamaste/tool
echo
