# ESP32-P4 AIoT 伴学终端 — 基于 OSNet-IBN-x0.25 的 Re-ID 完整产品方案（v5）

> 版本：v5.0  
> 日期：2026-07-04  
> 目标硬件：ESP32-P4-Function-EV-Board  
> 目标平台：ESP-IDF v5.5.2 + TFLite Micro（或 ESP-DL）  
> 核心模型：OSNet-IBN-x0.25（ICCV 2019）上半身微调版  
> 设计目标：**不追求复现博士论文，而是用更成熟、更轻量、更适合上半身场景的公开方案，真正落地跨外观 Re-ID**

---

## 1. 方案可行性评估

### 1.1 为什么选 OSNet-IBN-x0.25？

| 需求 | OSNet-IBN-x0.25 是否匹配 | 说明 |
|------|--------------------------|------|
| **轻量** | ✅ 非常匹配 | 宽度乘子 0.25，参数量约 200K~500K，INT8 量化后 < 300KB |
| **跨外观** | ✅ 匹配 | IBN（Instance-Batch Normalization）对衣服颜色/纹理变化天然鲁棒 |
| **上半身场景** | ✅ 高度匹配 | 多尺度融合能同时捕捉头肩轮廓、 torso 结构、姿态等上半身身份线索 |
| **端侧部署** | ✅ 可行 | 标准 Conv+BN+ReLU+AvgPool+FC，TFLite Micro / ESP-DL 全支持 |
| **推理速度** | ⚠️ 需验证 | 预估 100~300ms/帧（ESP32-P4 纯 CPU INT8），可接受事件触发式推理 |
| **精度预期** | ⚠️ 有限但够用 | PRCC 上半身跨衣服 Rank-1 目标 > 50%，同衣服 > 90% |

### 1.2 与博士论文方法的关系

本方案**不直接复现**论文的 M2Net/CAG/MVMA，但继承了论文的**核心思想**：

| 论文思想 | 本方案实现 |
|----------|------------|
| 解耦外观与身份 | IBN 层 + Triplet 跨衣服采样 |
| 多尺度特征 | OSNet 动态多尺度聚合 |
| 跨外观损失 | Triplet Hard + 换装一致性约束 |
| 数据增强 | PRCC 上半身裁剪 + 颜色抖动/模糊；注册时多衣服采集替代 CAG |
| 视角建模 | 视角分桶 + 可选 ViewNet / 规则视角 |
| 度量学习 | L2 归一化 + 余弦相似度 |

**结论**：OSNet-IBN-x0.25 是论文思想在公开生态中的**轻量落地载体**，比手工特征方案强，比完整论文蒸馏方案快、稳、易实现。

### 1.3 预期效果

| 场景 | 预期指标 | 说明 |
|------|----------|------|
| 同衣服返回识别 | > 80~90% | 上半身特征稳定 |
| 换外套返回识别 | > 50~60% | IBN + Triplet 提供基础换装能力 |
| 注册时采集 2 套衣服 | > 70% | 多外观模板可显著提升 |
| 陌生人过滤 | > 85% | 阈值可调的余弦距离判定 |
| 单帧推理耗时 | 100~300ms | 事件触发，不每帧运行 |
| 模型大小 | < 300KB（INT8） | 可放 Flash/SPIFFS |

> 注：以上为演示级目标，非安防级精度。若追求更高换装精度，需增大模型、增加数据或引入云端二次确认。

---

## 2. 系统总体架构

```text
┌─────────────────────────────────────────────────────────────────────────┐
│  阶段一：云端训练（PC/服务器/Colab）                                      │
│  ├── OSNet-IBN-x0.25 预训练权重（ImageNet + Market-1501/Duke）          │
│  ├── PRCC 上半身数据集改造                                              │
│  ├── 冻结底层 + 顶层微调 + Triplet + 换装一致性约束                     │
│  └── 导出 ONNX → INT8 PTQ → .tflite                                     │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼ 烧录到 ESP32-P4 Flash/SPIFFS
┌─────────────────────────────────────────────────────────────────────────┐
│  阶段二：边缘运行（ESP32-P4）                                             │
│  ├── TFLite Micro 解释器加载 osnet_upperbody.tflite                     │
│  ├── human_detect 提供行人 bbox                                         │
│  ├── ROI 裁剪 → 128×64 → 归一化/量化 → 推理                             │
│  ├── 输出 128 维特征 → L2 归一化 → Gallery 匹配                         │
│  └── 与状态机/UI/语音/监控闭环对接                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 云端训练方案

### 3.1 基础模型

- **模型**：OSNet-IBN-x0.25
- **输入**：128×64×3 RGB（上半身适配，比原 256×128 更小）
- **输出**：128 维身份特征向量（原模型可能输出 256/512，改为 128 以节省存储）
- **预训练**：ImageNet → Market-1501 / DukeMTMC（常用 Re-ID 预训练）

模型结构要点：

```text
Input 128×64×3
    │
    ▼
Stem: Conv3x3 → BN → ReLU → MaxPool
    │
    ▼
OSNet Block × 4
    ├── 1×1 分支：局部细节
    ├── 3×3 分支：中尺度结构
    ├── 5×5 分支：大尺度轮廓
    ├── 动态聚合：学习权重融合多尺度
    └── IBN：前半通道 Instance Norm，后半 Batch Norm
    │
    ▼
Global Average Pooling
    │
    ▼
FC(64 → 128) → 特征向量
```

### 3.2 数据集：PRCC 上半身专用版

- **原始 PRCC**：221 人，3 摄像头，A 摄像头衣服 1，B/C 摄像头衣服 2。
- **上半身裁剪**：
  - 检测框 `[x, y, w, h]`；
  - 取上半身 60%：`[x, y, w, h×0.6]`（头顶到髋关节）；
  - Resize 到 128×64，保持宽高比，短边填充。
- **训练/验证划分**：
  - 按人划分，训练集与测试集人不重叠；
  - 子集：同衣服测试（A→A）、跨衣服测试（A→B/C）。

数据增强：

- 随机裁剪（模拟检测框抖动）；
- 颜色抖动（亮度、对比度，模拟光照）；
- 高斯模糊（模拟焦距变化）；
- 谨慎使用水平翻转（上半身左右不对称，如头发分线）。
- **不做随机擦除**：上半身区域小，擦除可能丢失关键身份区域。

### 3.3 微调策略：分层冻结

PRCC 只有 221 人，数据量小，全微调易过拟合。采用分层解冻：

| 阶段 | Epoch | 冻结层 | 训练层 | 学习率 |
|------|-------|--------|--------|--------|
| A | 1~10 | Stem + Block 1~2 | Block 3~4 + 聚合层 + FC | 1e-3 |
| B | 11~20 | Block 1 | Block 2~4 + 顶层 | 1e-4 |
| C（可选） | 21~30 | 无 | 全部 | 1e-5 |

- 阶段 A 必须做；阶段 B 视验证指标决定；阶段 C 通常跳过，早停防止过拟合。

### 3.4 损失函数

联合损失：

```text
L_total = L_id + λ_triplet × L_triplet + λ_center × L_center
```

- **L_id**：Softmax 交叉熵，身份分类。
- **L_triplet**：Batch Hard Triplet Loss。
  - PK 采样：P 人 × K 张；
  - **关键改造**：强制正样本为“同一人、不同衣服”；
  - 负样本：不同人（同衣或异衣）。
- **L_center**：Center Loss（可选），增强类内紧凑性。

**换装一致性约束**：

```text
对同一人衣服 1 的图 x_i 和衣服 2 的图 x_j：
  L_consistency = max(0, α - cos(f_i, f_j))
```

迫使同一人跨衣服特征相似度高于阈值 α（如 0.6）。

### 3.5 验证指标

| 指标 | 目标 | 说明 |
|------|------|------|
| 同衣服 Rank-1 | > 90% | 验证基础能力 |
| 跨衣服 Rank-1 | > 50% | 核心换装能力 |
| 跨衣服 mAP | > 35% | 检索整体质量 |

### 3.6 导出 ONNX

- 加载最佳 epoch 权重；
- `model.eval()`，使用 running statistics；
- dummy input：`torch.randn(1, 3, 128, 64)`；
- `torch.onnx.export(..., opset_version=11, input_names=["input"], output_names=["feature"])`；
- 验证 ONNX 与 PyTorch 输出误差 < 1e-5。

---

## 4. 模型压缩与量化

### 4.1 结构精简

- 输入从 256×128 降到 128×64；
- Stem 第一个 Conv stride 可从 2 改为 1（避免过度下采样）；
- 输出维度从 256/512 降到 128；
- Block 3~4 可考虑进一步压缩到 x0.125，但需谨慎（上层特征关键）。

### 4.2 量化策略：INT8 PTQ（优先）

```text
FP32 ONNX
    │
    ▼
准备校准集：100~500 张 PRCC 上半身验证图
    │
    ▼
INT8 后训练量化（per-channel weight, per-tensor activation）
    │
    ▼
验证 INT8 vs FP32 特征余弦相似度 > 0.98
    │
    ▼
导出 .tflite
```

若 PTQ 后跨衣服 Rank-1 下降 > 5%，改用 **QAT（量化感知训练）** 重新训练 5~10 epoch。

### 4.3 部署格式选择

| 引擎 | 优点 | 缺点 | 建议 |
|------|------|------|------|
| **TFLite Micro** | 生态成熟，OSNet 算子全支持 | 无 SIMD 优化，速度一般 | **首选** |
| ESP-DL | 乐鑫优化，双核调度 | 需转换为 .espdl，工具链较新 | 备选 |
| 自定义 RISC-V P 扩展 | 速度潜力最大 | 开发成本高 | 最后手段 |

**本方案首选 TFLite Micro**：把 `.tflite` 烧录到 SPIFFS，运行时加载到 RAM/PSRAM。

---

## 5. ESP32-P4 边缘部署

### 5.1 内存布局

| 区域 | 内容 | 大小估算 |
|------|------|----------|
| SPI Flash / SPIFFS | `osnet_upperbody.tflite`（INT8） | ~200~300KB |
| SPI Flash / SPIFFS | `reid_gallery_v2.bin` | ~5KB |
| 内部 SRAM | TFLite 解释器工作区 | ~100KB |
| 内部 SRAM | 输入 tensor 24KB + 中间特征图 32KB + 输出 0.1KB | ~60KB |
| 内部 SRAM | 当前 ROI 缓存、应用状态 | ~50KB |
| PSRAM（若可用） | 模型/特征图备选 | 8~16MB |

> 若内部 SRAM 紧张，可将模型或中间 tensor 放到 PSRAM，速度损失约 10~20%。

### 5.2 预处理流水线

```text
RGB565 整帧（1024×600）
    │
    ▼
human_detect → 行人 bbox
    │
    ▼
裁剪上半身 ROI：bbox 上半 60%
    │
    ▼
Resize 128×64（双线性或最近邻）
    │
    ▼
RGB565 → RGB888
    │
    ▼
归一化：((x/255) - mean) / std
  mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225]
    │
    ▼
量化到 INT8：乘以 scale，加上 zero_point
    │
    ▼
输入 TFLite 解释器
```

### 5.3 推理与后处理

```text
TFLite Invoke
    │
    ▼
输出 128 维特征（INT8 或 FP32）
    │
    ▼
L2 归一化：f = f / ||f||
    │
    ▼
加载 Gallery 模板（已预归一化）
    │
    ▼
视角过滤：优先同视角 + 相邻视角桶
    │
    ▼
批量点积：score_i = dot(query, template_i)
    │
    ▼
决策：
  score > 0.75 → SELF（本人）
  score < 0.45 → STRANGER（陌生人）
  中间 → UNKNOWN（未知，可 VLM 兜底）
```

---

## 6. 与现有项目功能缝合

### 6.1 新增组件：`components/reid_dl/`

对外 API 保持与 v3/v4 一致，内部改为 TFLite Micro 推理：

```c
esp_err_t reid_dl_init(const char *model_path);
esp_err_t reid_dl_enroll_start(const char *name);
esp_err_t reid_dl_enroll_feed(frame_t *frame, bbox_t *bbox);
esp_err_t reid_dl_enroll_finish(uint8_t *out_id);
esp_err_t reid_dl_identify(frame_t *frame, bbox_t *bbox,
                            reid_result_t *result, uint8_t *id,
                            float *score, reid_view_t *view);
```

### 6.2 复用的现有功能

| 现有功能 | 缝合方式 |
|----------|----------|
| `human_detect` | 提供行人 bbox 与在座/离座事件，触发 Re-ID |
| `camera_capture` | 提供 RGB565 整帧，用于 ROI 裁剪 |
| 语音唤醒 + ASR | 新增“记住我，我叫 XXX”和“开始监测/结束监测”命令 |
| 豆包 TTS | 播放欢迎语、陌生人报警、离座提醒（带名字） |
| PIR + 监控模式 | PIR 触发后先 Re-ID，陌生人再 VLM 双图对比 |
| LVGL UI | 显示“XXX 在座 / 陌生人 / 未知”与个人专注时长 |
| LED | 在座亮灯（已集成） |

### 6.3 三大业务闭环（与 v3/v4 一致，内核升级）

#### 闭环一：入座识别 + 专注陪伴

```text
学生入座
  → human_detect 确认在座
  → reid_dl_identify() 运行 OSNet-IBN
  → 匹配注册身份
  → TTS：“欢迎回来，小明。”
  → UI 显示“小明 在座”，恢复个人计时
```

#### 闭环二：陌生人监控

```text
用户说“开始监测”
  → 保存参考帧 + 当前身份白名单
  → PIR 触发
  → reid_dl_identify()
  → 白名单 → 忽略
  → 陌生人 → VLM 确认 → TTS 报警
```

#### 闭环三：语音注册

```text
用户说“记住我，我叫小明”
  → 进入 ENROLLING
  → 提示转身，5 秒采集 25 帧
  → 每帧 OSNet 推理 + 视角估计
  → 按 front/side/back 分桶，每桶保留最佳 2 帧
  → 写入 SPIFFS Gallery
  → TTS：“已经记住你了，小明。”
```

---

## 7. Gallery 设计

二进制文件 `reid_gallery_v2.bin`：

```text
文件头（32 bytes）
  - magic: "REID"
  - version: 2
  - num_identities: 3
  - templates_per_identity: 12
  - feature_dim: 128
  - quantization_type: INT8

每条模板（140 bytes @ INT8）
  - identity_id: uint8
  - name: char[16]
  - view: uint8 (0=front, 1=side, 2=back, 3=unknown)
  - feature[128]: int8（已 L2 归一化）
  - enrolled_at: int64
  - hit_count: uint32
  - confidence: uint8
  - reserved: 3 bytes

总容量：3 × 12 × 140 + 32 ≈ 5KB
```

---

## 8. 状态机与 UI

沿用 v3/v4 设计：

```text
IDLE → IDENTIFYING → FOCUS
              ↓
           PAUSE ←→ MONITORING
              ↓
          ENROLLING → ALERT
```

UI 字段扩展：

```c
typedef struct {
    // ... 已有字段 ...
    const char *identity_name;     // "小明" / "陌生人" / "未知"
    bool        identity_matched;
    uint32_t    identity_study_seconds;
} ui_model_t;
```

---

## 9. 剩余工作量评估

> 单开发者、每天 8 小时估算。

| 阶段 | 任务 | 人天 |
|------|------|------|
| 环境准备 | 烧录端口修复、豆包 TTS 验证、Colab/训练环境、PRCC 下载 | 2~3 |
| 数据改造 | 上半身裁剪脚本、数据增强、训练/验证划分 | 1 |
| 模型微调 | OSNet-IBN-x0.25 分层冻结、Triplet + 换装一致性、训练监控 | 3~4 |
| 验证导出 | 指标测试、ONNX 导出、一致性验证 | 1 |
| 量化压缩 | PTQ/QAT、TFLite 转换、精度验证 | 2~3 |
| 边缘部署 | TFLite Micro 集成、ROI 预处理、内存优化、推理测试 | 4~5 |
| 系统集成 | Gallery 管理、注册/识别/监控闭环、UI/语音 | 3 |
| 测试调优 | 阈值调优、跨衣服测试、稳定性测试、文档 | 3~4 |
| **合计** | — | **19~24 天** |

> 若 2 人并行：可压缩至 **12~15 天**。  
> 若 3 人并行：可压缩至 **9~12 天**。

---

## 10. 三人并行分工建议

| 角色 | 核心职责 | 人天 |
|------|----------|------|
| **A：云端训练** | PRCC 数据改造、OSNet 微调、Triplet 损失、换装一致性、ONNX 导出、PTQ/QAT | 10~12 |
| **B：边缘部署** | TFLite Micro 集成、ROI 预处理、dl::Model/TFLite 加载、推理接口、内存优化 | 8~10 |
| **C：应用集成与验证** | Gallery 设计、注册/识别/监控闭环、UI/语音、测试数据集、稳定性测试、文档 | 8~10 |

### 10.1 接口契约

```c
// components/reid_dl/include/reid_dl.h
#pragma once
#include <stdint.h>
#include "esp_err.h"

#define REID_FEATURE_DIM 128
#define REID_MAX_NAME_LEN 16

typedef enum {
    REID_VIEW_FRONT = 0,
    REID_VIEW_SIDE,
    REID_VIEW_BACK,
    REID_VIEW_UNKNOWN,
} reid_view_t;

typedef enum {
    REID_RESULT_UNKNOWN = 0,
    REID_RESULT_SELF,
    REID_RESULT_STRANGER,
} reid_result_t;

typedef struct {
    uint16_t x, y, w, h;
} reid_bbox_t;

esp_err_t reid_dl_init(const char *model_path);
esp_err_t reid_dl_enroll_start(const char *name);
esp_err_t reid_dl_enroll_feed(const uint8_t *rgb565, int w, int h,
                              const reid_bbox_t *bbox);
esp_err_t reid_dl_enroll_finish(uint8_t *out_id);
esp_err_t reid_dl_identify(const uint8_t *rgb565, int w, int h,
                           const reid_bbox_t *bbox,
                           reid_result_t *out_result,
                           uint8_t *out_id, float *out_score,
                           reid_view_t *out_view);
esp_err_t reid_dl_gallery_load(void);
esp_err_t reid_dl_gallery_save(void);
```

### 10.2 时间线

```text
Day 1-2: 三人接口对齐；C 修烧录/TTS；A 准备数据；B 验证 TFLite Micro 编译
Day 3-5: A 数据改造 + 开始训练；B 跑通任意 .tflite 加载；C UI/状态机扩展
Day 6-9: A 微调 + Triplet 调参；B ROI 预处理 + 推理封装；C Gallery 设计
Day 10-12: A 量化导出；B 集成识别接口；C 注册/监控闭环
Day 13-15: A/B 模型精度联合调试；C 采集测试数据
Day 16-18: 三人联调闭环演示
Day 19-21: 阈值调优 + 稳定性测试 + 文档
```

---

## 11. 风险与应对

| 风险 | 影响 | 应对 |
|------|------|------|
| **OSNet 推理 > 300ms** | 状态切换延迟明显 | ① 降输入至 96×48；② 模型压缩至 x0.125；③ 仅事件触发 |
| **INT8 量化后精度崩塌** | 跨衣服识别失效 | ① QAT 重新训练；② 回退 FP16；③ 增加校准数据 |
| **PRCC 数据与真实场景域差异大** | 实际识别率低 | ① 用设备采集真实数据做域适应微调；② 注册时多衣服采集 |
| **侧/背面坐姿识别差** | 频繁 UNKNOWN | ① 注册时强制转头；② 侧/背面阈值单独放宽；③ 增加模板数 |
| **多人同时在场** | human_detect 只给单个 bbox | ① 取最大 ROI；② 产品限定单人场景；③ 扩展多 bbox 处理 |
| **Gallery SPIFFS 损坏** | 注册丢失 | ① 双备份 + CRC；② 启动时自动恢复 |
| **烧录端口持续异常** | 无法迭代 | 换线/换口/ESP-Prog；A/B 先用 PC 验证 |

---

## 12. 验收标准

| 阶段 | 验收项 | 通过标准 |
|------|--------|----------|
| 云端训练 | PRCC 上半身微调跨衣服 Rank-1 | > 50% |
| 量化验证 | INT8 vs FP32 特征余弦相似度 | > 0.98 |
| 边缘部署 | ESP32-P4 单帧推理耗时 | < 300ms |
| 注册闭环 | 1 人 3 视角注册，重启后加载 | 100% 成功 |
| 入座闭环 | 同衣服离开 5 分钟返回识别率 | > 80% |
| 跨衣服闭环 | 换一件衣服返回识别率 | > 50% |
| 监控闭环 | 主人走过不报警，陌生人走过报警 | > 80% |
| 稳定性 | 连续运行 24 小时 | 无崩溃，无内存泄漏 |

---

## 13. 结论

本方案以 **OSNet-IBN-x0.25** 为核心，走通了“云端预训练 → 上半身微调 → INT8 量化 → TFLite Micro 边缘部署 → 与现有伴学/监控功能缝合”的完整链路。

- **比 v3 手工特征强**：真正运行神经网络，换装鲁棒性来自 IBN + Triplet，而非手工规则。
- **比 v4 论文蒸馏方案务实**：不需要复现 M2Net/CAG/MVMA，直接用成熟的公开模型，周期更短、可控性更高。
- **与现有项目无缝缝合**：复用 human_detect、PIR、VLM、TTS、UI、状态机，只把 Re-ID 内核升级。

**建议下一步**：
1. 先确认 OSNet-IBN-x0.25 预训练权重可获取（torchreid / torchreid-models）；
2. 在 Colab/PC 上 1 天内跑通 PRCC 上半身微调的 baseline；
3. 同时 C 修复烧录端口，B 验证 TFLite Micro 在 ESP-IDF v5.5.2 中的编译与加载；
4. baseline 精度达标后，进入量化和边缘部署。
