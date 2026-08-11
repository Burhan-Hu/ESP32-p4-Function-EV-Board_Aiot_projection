"""Triplet Hard Loss with Batch Hard mining."""

import torch
import torch.nn as nn
import torch.nn.functional as F


class TripletHardLoss(nn.Module):
    """Batch Hard Triplet Loss。

    Args:
        margin: 三元组间隔。
        normalize_feature: 是否先对特征做 L2 归一化。
    """

    def __init__(self, margin: float = 0.3, normalize_feature: bool = True):
        super().__init__()
        self.margin = margin
        self.normalize_feature = normalize_feature

    def forward(self, features: torch.Tensor, labels: torch.Tensor) -> torch.Tensor:
        if self.normalize_feature:
            features = F.normalize(features, p=2, dim=1)

        dist_mat = self._euclidean_dist(features, features)

        N = dist_mat.size(0)
        is_pos = labels.expand(N, N).eq(labels.expand(N, N).t()).float()
        is_neg = labels.expand(N, N).ne(labels.expand(N, N).t()).float()

        #  hardest positive
        dist_ap, _ = torch.max(dist_mat * is_pos - 9999999 * (1 - is_pos), dim=1)
        #  hardest negative
        dist_an, _ = torch.min(dist_mat * is_neg + 9999999 * (1 - is_neg), dim=1)

        y = torch.ones_like(dist_an)
        loss = F.margin_ranking_loss(dist_an, dist_ap, y, margin=self.margin)
        return loss

    @staticmethod
    def _euclidean_dist(x: torch.Tensor, y: torch.Tensor) -> torch.Tensor:
        m, n = x.size(0), y.size(0)
        xx = torch.pow(x, 2).sum(1, keepdim=True).expand(m, n)
        yy = torch.pow(y, 2).sum(1, keepdim=True).expand(n, m).t()
        dist = xx + yy
        dist.addmm_(x, y.t(), beta=1, alpha=-2)
        dist = dist.clamp(min=1e-12).sqrt()
        return dist
