"""OSNet-IBN-x0.25 上半身微调训练脚本。"""

import os
import argparse
import yaml
from pathlib import Path
from typing import Dict, Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.optim import Adam
from torch.optim.lr_scheduler import StepLR
from torch.utils.data import DataLoader
from torchvision import transforms
from tqdm import tqdm
import os as _os
import sys as _sys

_TQDM_DISABLE = not _sys.stdout.isatty()

from datasets.prcc import PRCCUpperBodyDataset
from datasets.transforms import build_transforms
from losses.triplet import TripletHardLoss
from losses.center_loss import CenterLoss
from losses.cross_clothes import CrossClothesConsistencyLoss
from models.osnet_upperbody import build_osnet_upperbody, freeze_layers, get_model_summary
from utils.sampler import CrossClothesPKSampler
from utils.metrics import compute_distance_matrix, eval_market1501


def parse_args():
    parser = argparse.ArgumentParser(description='Train OSNet upper-body Re-ID')
    parser.add_argument('--config', type=str, default='config.yaml', help='配置文件路径')
    parser.add_argument('--device', type=str, default='cuda' if torch.cuda.is_available() else 'cpu')
    parser.add_argument('--resume', type=str, default='', help='恢复训练的 checkpoint 路径')
    return parser.parse_args()


def load_config(path: str) -> Dict:
    with open(path, 'r', encoding='utf-8') as f:
        cfg = yaml.safe_load(f)
    return cfg


def build_dataloaders(cfg: Dict, device: str):
    train_transform = build_transforms(
        cfg['image_size'], cfg['mean'], cfg['std'], is_train=True
    )
    test_transform = build_transforms(
        cfg['image_size'], cfg['mean'], cfg['std'], is_train=False
    )

    train_set = PRCCUpperBodyDataset(
        cfg['data_root'], split='train', transform=train_transform,
        upper_body_ratio=cfg['upper_body_ratio']
    )
    # 验证集：优先用 val，否则用 test
    val_split = 'val' if os.path.isdir(os.path.join(cfg['data_root'], 'rgb', 'val')) else 'test'
    val_set = PRCCUpperBodyDataset(
        cfg['data_root'], split=val_split, transform=test_transform,
        upper_body_ratio=cfg['upper_body_ratio']
    )

    sampler = CrossClothesPKSampler(train_set, num_instances=cfg['num_instances'])
    pin = device != 'cpu'
    train_loader = DataLoader(
        train_set, batch_size=cfg['batch_size'], sampler=sampler,
        num_workers=2, pin_memory=pin, drop_last=True
    )
    val_loader = DataLoader(
        val_set, batch_size=cfg['batch_size'], shuffle=False,
        num_workers=2, pin_memory=pin
    )
    return train_loader, val_loader, train_set.num_classes


def split_query_gallery(dataset: PRCCUpperBodyDataset):
    """将测试集拆为 query/gallery：每个身份选 cam=0 的一张作为 gallery，其余 query。"""
    gallery_indices = []
    query_indices = []
    seen_pids = set()
    for idx, sample in enumerate(dataset.samples):
        pid = sample[1]
        cam = sample[2]
        if pid not in seen_pids and cam == 0:
            gallery_indices.append(idx)
            seen_pids.add(pid)
        else:
            query_indices.append(idx)

    # 兜底：若某身份没有 cam=0，随机选一张作为 gallery
    all_pids = {s[1] for s in dataset.samples}
    missing_pids = all_pids - seen_pids
    for pid in missing_pids:
        candidates = [i for i, s in enumerate(dataset.samples) if s[1] == pid]
        gallery_indices.append(candidates[0])
        for c in candidates[1:]:
            query_indices.append(c)

    return query_indices, gallery_indices


@torch.no_grad()
def extract_features(model: nn.Module, loader: DataLoader, device: str):
    model.eval()
    features_all = []
    pids_all = []
    cams_all = []
    clothes_all = []
    for batch in tqdm(loader, desc='Extracting', disable=_TQDM_DISABLE):
        imgs = batch['img'].to(device)
        feats = model(imgs)  # eval 模式返回特征
        feats = F.normalize(feats, p=2, dim=1)
        features_all.append(feats.cpu())
        pids_all.append(batch['pid'])
        cams_all.append(batch['cam'])
        clothes_all.append(batch['clothes'])

    features = torch.cat(features_all, dim=0).numpy()
    pids = torch.cat(pids_all, dim=0).numpy()
    cams = torch.cat(cams_all, dim=0).numpy()
    clothes = torch.cat(clothes_all, dim=0).numpy()
    return features, pids, cams, clothes


@torch.no_grad()
def evaluate(model: nn.Module, val_loader: DataLoader, dataset: PRCCUpperBodyDataset, device: str):
    query_indices, gallery_indices = split_query_gallery(dataset)
    features, pids, cams, clothes = extract_features(model, val_loader, device)

    q_feat = features[query_indices]
    g_feat = features[gallery_indices]
    q_pid = pids[query_indices]
    g_pid = pids[gallery_indices]
    q_cam = cams[query_indices]
    g_cam = cams[gallery_indices]
    q_clothes = clothes[query_indices]
    g_clothes = clothes[gallery_indices]

    distmat = compute_distance_matrix(q_feat, g_feat)
    rank1, mAP = eval_market1501(distmat, q_pid, g_pid, q_cam, g_cam)

    # 同衣服 / 跨衣服 子集评估
    same_q = q_clothes == 0
    cross_q = q_clothes == 1
    sc_rank1 = None
    cc_rank1 = None
    if same_q.sum() > 0:
        pred_pids = g_pid[distmat[same_q].argmin(axis=1)]
        sc_rank1 = (pred_pids == q_pid[same_q]).mean()
    if cross_q.sum() > 0:
        pred_pids = g_pid[distmat[cross_q].argmin(axis=1)]
        cc_rank1 = (pred_pids == q_pid[cross_q]).mean()

    return {
        'rank1': rank1,
        'mAP': mAP,
        'same_clothes_rank1': sc_rank1,
        'cross_clothes_rank1': cc_rank1,
    }


def train_one_epoch(
    model: nn.Module,
    loader: DataLoader,
    criterion_id: nn.Module,
    criterion_triplet: nn.Module,
    criterion_center: nn.Module,
    criterion_consistency: nn.Module,
    optimizer: torch.optim.Optimizer,
    cfg: Dict,
    device: str,
    epoch: int,
):
    model.train()
    total_loss = 0.0
    total_id = 0.0
    total_tri = 0.0
    total_cen = 0.0
    total_con = 0.0

    pbar = tqdm(loader, desc=f'Epoch {epoch}', disable=_TQDM_DISABLE)
    for batch in pbar:
        imgs = batch['img'].to(device)
        pids = batch['pid'].to(device)
        clothes = batch['clothes'].to(device)

        logits, features = model(imgs)

        loss_id = criterion_id(logits, pids)
        loss_tri = criterion_triplet(features, pids)
        loss_cen = criterion_center(features, pids)
        loss_con = criterion_consistency(features, pids, clothes)

        loss = (
            cfg['loss_id_weight'] * loss_id
            + cfg['loss_triplet_weight'] * loss_tri
            + cfg['loss_center_weight'] * loss_cen
            + cfg['loss_consistency_weight'] * loss_con
        )

        optimizer.zero_grad()
        loss.backward()
        optimizer.step()

        total_loss += loss.item()
        total_id += loss_id.item()
        total_tri += loss_tri.item()
        total_cen += loss_cen.item()
        total_con += loss_con.item()

        pbar.set_postfix({
            'loss': f'{loss.item():.3f}',
            'id': f'{loss_id.item():.3f}',
            'tri': f'{loss_tri.item():.3f}',
            'con': f'{loss_con.item():.3f}',
        })

    n = len(loader)
    return {
        'loss': total_loss / n,
        'id': total_id / n,
        'triplet': total_tri / n,
        'center': total_cen / n,
        'consistency': total_con / n,
    }


def get_lr(optimizer: torch.optim.Optimizer) -> float:
    return optimizer.param_groups[0]['lr']


def main():
    args = parse_args()
    cfg = load_config(args.config)
    device = args.device
    print(f'Using device: {device}')

    os.makedirs(cfg['output_dir'], exist_ok=True)

    train_loader, val_loader, num_classes = build_dataloaders(cfg, device)
    print(f'Train samples: {len(train_loader.dataset)}, classes: {num_classes}')
    print(f'Val samples: {len(val_loader.dataset)}')

    model = build_osnet_upperbody(
        num_classes=num_classes,
        feature_dim=cfg['feature_dim'],
        pretrained=cfg['pretrained'],
        use_ibn=cfg['use_ibn'],
    )
    model = model.to(device)
    get_model_summary(model, input_size=(1, 3, cfg['image_size'][0], cfg['image_size'][1]))

    criterion_id = nn.CrossEntropyLoss(label_smoothing=0.1).to(device)
    criterion_triplet = TripletHardLoss(margin=0.3).to(device)
    criterion_center = CenterLoss(num_classes=num_classes, feat_dim=cfg['feature_dim']).to(device)
    criterion_consistency = CrossClothesConsistencyLoss(margin=cfg['consistency_margin']).to(device)

    # 分开优化：center loss 需要单独较大 lr
    optimizer = Adam([
        {'params': model.parameters(), 'lr': cfg['base_lr']},
        {'params': criterion_center.parameters(), 'lr': cfg['base_lr'] * 10},
    ])
    scheduler = StepLR(optimizer, step_size=cfg['num_epochs'] // 3, gamma=0.1)

    start_epoch = 1
    best_rank1 = 0.0
    if args.resume:
        ckpt = torch.load(args.resume, map_location=device)
        model.load_state_dict(ckpt['model_state_dict'])
        optimizer.load_state_dict(ckpt['optimizer_state_dict'])
        start_epoch = ckpt['epoch'] + 1
        best_rank1 = ckpt.get('best_rank1', 0.0)
        print(f'Resumed from epoch {start_epoch - 1}, best Rank-1: {best_rank1:.2%}')

    for epoch in range(start_epoch, cfg['num_epochs'] + 1):
        # 分层冻结
        freeze_layers(model, cfg['freeze_until_epoch'], epoch)
        # 阶段 B：conv1 仍冻结
        if epoch <= cfg['partial_freeze_until_epoch']:
            for name, p in model.named_parameters():
                if name.startswith('conv1'):
                    p.requires_grad = False

        print(f'\nEpoch {epoch}/{cfg["num_epochs"]}, lr={get_lr(optimizer):.6f}')
        train_metrics = train_one_epoch(
            model, train_loader,
            criterion_id, criterion_triplet, criterion_center, criterion_consistency,
            optimizer, cfg, device, epoch
        )
        print(f"Train -> loss: {train_metrics['loss']:.4f}, "
              f"id: {train_metrics['id']:.4f}, tri: {train_metrics['triplet']:.4f}, "
              f"cen: {train_metrics['center']:.4f}, con: {train_metrics['consistency']:.4f}")

        scheduler.step()

        if epoch % cfg['eval_interval'] == 0 or epoch == cfg['num_epochs']:
            metrics = evaluate(model, val_loader, val_loader.dataset, device)
            print(f"Val   -> Rank-1: {metrics['rank1']:.2%}, mAP: {metrics['mAP']:.2%}")
            if metrics['same_clothes_rank1'] is not None:
                print(f"       Same-clothes Rank-1: {metrics['same_clothes_rank1']:.2%}")
            if metrics['cross_clothes_rank1'] is not None:
                print(f"       Cross-clothes Rank-1: {metrics['cross_clothes_rank1']:.2%}")

            is_best = metrics['rank1'] > best_rank1
            best_rank1 = max(best_rank1, metrics['rank1'])

            if is_best or epoch % cfg['save_interval'] == 0:
                ckpt_path = os.path.join(cfg['output_dir'], f'checkpoint_epoch{epoch}.pth')
                torch.save({
                    'epoch': epoch,
                    'model_state_dict': model.state_dict(),
                    'optimizer_state_dict': optimizer.state_dict(),
                    'best_rank1': best_rank1,
                    'metrics': metrics,
                    'cfg': cfg,
                }, ckpt_path)
                print(f'Saved checkpoint: {ckpt_path}')
                if is_best:
                    best_path = os.path.join(cfg['output_dir'], 'best_model.pth')
                    torch.save({
                        'epoch': epoch,
                        'model_state_dict': model.state_dict(),
                        'best_rank1': best_rank1,
                        'metrics': metrics,
                        'cfg': cfg,
                    }, best_path)
                    print(f'New best model saved: {best_path}')

    print(f'\nTraining finished. Best Rank-1: {best_rank1:.2%}')


if __name__ == '__main__':
    main()
