#!/usr/bin/env python3
import onnxruntime as ort
print(f"ORT version: {ort.__version__}")
try:
    session = ort.InferenceSession("/tmp/vits-piper-en_US-amy-low/en_US-amy-low.onnx")
    print("SUCCESS: Session created")
    inputs = [i.name for i in session.get_inputs()]
    outputs = [o.name for o in session.get_outputs()]
    print(f"Inputs: {inputs}")
    print(f"Outputs: {outputs}")
except Exception as e:
    print(f"FAILED: {e}")
