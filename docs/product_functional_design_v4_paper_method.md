# ESP32-P4 AIoT 伴学终端 — 真正落地论文方法的 Re-ID 设计（v4）

> 版本：v4.0  
> 日期：2026-07-04  
> 目标硬件：ESP32-P4-Function-EV-Board  
> 目标平台：ESP-IDF v5.5.2 + ESP-DL  
> 核心转变：**不再只是“概念借鉴”，而是通过知识蒸馏把论文的 M2Net/CAG/MVMA 能力压缩到端侧可运行的轻量 CNN**

---

## 1. 对之前方案的坦诚复盘

`docs/product_functional_design_v3.md` 中提出的“颜色+轮廓+体型”手工特征方案：

- 与论文的 M2Net 三模态 CNN **名字像，实现完全不同**；
- 没有训练过程，没有损失函数，没有 DenseNet121 Backbone；
- “视角分桶”只是手工宽高比规则，不是 MVMA 的视角预测网络；
- 换装后基本会失效，无法解决论文要解决的“跨外观”问题。

**v4 的目标**：在 ESP32-P4 上**真正运行一个从论文方法蒸馏出来的神经网络**，让它学到论文模型“忽略衣服、关注体型与结构”的能力。

---

## 2. 核心思路：教师-学生蒸馏体系

论文方法太重（DenseNet121 + RCF + PSPNet + GAN + 双分支），无法直接在 P4 运行。解决方案是**知识蒸馏**：

```text
云端/PC 端（教师模型，论文方法）
    M2Net(DenseNet121) + CAG + MVMA
    输入：RGB + 轮廓草图(RCF) + 语义分割(PSPNet)
    输出：身份预测 + 外观预测 + 视角预测 + 融合特征
            │
            │  蒸馏：让学生网络模仿教师的身份判断与特征
            ▼
边缘端（学生模型，ESP32-P4 运行）
    Micro-ReID + Micro-ViewNet
    输入：RGB565 行人 ROI（仅 RGB）
    输出：128 维身份特征 + front/side/back 视角概率
```

这样：
- 边缘端**只跑一个轻量 CNN**，不需要 RCF/PSPNet/GAN；
- 学生虽然只看 RGB，但蒸馏迫使它学习教师认为重要的“身份线索”，间接实现外观解耦；
- CAG 生成的换装数据用于增强教师软标签，从而提升学生换装鲁棒性；
- MVMA 的视角感知通过蒸馏传递到 Micro-ViewNet，用于视角分桶匹配。

---

## 3. 论文方法 → 端侧实现映射表

| 论文方法 | 论文核心 | 端侧实现（v4） | 是否真正使用 |
|----------|----------|----------------|--------------|
| **NKUP+ 数据集** | 大规模跨外观、跨季节、多视角数据 | 用于教师模型训练；边缘端不训练 | ✅ 数据驱动 |
| **M2Net 多模态融合** | RGB + RCF 轮廓 + PSPNet 语义分割，三分支 CNN + GeM 池化 | 教师模型完整运行；学生通过蒸馏继承三模态融合能力，边缘端只跑 RGB | ✅ 能力蒸馏 |
| **RAS 随机外观采样** | 训练时强制同身份不同外观同批次 | 教师训练使用；蒸馏时同样按外观分层采样 | ✅ 训练策略 |
| **CA 跨外观损失** | 同身份不同外观距离 < 不同身份相似外观距离 | 教师损失函数；学生蒸馏损失中保留该约束 | ✅ 损失蒸馏 |
| **CAG 生成增强** | GAN 解耦外观/身份，生成换装图像 | 云端生成合成数据，加入蒸馏训练；边缘端不运行 GAN | ✅ 数据增强 |
| **DenseNet121 Backbone** | 教师骨干网络 | 教师使用；学生使用 Depthwise-Separable 微型网络 | ✅ 教师侧 |
| **MVMA 多视角网络** | 视角预测器 + 全局/局部双分支 + 跨视角损失 + 视角正则化 | 教师完整训练；学生训练 Micro-ViewNet 蒸馏视角预测；边缘 Gallery 按视角桶匹配 | ✅ 能力蒸馏 |
| **度量学习 / 重排序** | 困难样本挖掘、重排序后处理 | 边缘端用余弦距离 + 视角桶 + Top-K，不做重排序 | ⚠️ 简化 |

---

## 4. 边缘端网络设计

### 4.1 Micro-ReID（身份特征网络）

目标：参数量 **< 200 KB**，单次推理 **< 200 ms**，输出 **128 维**身份特征。

```text
输入：128×64×3 RGB（从 1024×600 裁剪出行人 ROI 并缩放）

Block 1 下采样
  Conv3x3(3 → 16, stride=2)      # 64×32×16
  DepthwiseConv3x3(16 → 32)      # 64×32×32
  MaxPool2x2                     # 32×16×32

Block 2 多尺度特征
  DepthwiseConv3x3(32 → 64)      # 32×16×64
  DepthwiseConv5x5(64 → 64, dilation=2)  # 扩大感受野
  AdaptiveAvgPool2d → 1×1×64

特征投影（模拟论文身份编码器 Ei）
  FC(64 → 128)                   # 身份特征 fi
  L2 Normalize                   # 单位长度，便于余弦距离

参数量估算：~150 KB（FP32）/ ~75 KB（INT8）
推理耗时估算：~120~180 ms（ESP32-P4 纯 CPU，双核 + INT8）
```

### 4.2 Micro-ViewNet（视角估计网络）

目标：输入 Micro-ReID 的中间特征，输出 front/side/back 三分类概率。

```text
输入：Block 2 输出 32×16×64
  GlobalAvgPool → 64
  FC(64 → 3) + Softmax

参数量估算：< 50 KB（INT8）
推理耗时估算：~10 ms
```

### 4.3 为什么这样能“继承”论文能力？

- 教师用 **三模态 + 跨外观损失** 学到了“换衣服也不变”的身份特征；
- 蒸馏时，学生只看到 RGB，但损失函数要求学生输出的身份预测/特征与教师一致；
- 为了模仿教师，学生必须**从 RGB 中提取与教师相同的身份线索**，也就是忽略衣服、关注体型/姿态/轮廓结构；
- 因此，学生虽然没有显式输入轮廓和语义分割，但行为上被训练成了“单 RGB 的 M2Net 蒸馏版”。

---

## 5. 云端/PC 端蒸馏训练流程

### 5.1 教师模型准备

首选：论文作者开源的 M2Net + CAG + MVMA 预训练权重。  
若不可获取，可用公开跨外观 Re-ID 模型替代：OSNet、TransReID、PCB 在 PRCC / DeepChange 上训练。

### 5.2 训练数据

| 数据来源 | 作用 |
|----------|------|
| NKUP+ / PRCC / DeepChange | 教师预训练与蒸馏的真实数据 |
| CAG 生成图像 | 增加“同身份、多外观”样本，强化跨外观能力 |
| 设备端注册 captures（可选） | 后期增量蒸馏，让模型适配实际场景 |

### 5.3 蒸馏损失

```python
# 教师（冻结）
feat_t, logits_t_id, logits_t_app, view_t = teacher(rgb, sketch, parsing)

# 学生（可训练）
feat_s, logits_s_id, view_s = student(rgb)

# 损失
loss = α * CE(logits_s_id, labels)            # 硬标签身份分类
     + β * KL(logits_s_id, logits_t_id)        # 教师身份预测软标签
     + γ * MSE(feat_s, proj(feat_t))           # 特征对齐（教师投影到 128 维）
     + δ * CE(view_s, view_labels)             # 视角自监督
     + ε * MSE(view_s, view_t)                 # 视角蒸馏
```

关键：通过 `logits_t_app`（教师外观预测）反向约束，确保学生不会过度拟合衣服颜色。

### 5.4 量化与导出

```text
PyTorch FP32 Micro-ReID / Micro-ViewNet
        │
        ▼
ESP-PPQ 量化（INT8，per-channel，目标平台 ESP32-P4）
        │
        ▼
导出 .espdl 格式
        │
        ▼
pack_espdl_models.py 打包为 models.espdl
        │
        ▼
烧录到 Flash 分区或 SPIFFS
```

---

## 6. 边缘端部署与运行

### 6.1 模型加载

参考 `human_detect` 的 `dl::Model` 用法：

```cpp
// components/reid_dl/reid_dl.cpp
#include "dl_model.hpp"

static dl::Model *s_reid_model = nullptr;
static dl::Model *s_view_model = nullptr;

esp_err_t reid_dl_init(void)
{
    s_reid_model = new dl::Model(
        "/spiffs/reid_micro.espdl",
        "micro_reid",
        fbs::MODEL_LOCATION_IN_FLASH);
    s_reid_model->minimize();

    s_view_model = new dl::Model(
        "/spiffs/view_micro.espdl",
        "micro_viewnet",
        fbs::MODEL_LOCATION_IN_FLASH);
    s_view_model->minimize();
    return ESP_OK;
}
```

模型可放在：
- Flash 分区（大容量，适合量产）
- SPIFFS（开发阶段灵活替换）
- SD 卡（若硬件支持）

### 6.2 推理流程

```text
RGB565 整帧（1024×600）
        │
        ▼
人体检测 PedestrianDetect → 行人 bbox
        │
        ▼
裁剪 ROI → 缩放至 128×64 → RGB888 归一化
        │
        ▼
Micro-ReID 推理 → 128 维特征向量
        │
        ▼
Micro-ViewNet 推理 → front/side/back 概率
        │
        ▼
与 Gallery 同/近视角模板比余弦距离
        │
        ▼
输出：identity_id + confidence + view
```

### 6.3 Gallery 设计

- 特征维度从 97（手工）升级为 **128**（神经网络）。
- 每条模板：128×4 + 元数据 ≈ 550 字节。
- 3 人 × 6 模板 ≈ 10 KB，完全可接受。
- 视角桶匹配策略同 v3，但视角来自 Micro-ViewNet 概率而非手工规则。

---

## 7. 与现有项目的集成

### 7.1 替换手工 Re-ID 模块

- 原 `components/reid_lite/` 改为 `components/reid_dl/`。
- 对外 API 保持与 v3 一致（`reid_init / reid_enroll_start / reid_enroll_feed / reid_enroll_finish / reid_identify`），内部实现改为神经网络推理。
- 若模型未加载成功，可 fallback 到手工特征（保留 v3 代码作为降级）。

### 7.2 应用层几乎不变

v3 中设计的业务闭环仍然适用：

1. **入座识别闭环**：`on_human_present()` → `reid_identify()` → 欢迎语 + 个人计时。
2. **陌生人监控闭环**：`monitor_start()` 记录白名单；PIR 触发先 Re-ID，陌生人再走 VLM 确认。
3. **语音注册闭环**："记住我，我叫 XXX" → 多角度抓拍 → 提取 128 维特征 → Gallery 持久化。

唯一变化是 `reid_identify()` 内部运行的是 CNN，准确率和换装鲁棒性显著提升。

### 7.3 UI 与状态机

沿用 v3 扩展：

```c
typedef struct {
    // ... 已有字段 ...
    const char *identity_name;
    bool        identity_matched;
    uint32_t    identity_study_seconds;
} ui_model_t;
```

状态机同样沿用：`IDLE / IDENTIFYING / FOCUS / PAUSE / MONITORING / ENROLLING / ALERT`。

---

## 8. 产品闭环（v4 版）

### 8.1 闭环一：入座识别 + 专注陪伴

```text
学生入座
  → human_detect 确认在座
  → reid_dl_identify() 运行 Micro-ReID
  → 匹配注册身份（小明）
  → TTS：“欢迎回来，小明。”
  → UI 显示“小明 在座”
  → 启动/恢复小明专注计时
```

### 8.2 闭环二：陌生人监控

```text
用户说“开始监测”
  → monitor_start()：保存参考帧 + 当前身份白名单
  → PIR 触发
  → reid_dl_identify() 识别当前人物
  → 白名单身份 → 忽略
  → 陌生人 → VLM 对比参考帧/当前帧 → TTS 报警
  → 用户说“结束监测” → 停止
```

### 8.3 闭环三：语音注册

```text
用户说“记住我，我叫小明”
  → 进入 ENROLLING 状态
  → 提示转身
  → 连续抓拍 5~8 帧
  → 每帧运行 Micro-ReID + Micro-ViewNet
  → 按 front/side/back 分桶，每桶保留 1~2 个 128 维模板
  → 写入 SPIFFS Gallery
  → TTS：“我记住你了，小明。”
```

---

## 9. 工作量评估（v4 真正落地论文方法）

> 单开发者、每天 8 小时、假设论文代码/权重可获取或可用公开模型替代。

| 阶段 | 任务 | 人天 |
|------|------|------|
| **Phase 0：环境准备** | 解决烧录端口；验证豆包 TTS；确认 ESP-DL 在 P4 上能跑自定义模型 | 2~3 |
| **Phase 1：教师模型准备** | 获取/复现 M2Net 或替代模型；在 NKUP+/PRCC 上验证精度；准备 CAG 数据 | 4~6 |
| **Phase 2：学生网络设计** | 设计 Micro-ReID / Micro-ViewNet；在 PC 上训练baseline | 3~4 |
| **Phase 3：蒸馏训练** | 实现蒸馏损失；调参 α/β/γ/δ/ε；量化前精度验证 | 5~7 |
| **Phase 4：量化与导出** | ESP-PPQ INT8 量化；导出 .espdl；验证量化损失 | 2~3 |
| **Phase 5：边缘部署** | 创建 `components/reid_dl/`；ROI 预处理；dl::Model 加载；推理接口 | 3~4 |
| **Phase 6：业务集成** | 状态机、UI、语音、监控白名单接入 | 3~4 |
| **Phase 7：测试调优** | 跨外观测试、多人注册、24h 稳定性、阈值调优、文档 | 4~5 |
| **合计** | — | **26~36 天** |

> 若教师模型/代码不可获取，改用公开 OSNet 做 MVP，可压缩到 **16~22 天**，但跨外观能力会弱于论文蒸馏方案。

---

## 10. 三人并行分工（v4 方案）

由于 v4 涉及云端训练 + 边缘部署 + 应用集成，三人并行更高效。

| 角色 | 负责人 | 核心职责 | 人天 |
|------|--------|----------|------|
| **A：云端训练** | 1 人 | 教师模型、学生网络、蒸馏训练、量化导出 | 20~25 |
| **B：边缘部署** | 1 人 | ESP-DL 模型加载、Micro-ReID/ViewNet 推理引擎、ROI 预处理、Gallery | 12~15 |
| **C：应用集成与验证** | 1 人 | 状态机、UI、语音、监控闭环、测试数据集、稳定性测试、文档 | 12~15 |

### 10.1 并行时间线

```text
Week 1
  Day 1-2: 三人接口对齐（reid_dl.h、模型输入输出、Gallery 格式、状态机）
  Day 3-5: A 准备教师模型；B 跑通 ESP-DL 加载任意 .espdl；C 修烧录/TTS/PIR

Week 2
  Day 6-9: A 设计 Micro-ReID/Micro-ViewNet；B 实现 ROI 预处理 + dl::Model 封装
  Day 10-12: A 开始蒸馏训练；C 完成 UI/状态机扩展（接 B 的桩函数）

Week 3
  Day 13-16: A 蒸馏调参 + 量化；B 集成识别接口 + Gallery 持久化
  Day 17-19: C 接入监控白名单、语音注册、陌生人报警

Week 4
  Day 20-23: A/B 联合调试模型精度；C 采集跨外观测试数据
  Day 24-26: 三人联调闭环演示
  Day 27-28: 稳定性测试 + 文档收尾
```

### 10.2 模块间接口契约

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

esp_err_t reid_dl_init(const char *reid_model_path,
                       const char *view_model_path);
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

### 10.3 依赖与解耦

| 依赖 | 解耦方法 |
|------|----------|
| B 需要 A 的模型 | A 先用随机初始化或公开预训练模型导出一份 `.espdl` 给 B 调加载流程 |
| C 需要 B 的推理接口 | B 提供桩函数：固定返回示例身份，C 先调通业务逻辑 |
| A/B/C 都需要真实硬件 | C 负责环境修复；A/B 前期可用 PC 模拟或开发板借调 |

---

## 11. 风险与应对（v4）

| 风险 | 影响 | 应对 |
|------|------|------|
| **论文代码/权重未开源** | 无法复现教师模型 | 用公开 OSNet / TransReID / PCB 替代教师，在 PRCC 上训练 |
| **ESP32-P4 纯 CPU 推理太慢** | > 500 ms，体验差 | ① 输入降到 96×48；② INT4 量化；③ 仅在人状态变化时推理；④ fallback 手工特征 |
| **量化后精度崩塌** | 识别率骤降 | QAT 量化感知训练；混合精度保留第一层/最后一层 FP16；回退 FP32 |
| **内存不足** | 模型 + 特征图 + Gallery 超支 | 模型放 Flash/PSRAM；dl::Model minimize；限制 Gallery 模板数 |
| **蒸馏效果不佳** | 换装仍失效 | 增加 CAG 数据；注册时要求多衣服样本；允许 UNKNOWN 走 VLM 二次确认 |
| **环境阻塞** | 无法烧录验证 | 换线/换口/ESP-Prog；A/B 先用 QEMU/PC 模拟部分流程 |

---

## 12. 最小可行方案（MVP）建议

如果完整蒸馏链路风险太高，建议先做一个 **1 周内可验证的 MVP**：

1. 用公开轻量 Re-ID 模型 **OSNet-IBN-x0.25**（~200K 参数）作为教师/直接模型；
2. 在 Market-1501 或 PRCC 上训练/微调；
3. 导出 ONNX → ESP-PPQ 量化 → `.espdl`；
4. 在 P4 上验证：
   - 推理耗时是否 < 300 ms；
   - 同一人换外套识别率是否 > 60%；
5. 若 MVP 通过，再投入资源复现论文完整蒸馏链路。

---

## 13. 结论

v4 不再停留在“手工特征 + 论文概念”，而是设计了一条**真正让论文方法在 ESP32-P4 上生效**的路径：

- **云端**：完整运行 M2Net + CAG + MVMA，学习跨外观、跨视角身份表示；
- **蒸馏**：把教师能力压缩到 Micro-ReID / Micro-ViewNet；
- **边缘**：通过 ESP-DL + ESP-PPQ 运行 INT8 量化模型，实现端侧身份识别；
- **应用**：复用 v3 的入座/监控/注册三大闭环，只把识别内核升级为神经网络。

工作量比手工方案大（单开发者 26~36 天），但这是**真正解决“换衣服后还能认出是谁”**的技术路径，也是项目从“概念 Demo”走向“技术深度”的关键。

**下一步建议**：
1. 确认论文代码/权重是否可获取；
2. 用 ESP-PPQ 把一个公开小模型（如 MobileNetV2）量化并跑通 ESP-DL 加载，验证 P4 推理能力；
3. 若前两点可行，按三人分工启动 v4 开发。
