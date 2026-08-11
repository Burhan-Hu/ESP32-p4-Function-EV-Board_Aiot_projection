"""修正 Conv 量化 scale 的 exponent 约束并导出 .espdl。

ESP-DL 要求 Conv 的输出 scale >= 输入 scale * 权重 scale（即 output_exponent >= input_exponent + weight_exponent）。
PPQ 校准后某些 gate Conv 的输出 scale 过细，这里自动把 output scale 放大到满足约束的最小值。
"""

import argparse
import os
import sys

import torch
import numpy as np
from torch.utils.data import DataLoader

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.quantize_espdl import CalibDataset


def parse_args():
    parser = argparse.ArgumentParser(description='Fix Conv scales and export espdl')
    parser.add_argument('--onnx', type=str, required=True)
    parser.add_argument('--output', type=str, required=True)
    parser.add_argument('--calib_data', type=str, required=True)
    parser.add_argument('--input_h', type=int, default=128)
    parser.add_argument('--input_w', type=int, default=64)
    parser.add_argument('--num_calib', type=int, default=200)
    parser.add_argument('--target', type=str, default='esp32p4')
    parser.add_argument('--bits', type=int, default=8)
    return parser.parse_args()


def build_loader(data_dir, h, w, num):
    dataset = CalibDataset(data_dir, h, w, num)
    return DataLoader(dataset, batch_size=1, shuffle=False, num_workers=0)


def get_master_config(cfg):
    """找到被 overlapped 配置实际指向的 master config。"""
    seen = set()
    while cfg.dominated_by is not cfg:
        cfg = cfg.dominated_by
        if id(cfg) in seen:
            break
        seen.add(id(cfg))
    return cfg


def fix_conv_scales(graph):
    """遍历 Conv/Gemm，保证 output_scale >= input_scale * weight_scale。"""
    fixed = []
    for op in graph.operations.values():
        if op.type not in ('Conv', 'Gemm', 'MatMul'):
            continue
        if len(op.input_quant_config) < 2:
            continue
        in_cfg = op.input_quant_config[0]
        w_cfg = op.input_quant_config[1]
        out_cfg = get_master_config(op.output_quant_config[0])

        in_scale = float(in_cfg.scale) if in_cfg.scale.numel() == 1 else float(torch.min(in_cfg.scale))
        w_scale = float(w_cfg.scale) if w_cfg.scale.numel() == 1 else float(torch.min(w_cfg.scale))
        min_out_scale = in_scale * w_scale

        cur_scale = float(out_cfg.scale) if out_cfg.scale.numel() == 1 else float(torch.min(out_cfg.scale))
        if cur_scale < min_out_scale:
            new_scale = min_out_scale
            device = out_cfg.scale.device
            out_cfg.scale = torch.tensor(new_scale, dtype=torch.float32, device=device)
            out_cfg.offset = torch.tensor(0, dtype=torch.float32, device=device)
            fixed.append((op.name, op.type, cur_scale, new_scale))
    return fixed


def main():
    args = parse_args()
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.api.espdl_interface import get_target_platform
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.core import TargetPlatform
    from esp_ppq.executor.torch import TorchExecutor

    loader = build_loader(args.calib_data, args.input_h, args.input_w, args.num_calib)
    setting = QuantizationSettingFactory.espdl_setting()

    print('Step 1: quantize without export ...')
    graph = espdl_quantize_onnx(
        onnx_import_file=args.onnx,
        espdl_export_file=args.output,
        calib_dataloader=loader,
        calib_steps=len(loader),
        input_shape=[1, 3, args.input_h, args.input_w],
        target=args.target,
        num_of_bits=args.bits,
        setting=setting,
        device='cpu',
        error_report=False,
        skip_export=True,
        export_test_values=False,
        verbose=0,
    )

    print('Step 2: fix conv scales ...')
    fixed = fix_conv_scales(graph)
    print(f'Fixed {len(fixed)} ops')
    for name, op_type, old, new in fixed:
        print(f'  {name} ({op_type}): {old:.3e} -> {new:.3e}')

    print('Step 3: export espdl ...')
    import esp_ppq.lib as PFL
    target_platform = get_target_platform(args.target, args.bits)
    PFL.Exporter(platform=target_platform).export(
        file_path=args.output,
        graph=graph,
        values_for_test=None,
        export_config=True,
        auto_streaming=False,
        streaming_table=None,
        streaming_input_shape=None,
        streaming_custom_passes=None,
    )
    print(f'Saved: {args.output}')


if __name__ == '__main__':
    main()
