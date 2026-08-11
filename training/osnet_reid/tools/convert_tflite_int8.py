"""ONNX → INT8 TFLite 转换脚本（需要安装 onnx2tf 和 tensorflow）。

安装依赖：
    pip install onnx2tf tensorflow

使用：
    python tools/convert_tflite_int8.py \
        --onnx outputs/osnet_upperbody.onnx \
        --output outputs/osnet_upperbody_int8.tflite \
        --calib_data data/prcc/rgb/val \
        --input_h 128 --input_w 64
"""

import argparse
import os
import glob
from typing import List

import numpy as np
from PIL import Image


def parse_args():
    parser = argparse.ArgumentParser(description='ONNX to INT8 TFLite')
    parser.add_argument('--onnx', type=str, required=True)
    parser.add_argument('--output', type=str, default='osnet_upperbody_int8.tflite')
    parser.add_argument('--calib_data', type=str, required=True,
                        help='校准图像目录，用于统计激活分布')
    parser.add_argument('--input_h', type=int, default=128)
    parser.add_argument('--input_w', type=int, default=64)
    parser.add_argument('--num_calib', type=int, default=200,
                        help='用于校准的图像数量')
    return parser.parse_args()


def collect_images(data_dir: str, num: int) -> List[str]:
    patterns = ['*.jpg', '*.jpeg', '*.png']
    imgs = []
    for p in patterns:
        imgs.extend(glob.glob(os.path.join(data_dir, '**', p), recursive=True))
    return imgs[:num]


def preprocess(img_path: str, h: int, w: int):
    img = Image.open(img_path).convert('RGB')
    # 与训练时一致：上半身裁剪 60%
    w0, h0 = img.size
    img = img.crop((0, 0, w0, int(h0 * 0.6)))
    img = img.resize((w, h))
    x = np.array(img).astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406])
    std = np.array([0.229, 0.224, 0.225])
    x = (x - mean) / std
    x = np.transpose(x, (2, 0, 1))
    x = np.expand_dims(x, axis=0)
    return x.astype(np.float32)


def representative_dataset_gen(image_paths: List[str], h: int, w: int):
    def _gen():
        for path in image_paths:
            yield [preprocess(path, h, w)]
    return _gen


def main():
    args = parse_args()

    try:
        import onnx2tf
        import tensorflow as tf
    except ImportError as e:
        raise ImportError(
            "请先安装 onnx2tf 和 tensorflow：pip install onnx2tf tensorflow"
        ) from e

    # 1. ONNX -> SavedModel
    saved_model_dir = args.output + '_saved_model'
    print(f'Converting ONNX to SavedModel: {saved_model_dir}')
    onnx2tf.convert(
        input_onnx_file_path=args.onnx,
        output_folder_path=saved_model_dir,
        output_signaturedefs=True,
    )

    # 2. INT8 TFLite 量化
    converter = tf.lite.TFLiteConverter.from_saved_model(saved_model_dir)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset_gen(
        collect_images(args.calib_data, args.num_calib),
        args.input_h, args.input_w
    )
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.float32

    print('Quantizing to INT8...')
    tflite_model = converter.convert()

    with open(args.output, 'wb') as f:
        f.write(tflite_model)
    print(f'INT8 TFLite saved: {args.output}')


if __name__ == '__main__':
    main()
