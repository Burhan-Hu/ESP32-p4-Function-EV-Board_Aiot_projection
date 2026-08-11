"""数据增强与预处理。"""

from torchvision import transforms


def build_transforms(image_size, mean, std, is_train=True):
    """构建 torchvision 变换。

    Args:
        image_size: [H, W]
        mean: RGB 均值
        std: RGB 标准差
        is_train: 训练/验证
    """
    h, w = image_size
    if is_train:
        return transforms.Compose([
            transforms.Resize((h, w)),
            transforms.RandomHorizontalFlip(p=0.5),
            transforms.RandomCrop((h, w), padding=4),
            transforms.ColorJitter(brightness=0.2, contrast=0.2, saturation=0.1),
            transforms.RandomAffine(degrees=5, translate=(0.05, 0.05)),
            transforms.ToTensor(),
            transforms.Normalize(mean=mean, std=std),
        ])
    else:
        return transforms.Compose([
            transforms.Resize((h, w)),
            transforms.ToTensor(),
            transforms.Normalize(mean=mean, std=std),
        ])
