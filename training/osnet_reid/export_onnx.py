"""将训练好的 OSNet 模型导出为 ONNX，用于后续 INT8 量化与 ESP32-P4 部署。"""

import argparse
import os

import torch
import torch.nn as nn
import onnx
import onnxruntime as ort
import numpy as np

from models.osnet_upperbody import build_osnet_upperbody


def parse_args():
    parser = argparse.ArgumentParser(description='Export OSNet to ONNX')
    parser.add_argument('--checkpoint', type=str, required=True, help='best_model.pth 路径')
    parser.add_argument('--output', type=str, default='osnet_upperbody.onnx', help='输出 ONNX 路径')
    parser.add_argument('--input_h', type=int, default=128)
    parser.add_argument('--input_w', type=int, default=64)
    parser.add_argument('--verify', action='store_true', help='验证 ONNX 与 PyTorch 输出一致性')
    return parser.parse_args()


class FeatureExtractor(nn.Module):
    """包装模型，在 eval 时只返回 128 维特征。"""

    def __init__(self, model: nn.Module):
        super().__init__()
        self.model = model

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        self.model.eval()
        with torch.no_grad():
            feat = self.model(x)
        return feat


def main():
    args = parse_args()

    ckpt = torch.load(args.checkpoint, map_location='cpu', weights_only=False)
    cfg = ckpt.get('cfg', {})
    num_classes = cfg.get('num_classes', 150)
    feature_dim = cfg.get('feature_dim', 128)
    use_ibn = cfg.get('use_ibn', True)

    model = build_osnet_upperbody(
        num_classes=num_classes,
        feature_dim=feature_dim,
        pretrained=False,
        use_ibn=use_ibn,
    )
    model.load_state_dict(ckpt['model_state_dict'])
    model.eval()

    extractor = FeatureExtractor(model)
    extractor.eval()

    dummy_input = torch.randn(1, 3, args.input_h, args.input_w)

    torch.onnx.export(
        extractor,
        dummy_input,
        args.output,
        export_params=True,
        opset_version=11,
        do_constant_folding=True,
        input_names=['input'],
        output_names=['feature'],
        dynamo=False,
    )
    print(f'ONNX exported to: {args.output}')

    # 简化模型（可选）
    try:
        import onnxsim
        model_onnx = onnx.load(args.output)
        model_onnx, check = onnxsim.simplify(model_onnx)
        onnx.save(model_onnx, args.output)
        print('ONNX simplified.')
    except ImportError:
        print('onnxsim not installed, skip simplification.')

    # 验证模型结构
    model_onnx = onnx.load(args.output)
    onnx.checker.check_model(model_onnx)
    print('ONNX model check passed.')

    if args.verify:
        # PyTorch 输出
        with torch.no_grad():
            pt_out = extractor(dummy_input).numpy()

        # ONNX Runtime 输出
        session = ort.InferenceSession(args.output, providers=['CPUExecutionProvider'])
        onnx_out = session.run(None, {'input': dummy_input.numpy()})[0]

        diff = np.abs(pt_out - onnx_out).max()
        cos_sim = np.sum(pt_out * onnx_out) / (np.linalg.norm(pt_out) * np.linalg.norm(onnx_out) + 1e-8)
        print(f'PyTorch vs ONNX max diff: {diff:.6e}, cosine similarity: {cos_sim:.6f}')
        if diff > 1e-5:
            print('Warning: diff is larger than expected.')
        else:
            print('Verification passed.')


if __name__ == '__main__':
    main()
