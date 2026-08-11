# OSNet-IBN-x0.25 上半身 Re-ID 云端训练

本目录包含 ESP32-P4 AIoT 伴学终端 Re-ID 模块的**云端训练代码**，基于 [OSNet-IBN-x0.25](https://arxiv.org/abs/1905.00953)（torchreid 中通过 `osnet_x0_25` + `IN=True` 实现）在 PRCC 上半身数据上微调。

---

## 1. 目录结构

```text
training/osnet_reid/
├── config.yaml                 # 训练配置
├── requirements.txt            # Python 依赖
├── train.py                    # 主训练脚本
├── export_onnx.py              # 导出 ONNX
├── tools/
│   └── convert_tflite_int8.py  # ONNX → INT8 TFLite
├── datasets/
│   ├── prcc.py                 # PRCC 上半身数据集
│   └── transforms.py           # 数据增强
├── models/
│   └── osnet_upperbody.py      # OSNet 模型构建器
├── losses/
│   ├── triplet.py              # Batch Hard Triplet Loss
│   ├── center_loss.py          # Center Loss
│   └── cross_clothes.py        # 换装一致性约束
└── utils/
    ├── sampler.py              # 跨衣服 PK 采样器
    └── metrics.py              # Rank-1 / mAP 评估
```

---

## 2. 环境准备

### 2.1 推荐：使用 base Anaconda 环境（已预装 torch）

当前机器 base 环境已安装 `torch 2.9.0+cpu`、`torchvision`、`cv2` 等，直接安装剩余依赖即可：

```bash
cd training/osnet_reid
pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
```

> 若创建独立 venv 安装 torch，Windows 可能出现 `File name too long` 错误（torch 内部路径过长），建议：
> - 方案 A：使用 base Anaconda 环境；
> - 方案 B：在 Windows 组策略中启用“启用 Win32 长路径”，然后重新创建 venv。

### 2.2 下载 PRCC 数据集

PRCC 官方下载：
- 论文主页：https://crm-prcc.github.io/
- 或搜索 `PRCC dataset Person Re-identification`

解压后目录示例：

```text
data/prcc/
├── rgb/
│   ├── train/
│   │   ├── 001/
│   │   │   ├── 001_001_A_001.jpg
│   │   │   ├── 001_001_A_002.jpg
│   │   │   ├── 001_001_B_001.jpg
│   │   │   ├── 001_002_C_001.jpg   <-- 跨衣服
│   │   └── ...
│   └── test/
│       └── ...
```

文件名或父目录中需包含摄像头标记 `A` / `B` / `C`：
- `A`、`B`：同一件衣服
- `C`：不同衣服

将数据集放到 `training/osnet_reid/data/prcc`（或修改 `config.yaml` 中的 `data_root`）。

---

## 3. 训练

### 3.1 单卡训练

```bash
cd training/osnet_reid
python train.py --config config.yaml --device cpu
```

若本机有 NVIDIA GPU，可改为 `--device cuda`。

### 3.2 训练流程说明

1. **阶段 A（Epoch 1~10）**：冻结 `conv1` + `conv2`，只训练上层 `conv3`/`conv4`/`conv5`/`fc`/`classifier`，学习率 `1e-3`。
2. **阶段 B（Epoch 11~20）**：解冻 `conv2`，仍冻结 `conv1`，学习率降到 `1e-4`。
3. **阶段 C（Epoch 21~30，可选）**：全部解冻，学习率 `1e-5`，早停防止过拟合。

损失函数组合：

```text
L = L_id + L_triplet + 0.0005 * L_center + 0.5 * L_consistency
```

- `L_id`：Softmax 身份分类
- `L_triplet`：Batch Hard Triplet，强制跨衣服正样本
- `L_center`：Center Loss，增强类内紧凑
- `L_consistency`：同一人不同衣服的余弦相似度约束

### 3.3 输出

训练结果保存在 `outputs/`：

```text
outputs/
├── checkpoint_epoch5.pth
├── checkpoint_epoch10.pth
├── ...
└── best_model.pth
```

---

## 4. 导出 ONNX

```bash
python export_onnx.py \
    --checkpoint outputs/best_model.pth \
    --output outputs/osnet_upperbody.onnx \
    --input_h 128 --input_w 64 \
    --verify
```

验证通过后会显示：

```text
PyTorch vs ONNX max diff: x.xxxxe-xx, cosine similarity: 1.000000
Verification passed.
```

---

## 5. 转换为 INT8 TFLite

```bash
pip install onnx2tf tensorflow

python tools/convert_tflite_int8.py \
    --onnx outputs/osnet_upperbody.onnx \
    --output outputs/osnet_upperbody_int8.tflite \
    --calib_data data/prcc/rgb/val \
    --input_h 128 --input_w 64 \
    --num_calib 200
```

输出 `osnet_upperbody_int8.tflite` 即可烧录到 ESP32-P4 SPIFFS，供 TFLite Micro 加载。

> 如果 TensorFlow 安装困难，也可用 [ESP-PPQ](https://github.com/espressif/esp-ppq) 直接将 ONNX 量化为 ESP-DL 的 `.espdl` 格式。

---

## 6. 关键配置项

在 `config.yaml` 中可调：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `image_size` | `[128, 64]` | 模型输入尺寸 |
| `upper_body_ratio` | `0.6` | 上半身裁剪比例 |
| `feature_dim` | `128` | 输出特征维度 |
| `use_ibn` | `true` | 启用 Instance Norm（模拟 IBN） |
| `batch_size` | `32` | 受显存/内存限制可调 |
| `num_instances` | `4` | PK 采样每身份张数 |
| `loss_consistency_weight` | `0.5` | 换装一致性权重 |
| `consistency_margin` | `0.6` | 同一人跨衣服余弦相似度下限 |

---

## 7. 下一步

1. 准备 PRCC 数据并运行 `train.py`；
2. 观察 `Val Rank-1` 和 `Cross-clothes Rank-1`；
3. 满意后导出 ONNX 并量化；
4. 将 `.tflite` 放到 ESP32-P4 的 SPIFFS，继续边缘部署（见 `docs/product_functional_design_v5_osnet.md`）。
