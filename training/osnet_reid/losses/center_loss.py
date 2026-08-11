"""Center Loss。"""

import torch
import torch.nn as nn


class CenterLoss(nn.Module):
    """Center Loss，增强类内紧凑性。

    Args:
        num_classes: 身份数量。
        feat_dim: 特征维度。
    """

    def __init__(self, num_classes: int, feat_dim: int):
        super().__init__()
        self.num_classes = num_classes
        self.centers = nn.Parameter(torch.randn(num_classes, feat_dim))

    def forward(self, features: torch.Tensor, labels: torch.Tensor) -> torch.Tensor:
        batch_size = features.size(0)
        centers_batch = self.centers.index_select(0, labels)
        loss = (features - centers_batch).pow(2).sum() / batch_size / 2
        return loss
