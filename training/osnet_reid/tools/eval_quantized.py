"""评估量化后的 OSNet 在 PRCC val 上的 Rank-1/mAP。"""

import os
import sys
import glob
import re
import argparse
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from torch.utils.data import Dataset, DataLoader

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.fix_and_export import build_loader, fix_conv_scales


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument('--onnx', type=str, default='outputs/osnet_upperbody.onnx')
    parser.add_argument('--data', type=str, default='../../data/prcc/rgb/val')
    parser.add_argument('--input_h', type=int, default=128)
    parser.add_argument('--input_w', type=int, default=64)
    parser.add_argument('--num_calib', type=int, default=200)
    parser.add_argument('--target', type=str, default='esp32p4')
    parser.add_argument('--bits', type=int, default=8)
    parser.add_argument('--max_samples', type=int, default=0, help='0 表示用全部')
    return parser.parse_args()


def collect_samples(data_dir):
    samples = []
    for path in glob.glob(os.path.join(data_dir, '**', '*.jpg'), recursive=True):
        parts = Path(path).parts
        # expect .../val/<cam>/<pid>/<img>
        cam = parts[-3]
        pid_str = parts[-2]
        pid = int(pid_str)
        clothes = 0 if cam in ('A', 'B') else 1
        samples.append((path, pid, cam, clothes))
    return samples


def preprocess(img_path, h, w):
    img = Image.open(img_path).convert('RGB')
    w0, h0 = img.size
    img = img.crop((0, 0, w0, int(h0 * 0.6)))
    img = img.resize((w, h))
    x = np.array(img).astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406])
    std = np.array([0.229, 0.224, 0.225])
    x = (x - mean) / std
    x = np.transpose(x, (2, 0, 1))
    return x.astype(np.float32)


class ValDataset(Dataset):
    def __init__(self, data_dir, h, w):
        self.samples = collect_samples(data_dir)
        self.h = h
        self.w = w

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):
        path, pid, cam, clothes = self.samples[idx]
        x = preprocess(path, self.h, self.w)
        cam_map = {'A': 0, 'B': 1, 'C': 2}
        return torch.from_numpy(x), pid, cam_map.get(cam, 0), clothes


def compute_distance_matrix(a, b):
    a = torch.from_numpy(a)
    b = torch.from_numpy(b)
    return torch.cdist(a, b, p=2).numpy()


def eval_market1501(distmat, q_pids, g_pids, q_cams, g_cams):
    num_q = distmat.shape[0]
    indices = np.argsort(distmat, axis=1)
    matches = (g_pids[indices] == q_pids[:, None]).astype(np.int32)
    all_cmc = []
    all_AP = []
    for q_idx in range(num_q):
        q_pid = q_pids[q_idx]
        q_cam = q_cams[q_idx]
        order = indices[q_idx]
        keep = (g_pids[order] != q_pid) | (g_cams[order] != q_cam)
        cmc = matches[q_idx][order][keep]
        if not np.any(cmc):
            continue
        cmc = cmc.cumsum()
        cmc[cmc > 1] = 1
        all_cmc.append(cmc[:10])

        num_rel = cmc.sum()
        tmp_cmc = cmc.cumsum()
        tmp_cmc = [x / (i + 1.0) for i, x in enumerate(tmp_cmc)]
        tmp_cmc = np.asarray(tmp_cmc) * cmc
        AP = tmp_cmc.sum() / num_rel
        all_AP.append(AP)
    mAP = np.mean(all_AP) if all_AP else 0.0
    cmc = np.mean(all_cmc, axis=0) if all_cmc else np.zeros(10)
    rank1 = cmc[0] if len(cmc) else 0.0
    return rank1, mAP


def main():
    args = parse_args()
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.executor.torch import TorchExecutor
    from esp_ppq.api.espdl_interface import get_target_platform
    import esp_ppq.lib as PFL

    # 1. quantize
    calib_loader = build_loader(args.data, args.input_h, args.input_w, args.num_calib)
    setting = QuantizationSettingFactory.espdl_setting()
    print('Quantizing ...')
    graph = espdl_quantize_onnx(
        onnx_import_file=args.onnx,
        espdl_export_file='outputs/dummy.espdl',
        calib_dataloader=calib_loader,
        calib_steps=len(calib_loader),
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
    print('Fixing scales ...')
    fixed = fix_conv_scales(graph)
    print(f'Fixed {len(fixed)} ops')

    # 2. create executor
    executor = TorchExecutor(graph, device='cpu')

    # 3. run val
    val_set = ValDataset(args.data, args.input_h, args.input_w)
    if args.max_samples > 0 and args.max_samples < len(val_set):
        # 取前 N 个样本快速验证
        val_set.samples = val_set.samples[:args.max_samples]
    val_loader = DataLoader(val_set, batch_size=1, shuffle=False, num_workers=0)

    feats = []
    pids = []
    cams = []
    clothes = []
    print('Evaluating ...')
    for i, (x, pid, cam, cloth) in enumerate(val_loader):
        # input is [1,3,H,W] already
        out = executor.forward(inputs=[x], output_names=['feature'])
        feat_np = out[0].detach().cpu().numpy().reshape(-1)
        # L2 normalize
        feat_np = feat_np / (np.linalg.norm(feat_np) + 1e-8)
        feats.append(feat_np)
        pids.append(int(pid))
        cams.append(int(cam))
        clothes.append(int(cloth))
        if (i + 1) % 500 == 0:
            print(f'  {i+1}/{len(val_loader)}')

    feats = np.stack(feats)
    pids = np.array(pids)
    clothes = np.array(clothes)

    # split query/gallery: first image of each pid as gallery, rest query
    gallery_idx = []
    query_idx = []
    seen = set()
    for i, pid in enumerate(pids):
        if pid not in seen:
            gallery_idx.append(i)
            seen.add(pid)
        else:
            query_idx.append(i)
    if len(gallery_idx) == 0:
        print('No gallery, skip eval')
        return

    q_feat = feats[query_idx]
    g_feat = feats[gallery_idx]
    q_pid = pids[query_idx]
    g_pid = pids[gallery_idx]
    q_cam = np.array([cams[i] for i in query_idx])
    g_cam = np.array([cams[i] for i in gallery_idx])
    q_clothes = clothes[query_idx]
    g_clothes = clothes[gallery_idx]

    distmat = compute_distance_matrix(q_feat, g_feat)
    rank1, mAP = eval_market1501(distmat, q_pid, g_pid, q_cam, g_cam)
    print(f'Quantized Val -> Rank-1: {rank1:.2%}, mAP: {mAP:.2%}')

    same_q = q_clothes == 0
    cross_q = q_clothes == 1
    if same_q.sum():
        pred = g_pid[distmat[same_q].argmin(axis=1)]
        sc_r1 = (pred == q_pid[same_q]).mean()
        print(f'  Same-clothes Rank-1: {sc_r1:.2%}')
    if cross_q.sum():
        pred = g_pid[distmat[cross_q].argmin(axis=1)]
        cc_r1 = (pred == q_pid[cross_q]).mean()
        print(f'  Cross-clothes Rank-1: {cc_r1:.2%}')


if __name__ == '__main__':
    main()
