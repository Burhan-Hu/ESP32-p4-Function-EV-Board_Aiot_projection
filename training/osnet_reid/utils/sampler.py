"""
PK 采样器：每个 batch 包含 P 个身份，每个身份 K 张图。
额外保证：若某身份有跨衣服样本，尽量同时选中，强化 Triplet 跨衣服正样本。
"""

import copy
import random
from collections import defaultdict

from torch.utils.data import Sampler


class CrossClothesPKSampler(Sampler):
    """PK 采样器，鼓励跨衣服正样本对。

    Args:
        dataset: PRCCUpperBodyDataset 或类似数据集，samples 元素为 (path, pid, cam, clothes)。
        num_instances: 每个身份的图像数 K。
    """

    def __init__(self, dataset, num_instances: int = 4):
        self.dataset = dataset
        self.num_instances = num_instances
        self.index_dict = defaultdict(list)

        for idx, sample in enumerate(dataset.samples):
            pid = sample[1]
            self.index_dict[pid].append(idx)

        self.pids = list(self.index_dict.keys())

        # 计算每个 epoch 的迭代次数
        self.length = 0
        for pid in self.pids:
            self.length += len(self.index_dict[pid]) // num_instances * num_instances

    def __iter__(self):
        indices = []
        for pid in self.pids:
            idxs = copy.deepcopy(self.index_dict[pid])
            random.shuffle(idxs)

            # 补齐到 num_instances 的整数倍
            rem = len(idxs) % self.num_instances
            if rem != 0:
                idxs += random.choices(idxs, k=self.num_instances - rem)

            # 按衣服分组并交替抽取，鼓励跨衣服样本成对出现
            clothes_groups = defaultdict(list)
            for i in idxs:
                clothes = self.dataset.samples[i][3]
                clothes_groups[clothes].append(i)

            pid_indices = []
            keys = list(clothes_groups.keys())
            while any(clothes_groups[k] for k in keys):
                for k in keys:
                    group = clothes_groups[k]
                    if group:
                        pid_indices.append(group.pop(random.randrange(len(group))))

            for k in keys:
                pid_indices.extend(clothes_groups[k])

            # 按 num_instances 分块
            for i in range(0, len(pid_indices), self.num_instances):
                indices.extend(pid_indices[i:i + self.num_instances])

        random.shuffle(indices)
        return iter(indices)

    def __len__(self):
        return self.length
