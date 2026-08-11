"""
PRCC 上半身数据集加载器

支持两种目录结构：

结构 A（实际 PRCC 解压后常见）：
  data_root/rgb/<split>/<cam>/<pid>/...jpg
  例如：data/prcc/rgb/train/A/001/cropped_rgb001.jpg

结构 B：
  data_root/rgb/<split>/<pid>/...jpg
  例如：data/prcc/rgb/train/001/001_A_001.jpg

摄像头约定：
  - A、B：同一件衣服
  - C：不同衣服（跨衣服）
"""

import os
import re
import glob
from typing import List, Tuple, Callable, Optional

from PIL import Image
from torch.utils.data import Dataset


CAM_PATTERNS = re.compile(r'(?:^|[_\-/])([ABC])(?:[_\-/]|$)')


def _cam_to_ids(cam: str) -> Tuple[int, int]:
    cam_id = {'A': 0, 'B': 1, 'C': 2}.get(cam, 0)
    clothes_id = 0 if cam in ('A', 'B') else 1
    return cam_id, clothes_id


def _parse_cam_from_path(path: str) -> Tuple[Optional[str], int, int]:
    """从目录结构或文件名解析摄像头。

    返回 (cam_letter, cam_id, clothes_id)。若无法解析，cam_letter 为 None。
    """
    parts = path.replace('\\', '/').split('/')
    # 先看目录中是否有 A/B/C 摄像头文件夹
    for p in parts:
        if p in ('A', 'B', 'C'):
            cam_id, clothes_id = _cam_to_ids(p)
            return p, cam_id, clothes_id

    # 再看文件名
    text = os.path.basename(path)
    m = CAM_PATTERNS.search(text)
    if m:
        cam = m.group(1)
        cam_id, clothes_id = _cam_to_ids(cam)
        return cam, cam_id, clothes_id

    return None, 0, 0


class PRCCUpperBodyDataset(Dataset):
    """PRCC 上半身数据集。

    Args:
        data_root: PRCC 根目录。
        split: 'train' / 'val' / 'test'。
        transform: torchvision 变换，接收 PIL Image，输出 tensor。
        upper_body_ratio: 裁剪上半身比例。
    """

    def __init__(
        self,
        data_root: str,
        split: str = 'train',
        transform: Optional[Callable] = None,
        upper_body_ratio: float = 0.6,
    ):
        self.data_root = data_root
        self.split = split
        self.transform = transform
        self.upper_body_ratio = upper_body_ratio

        self.samples: List[Tuple[str, int, int, int]] = []
        self._scan()

        if len(self.samples) == 0:
            raise RuntimeError(
                f"未在 {data_root} 下找到 {split} 集图像，请检查数据路径。"
            )

        self.pid_map = {pid: idx for idx, pid in enumerate(sorted({s[1] for s in self.samples}))}
        self.samples = [
            (img_path, self.pid_map[pid], cam_id, clothes_id)
            for img_path, pid, cam_id, clothes_id in self.samples
        ]

    def _scan(self):
        candidates = [
            os.path.join(self.data_root, 'rgb', self.split),
            os.path.join(self.data_root, self.split),
        ]
        for root in candidates:
            if not os.path.isdir(root):
                continue
            for img_path in glob.glob(os.path.join(root, '**', '*.*'), recursive=True):
                ext = os.path.splitext(img_path)[1].lower()
                if ext not in ('.jpg', '.jpeg', '.png', '.bmp'):
                    continue

                # 尝试结构 A：.../<cam>/<pid>/image.jpg
                norm = img_path.replace('\\', '/')
                parts = norm.split('/')
                pid_str = None
                if len(parts) >= 2:
                    parent = parts[-2]      # pid
                    grandparent = parts[-3] if len(parts) >= 3 else ''  # cam
                    if grandparent in ('A', 'B', 'C'):
                        try:
                            pid_str = parent
                            pid = int(pid_str)
                            cam_letter, cam_id, clothes_id = _cam_to_ids(grandparent)
                            self.samples.append((img_path, pid, cam_id, clothes_id))
                            continue
                        except ValueError:
                            pass

                # 尝试结构 B：.../<pid>/image.jpg（摄像头从文件名解析）
                pid_str = os.path.basename(os.path.dirname(img_path))
                try:
                    pid = int(pid_str)
                except ValueError:
                    continue
                _, cam_id, clothes_id = _parse_cam_from_path(img_path)
                self.samples.append((img_path, pid, cam_id, clothes_id))

            if self.samples:
                break

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, index):
        img_path, pid, cam_id, clothes_id = self.samples[index]
        img = Image.open(img_path).convert('RGB')

        # 上半身裁剪：取上半 upper_body_ratio
        w, h = img.size
        crop_h = int(h * self.upper_body_ratio)
        if crop_h < 1:
            crop_h = h
        img = img.crop((0, 0, w, crop_h))

        if self.transform is not None:
            img = self.transform(img)

        return {
            'img': img,
            'pid': pid,
            'cam': cam_id,
            'clothes': clothes_id,
            'path': img_path,
        }

    @property
    def num_classes(self) -> int:
        return len(self.pid_map)
