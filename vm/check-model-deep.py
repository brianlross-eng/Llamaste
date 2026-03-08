#!/usr/bin/env python3
"""Deep check of ONNX model for subgraph issues."""
import onnx
import sys

model_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/vits-piper-en_US-amy-low/en_US-amy-low.onnx"

model = onnx.load(model_path)
graph = model.graph

# Count control flow ops
control_flow_ops = ["If", "Loop", "Scan"]
cf_nodes = [n for n in graph.node if n.op_type in control_flow_ops]
print(f"Control flow nodes: {len(cf_nodes)}")
for n in cf_nodes:
    print(f"  - {n.op_type}: inputs={list(n.input)}, outputs={list(n.output)}")
    for attr in n.attribute:
        if attr.type == onnx.AttributeProto.GRAPH:
            g = attr.g
            print(f"    Subgraph '{attr.name}': {len(g.node)} nodes")
            print(f"      outputs: {[o.name for o in g.output]}")
            # Check if subgraph outputs exist in subgraph node outputs
            sub_outputs = set()
            for sn in g.node:
                for o in sn.output:
                    sub_outputs.add(o)
            for so in g.output:
                if so.name not in sub_outputs:
                    print(f"      ✗ MISSING: '{so.name}' not in subgraph nodes")

# Check ONNX opset
print(f"\nOpset imports:")
for oi in model.opset_import:
    print(f"  domain='{oi.domain}' version={oi.version}")

# Check metadata
print(f"\nMetadata ({len(model.metadata_props)}):")
for m in model.metadata_props:
    print(f"  {m.key} = {m.value[:100]}")

# Check IR version
print(f"\nIR version: {model.ir_version}")
print(f"Producer: {model.producer_name} v{model.producer_version}")

# Try full validation
try:
    onnx.checker.check_model(model, full_check=True)
    print("\n✓ Full model validation passed")
except Exception as e:
    print(f"\n✗ Model validation failed: {e}")
