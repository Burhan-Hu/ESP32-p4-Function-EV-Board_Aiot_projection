"""ONNX → ESP-DL .espdl INT8 量化脚本。

依赖：
    pip install esp-ppq

使用：
    python tools/quantize_espdl.py \
        --onnx outputs/osnet_upperbody.onnx \
        --output outputs/osnet_upperbody.espdl \
        --calib_data ../../data/prcc/rgb/val \
        --input_h 128 --input_w 64 \
        --num_calib 200
"""

import argparse
import os
import sys
import glob
from typing import List

import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader
from PIL import Image
import yaml


sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def parse_args():
    parser = argparse.ArgumentParser(description='Quantize OSNet ONNX to ESP-DL .espdl')
    parser.add_argument('--onnx', type=str, required=True, help='输入 ONNX 路径')
    parser.add_argument('--output', type=str, default='outputs/osnet_upperbody.espdl')
    parser.add_argument('--calib_data', type=str, required=True,
                        help='校准图像目录，支持 PRCC rgb/<split>/<cam>/<pid> 结构')
    parser.add_argument('--input_h', type=int, default=128)
    parser.add_argument('--input_w', type=int, default=64)
    parser.add_argument('--num_calib', type=int, default=200)
    parser.add_argument('--target', type=str, default='esp32p4',
                        help='目标平台：esp32 / esp32s3 / esp32p4')
    parser.add_argument('--bits', type=int, default=8, choices=[8, 16])
    return parser.parse_args()


def collect_images(data_dir: str, num: int) -> List[str]:
    patterns = ['*.jpg', '*.jpeg', '*.png']
    imgs = []
    for p in patterns:
        imgs.extend(glob.glob(os.path.join(data_dir, '**', p), recursive=True))
    return imgs[:num]


def preprocess(img_path: str, h: int, w: int) -> np.ndarray:
    img = Image.open(img_path).convert('RGB')
    # 上半身裁剪 60%
    w0, h0 = img.size
    img = img.crop((0, 0, w0, int(h0 * 0.6)))
    img = img.resize((w, h))
    x = np.array(img).astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406])
    std = np.array([0.229, 0.224, 0.225])
    x = (x - mean) / std
    x = np.transpose(x, (2, 0, 1))
    return x.astype(np.float32)


class CalibDataset(Dataset):
    """兼容 esp-ppq 的校准数据集，返回 NCHW torch.Tensor。"""

    def __init__(self, data_dir: str, h: int, w: int, num: int):
        self.image_paths = collect_images(data_dir, num)
        self.h = h
        self.w = w

    def __len__(self):
        return len(self.image_paths)

    def __getitem__(self, idx: int):
        x = preprocess(self.image_paths[idx], self.h, self.w)  # [3, H, W]
        return torch.from_numpy(x)


def main():
    args = parse_args()

    try:
        from esp_ppq.api import espdl_quantize_onnx
        from esp_ppq import QuantizationSettingFactory
    except ImportError as e:
        raise ImportError(
            "请先安装 esp-ppq：pip install esp-ppq"
        ) from e

    if not os.path.exists(args.onnx):
        raise FileNotFoundError(f"ONNX 文件不存在：{args.onnx}")

    dataset = CalibDataset(args.calib_data, args.input_h, args.input_w, args.num_calib)
    loader = DataLoader(dataset, batch_size=1, shuffle=False, num_workers=0)
    print(f'Calibration images: {len(dataset)}')

    setting = QuantizationSettingFactory.espdl_setting()

    print(f'Quantizing for {args.target} with {args.bits}-bit ...')
    quant_ppq_graph = espdl_quantize_onnx(
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
        skip_export=False,
        export_test_values=True,
        verbose=0,
    )
    print(f'ESP-DL model saved: {args.output}')


if __name__ == '__main__':
    main()
