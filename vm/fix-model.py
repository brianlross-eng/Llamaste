#!/usr/bin/env python3
"""Inspect and fix the Piper ONNX model for ORT 1.24.2 compatibility.

The error is: "Graph output (output) does not exist in the graph."
This means the model declares "output" as a graph output, but no node
produces a tensor called "output".
"""
import onnx
import sys

model_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/vits-piper-en_US-amy-low/en_US-amy-low.onnx"

print(f"Loading {model_path}...")
model = onnx.load(model_path)
graph = model.graph

print(f"\nGraph inputs ({len(graph.input)}):")
for inp in graph.input:
    print(f"  - {inp.name}: {inp.type}")

print(f"\nGraph outputs ({len(graph.output)}):")
for out in graph.output:
    print(f"  - {out.name}: {out.type}")

# List all node output names
all_outputs = set()
for node in graph.node:
    for o in node.output:
        all_outputs.add(o)

print(f"\nTotal node outputs: {len(all_outputs)}")

# Check if declared graph outputs exist in node outputs
for out in graph.output:
    if out.name in all_outputs:
        print(f"  ✓ Graph output '{out.name}' exists in nodes")
    else:
        print(f"  ✗ Graph output '{out.name}' NOT in nodes — this is the problem!")
        # Find similar names
        similar = [o for o in all_outputs if "output" in o.lower() or out.name.lower() in o.lower()]
        if similar:
            print(f"    Candidates: {similar[:10]}")

# Try to fix: rename the last node's output to match the declared graph output
if len(graph.output) == 1 and graph.output[0].name not in all_outputs:
    target_name = graph.output[0].name
    # Find the node that should produce this output
    # Usually it's the last node in the graph
    last_node = graph.node[-1]
    print(f"\nLast node: op={last_node.op_type}, outputs={list(last_node.output)}")

    if len(last_node.output) == 1:
        old_name = last_node.output[0]
        print(f"Fixing: renaming node output '{old_name}' -> '{target_name}'")
        last_node.output[0] = target_name

        fixed_path = model_path.replace(".onnx", "-fixed.onnx")
        onnx.save(model, fixed_path)
        print(f"Saved fixed model to: {fixed_path}")

        # Verify
        onnx.checker.check_model(fixed_path)
        print("Model validation passed!")
    else:
        print(f"Cannot auto-fix: last node has {len(last_node.output)} outputs")
else:
    print("\nModel looks OK or has multiple outputs — manual inspection needed")
