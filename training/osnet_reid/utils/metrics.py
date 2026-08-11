"""Re-ID 评估指标：Rank-1 / mAP。"""

import numpy as np
from typing import Tuple


def compute_distance_matrix(query_features: np.ndarray, gallery_features: np.ndarray) -> np.ndarray:
    """计算余弦距离矩阵。"""
    query_features = query_features / (np.linalg.norm(query_features, axis=1, keepdims=True) + 1e-8)
    gallery_features = gallery_features / (np.linalg.norm(gallery_features, axis=1, keepdims=True) + 1e-8)
    # 余弦相似度 -> 余弦距离
    dist = 1 - np.dot(query_features, gallery_features.T)
    return dist


def eval_market1501(
    distmat: np.ndarray,
    query_pids: np.ndarray,
    gallery_pids: np.ndarray,
    query_camids: np.ndarray,
    gallery_camids: np.ndarray,
    max_rank: int = 20,
) -> Tuple[float, float]:
    """计算 CMC 和 mAP，过滤同摄像头同身份（gallery 中同一摄像头同一身份视为 junk）。"""
    num_q, num_g = distmat.shape

    if num_g < max_rank:
        max_rank = num_g

    indices = np.argsort(distmat, axis=1)  # 距离升序
    matches = (gallery_pids[indices] == query_pids[:, np.newaxis]).astype(np.int32)

    all_cmc = []
    all_ap = []
    num_valid_q = 0

    for q_idx in range(num_q):
        q_pid = query_pids[q_idx]
        q_camid = query_camids[q_idx]

        order = indices[q_idx]
        remove = (gallery_pids[order] == q_pid) & (gallery_camids[order] == q_camid)
        keep = ~remove

        raw_cmc = matches[q_idx][keep]
        if not np.any(raw_cmc):
            continue

        cmc = raw_cmc.cumsum()
        cmc[cmc > 1] = 1
        all_cmc.append(cmc[:max_rank])

        # AP
        num_rel = raw_cmc.sum()
        tmp_cmc = raw_cmc.cumsum()
        y = np.arange(1, num_g + 1)
        tmp_cmc = tmp_cmc * (1.0 / y)
        tmp_cmc = tmp_cmc * raw_cmc
        ap = tmp_cmc.sum() / num_rel
        all_ap.append(ap)
        num_valid_q += 1

    if num_valid_q == 0:
        return 0.0, 0.0

    all_cmc = np.asarray(all_cmc).astype(np.float32)
    all_cmc = all_cmc.sum(0) / num_valid_q
    mAP = np.mean(all_ap)
    return float(all_cmc[0]), float(mAP)
