"""上半身 OSNet 模型构建器。

注意：torchreid 0.2.5 官方未提供 osnet_ibn_x0_25，但 osnet_x0_25 支持
传入 IN=True，从而在 conv1/conv2 及 OSBlock 后启用 Instance Norm，
达到与 OSNet-IBN 类似的跨外观效果，且保持 x0.25 的轻量体型。
"""

import torch
import torch.nn as nn
from torchreid.reid.models.osnet import osnet_x0_25


def build_osnet_upperbody(
    num_classes: int,
    feature_dim: int = 128,
    pretrained: bool = True,
    use_ibn: bool = True,
):
    """构建上半身 OSNet-x0.25（IBN 风格）。

    Args:
        num_classes: 训练集身份数。
        feature_dim: 输出特征维度，建议 128 以适配 ESP32-P4 存储。
        pretrained: 是否加载 ImageNet 预训练权重。
        use_ibn: 是否启用 Instance Norm（模拟 IBN）。
    """
    model = osnet_x0_25(
        num_classes=num_classes,
        pretrained=pretrained,
        loss='triplet',          # 返回 (logits, features)
        feature_dim=feature_dim,
        IN=use_ibn,
    )
    return model


def freeze_layers(model: nn.Module, freeze_until_epoch: int, current_epoch: int):
    """按训练阶段分层冻结/解冻。"""
    # 命名约定：conv1, maxpool, conv2, conv3, conv4, conv5, global_avgpool, fc, classifier
    # 阶段 A：冻结 conv1, maxpool, conv2
    # 阶段 B：解冻 conv2，仍冻结 conv1
    # 阶段 C：全部解冻

    # 默认全部可训练
    for p in model.parameters():
        p.requires_grad = True

    if current_epoch <= freeze_until_epoch:
        # 冻结 stem + block1~2
        for name, p in model.named_parameters():
            if name.startswith('conv1') or name.startswith('conv2'):
                p.requires_grad = False
    # 阶段 B：conv1 保持冻结，其他训练（已通过上面默认开启）
    # 若需要显式冻结 conv1：
    # if current_epoch <= partial_freeze_until_epoch:
    #     for name, p in model.named_parameters():
    #         if name.startswith('conv1'):
    #             p.requires_grad = False


def get_model_summary(model: nn.Module, input_size=(1, 3, 128, 64)):
    """打印模型参数量与推理测试。"""
    dummy = torch.randn(*input_size)
    model.eval()
    with torch.no_grad():
        out = model(dummy)
    total = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(f"Total params: {total / 1e3:.1f}K, Trainable: {trainable / 1e3:.1f}K")
    print(f"Output shape: {out.shape}")
