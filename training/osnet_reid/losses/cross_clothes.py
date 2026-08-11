"""换装一致性约束：同一人不同衣服的特征应接近。"""

import torch
import torch.nn as nn
import torch.nn.functional as F


class CrossClothesConsistencyLoss(nn.Module):
    """强制同一身份、不同衣服对的余弦相似度大于 margin。

    实现：在 batch 内找出所有 (pid 相同, clothes 不同) 的配对，
         计算 L = max(0, margin - cos(f_i, f_j))。
    """

    def __init__(self, margin: float = 0.6):
        super().__init__()
        self.margin = margin

    def forward(
        self,
        features: torch.Tensor,
        pids: torch.Tensor,
        clothes: torch.Tensor,
    ) -> torch.Tensor:
        features = F.normalize(features, p=2, dim=1)
        N = features.size(0)

        loss = 0.0
        count = 0
        for i in range(N):
            for j in range(i + 1, N):
                if pids[i] == pids[j] and clothes[i] != clothes[j]:
                    cos_sim = F.cosine_similarity(features[i:i + 1], features[j:j + 1], dim=1)
                    loss += F.relu(self.margin - cos_sim).sum()
                    count += 1

        if count == 0:
            return torch.tensor(0.0, device=features.device, requires_grad=True)
        return loss / count
