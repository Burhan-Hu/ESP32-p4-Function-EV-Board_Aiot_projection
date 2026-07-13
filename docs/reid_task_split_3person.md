# Re-ID 集成三人并行开发分工方案

> 基于 `docs/product_functional_design_v3.md` 设计，目标：**3 人并行，10~12 天完成开发与联调**。  
> 日期：2026-07-04

---

## 1. 总体原则

1. **先定接口，后并行编码**：Day 1 三人一起确认 `reid_lite` 的 C API、UI 数据结构、状态机扩展方案。
2. **每人一条独立工作流**：尽量减少阻塞；无法避免依赖的地方通过 mock/桩函数先行开发。
3. **每日 15 分钟站会**：同步接口变更、阻塞、第二天计划。
4. **统一分支**：
   - `main`：稳定基线
   - `feature/reid-core`：人员 A
   - `feature/reid-app`：人员 B
   - `feature/reid-platform`：人员 C
   - 每天傍晚视情况合并到 `develop/reid`。

---

## 2. 三人分工一览

| 角色 | 负责人 | 核心职责 | 关键产出 |
|------|--------|----------|----------|
| **A：算法模块** | 1 人 | Re-ID 特征提取、Gallery 管理、注册/识别算法 | `components/reid_lite/` 可独立编译运行 |
| **B：应用集成** | 1 人 | 状态机、UI 扩展、语音命令、在座/监控回调对接 | `main/mipi_isp_dsi_main.c` 与 UI 改动跑通 |
| **C：平台验证** | 1 人 | 烧录环境、TTS/音频验证、云端 VLM、测试数据、文档 | 硬件可烧录、TTS 出声、VLM 对比可用 |

---

## 3. 详细任务拆分

### 3.1 人员 A：Re-ID Lite 算法模块

#### 负责范围

- `components/reid_lite/` 新组件：Kconfig、CMakeLists、头文件、实现、单元测试。
- 行人 ROI 裁剪与预处理。
- 颜色、轮廓、体型、视角四类特征提取。
- Gallery 加载/保存/更新/删除（SPIFFS）。
- 注册流程底层：`enroll_start / enroll_feed / enroll_finish`。
- 识别流程底层：`identify`。

#### 任务清单

| # | 任务 | 人天 | 依赖 | 可交付检查点 |
|---|------|------|------|--------------|
| A1 | 创建 `components/reid_lite/` 目录结构、Kconfig、CMakeLists | 0.5 | — | 组件可被 menuconfig 识别 |
| A2 | 定义公共头文件 `reid_lite.h`（API、结构体、枚举） | 0.5 | B1 接口对齐 | 头文件通过三人评审 |
| A3 | RGB565 行人 ROI 裁剪 + 缩放（如 128×64） | 1 | — | 输出 ROI 像素正确 |
| A4 | 颜色特征：HSV 直方图（上身/下身分块） | 1 | A3 | 特征值稳定 |
| A5 | 轮廓特征：边缘图 + 水平/垂直投影 | 1.5 | A3 | 换衣服前后变化小 |
| A6 | 体型特征：头肩/躯干比例、重心 | 1 | A3 | 同一人不同角度稳定 |
| A7 | 视角估计：front/side/back | 0.5 | A6 | 角度分类可视化正确 |
| A8 | 特征归一化与余弦距离匹配 | 0.5 | A4~A7 | 同一人相似度高 |
| A9 | Gallery SPIFFS 持久化（JSON 或二进制） | 1.5 | — | 保存/加载/删除正确 |
| A10 | 注册接口：`reid_enroll_start/feed/finish` | 1 | A3~A9 | 1 人注册成功 |
| A11 | 识别接口：`reid_identify` + 阈值策略 | 1 | A8,A9 | 返回 self/stranger/unknown |
| A12 | 模板在线更新（命中计数加权平均） | 0.5 | A11 | 高置信度命中后模板微调 |
| A13 | 本地单元测试（用 3 组本地图片） | 1 | A10,A11 | 准确率 > 80% |

**小计：11 天**

#### 里程碑

- **Day 3**：头文件 + ROI + 颜色/轮廓特征原型可跑。
- **Day 6**：Gallery 保存/加载 + 注册/识别接口完成。
- **Day 9**：本地 3 人测试数据准确率 > 80%。
- **Day 11**：算法模块稳定，交付 B/C 集成。

---

### 3.2 人员 B：应用集成与交互

#### 负责范围

- 扩展 `ui_model_t` 与 UI 显示。
- 主状态机改造：`IDLE / IDENTIFYING / FOCUS / PAUSE / MONITORING / ENROLLING`。
- 改写 `on_human_present()`、`on_human_left()`、`on_human_left_reminder()`。
- 改写 `monitor_start()`、`on_pir_motion_detected()`，加入白名单。
- 语音命令：新增“记住我，我叫 XXX”。
- TTS 播报个性化：欢迎、陌生人报警、离座提醒带名字。

#### 任务清单

| # | 任务 | 人天 | 依赖 | 可交付检查点 |
|---|------|------|------|--------------|
| B1 | 与 A 对齐 `reid_lite.h` 接口；定义 UI 数据扩展字段 | 0.5 | — | 接口文档落地 |
| B2 | 扩展 `ui_model_t`：identity_name、identity_matched、per_identity_study_seconds | 0.5 | B1 | 编译通过 |
| B3 | 在 `ui_lvgl.c` 中显示“XXX 在座 / 陌生人 / 未知” | 1 | B2 | 屏幕显示正确 |
| B4 | 主状态机新增 IDENTIFYING、ENROLLING、ALERT 状态 | 1 | — | 状态切换无死锁 |
| B5 | 改写 `on_human_present()`：调用 `reid_identify()`，更新 UI/计时 | 1 | A11 | 识别到本人时恢复计时 |
| B6 | 改写 `on_human_left()`：暂停当前身份计时 | 0.5 | B5 | 离座时计时停止 |
| B7 | 改写 `on_human_left_reminder()`：TTS 播报带名字 | 0.5 | B6 | 语音“小明，你已经离开一分钟了” |
| B8 | 改写 `monitor_start()`：保存参考帧 + 当前身份白名单 | 0.5 | A11 | 启动监控时记录白名单 |
| B9 | 改写 `on_pir_motion_detected()`：先 Re-ID，白名单忽略，陌生人再走 VLM | 1.5 | A11,B8 | 主人移动不误报 |
| B10 | 语音命令解析：新增“记住我/我叫 XXX”进入 ENROLLING | 0.5 | A10 | ASR 文本命中后进入注册 |
| B11 | 注册状态机流程：提示转身、调用 `reid_enroll_feed`、结束播报 | 1 | A10,B10 | 完整注册走通 |
| B12 | 集成联调与 bug 修复 | 2 | A13 | 三大闭环可演示 |

**小计：10.5 天**

#### 里程碑

- **Day 3**：UI 扩展 + 状态机扩展完成。
- **Day 6**：在座/离座回调接入 Re-ID（可用 A 的桩函数）。
- **Day 9**：监控白名单 + 语音注册命令完成。
- **Day 11~12**：与 A/C 联调，闭环跑通。

---

### 3.3 人员 C：平台验证与测试

#### 负责范围

- 解决烧录端口问题（COM9 超时 / COM8 busy）。
- 验证并稳定豆包 TTS 出声、C6 SDIO 联网。
- 验证火山 VLM 双图对比接口。
- 建立测试数据集与测试脚本。
- 负责最终稳定性测试、阈值调优、文档整理。

#### 任务清单

| # | 任务 | 人天 | 依赖 | 可交付检查点 |
|---|------|------|------|--------------|
| C1 | 排查烧录端口：换线、换口、驱动、ESP-Prog | 1 | — | `idf.py flash` 稳定成功 |
| C2 | 验证豆包 TTS 出声与音量 | 0.5 | C1 | 喇叭正常播报 |
| C3 | 验证 C6 SDIO 联网与 Wi-Fi 连接 | 0.5 | C1 | `http_test_request` 成功 |
| C4 | 验证火山 VLM 双图对比（monitor 链路） | 1 | C3 | PIR 触发可返回变化描述 |
| C5 | 配置 SPIFFS 分区，确保有足够空间存 Gallery | 0.5 | — | 分区表确认 ≥ 64 KB |
| C6 | 编写测试脚本：抓拍多视角图片到 SD/串口导出 | 1 | C1 | 可采集测试素材 |
| C7 | 建立 3 人 × 3 视角测试数据集 | 1 | C6 | ≥ 27 张标注图片 |
| C8 | 协助 A 进行离线精度测试与阈值调优 | 1.5 | A13,C7 | 给出推荐阈值 |
| C9 | 24 小时稳定性测试：入座/离座/返回循环 | 1 | B12 | 无崩溃、无内存泄漏 |
| C10 | 编写 README、演示脚本、验收清单 | 1 | B12 | 他人可按文档复现 |

**小计：9 天**

#### 里程碑

- **Day 2**：烧录/联网/TTS 全部正常。
- **Day 5**：测试数据集与 SPIFFS 分区就绪。
- **Day 8**：阈值调优完成。
- **Day 11~12**：稳定性测试 + 文档完成。

---

## 4. 10~12 天并行时间线

```text
Day  1: 三人接口对齐会；A 开始目录/头文件；B 开始 UI/状态机扩展；C 修烧录端口
Day  2: A ROI+颜色；B ui_model 扩展；C TTS/联网验证
Day  3: A 轮廓；B 在座回调（接 A 桩函数）；C VLM 双图验证
Day  4: A 体型+视角；B 状态机/离座提醒；C SPIFFS 分区+测试脚本
Day  5: A 特征匹配；B 监控白名单逻辑；C 采集数据集
Day  6: A Gallery 持久化；B 语音“记住我”命令；C 数据集标注
Day  7: A 注册接口；B 注册状态机流程；C 协助 A 跑离线测试
Day  8: A 识别接口+阈值；B 陌生人报警 TTS；C 阈值初调
Day  9: A 模板更新+单元测试；B 全链路串联；C 阈值精调
Day 10: A/B 第一次集成联调；C 准备稳定性测试
Day 11: 三人联调 bug 修复；闭环演示
Day 12: 24h 稳定性测试 + 文档收尾
```

---

## 5. 模块间接口契约

### 5.1 Re-ID 模块公共头文件（A 负责，B/C 使用）

```c
// components/reid_lite/include/reid_lite.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define REID_FEATURE_DIM   97
#define REID_MAX_NAME_LEN  16

typedef enum {
    REID_VIEW_FRONT = 0,
    REID_VIEW_SIDE,
    REID_VIEW_BACK,
} reid_view_t;

typedef enum {
    REID_RESULT_UNKNOWN = 0,
    REID_RESULT_SELF,
    REID_RESULT_STRANGER,
} reid_result_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} reid_bbox_t;

typedef struct {
    uint8_t        identity_id;
    char           name[REID_MAX_NAME_LEN];
    reid_view_t    view;
    float          feature[REID_FEATURE_DIM];
    int64_t        enrolled_at;
    uint32_t       hit_count;
} reid_template_t;

/* 初始化与反初始化 */
esp_err_t reid_init(void);
void      reid_deinit(void);

/* 注册流程：开始 -> 逐帧喂图 -> 结束 */
esp_err_t reid_enroll_start(const char *name);
esp_err_t reid_enroll_feed(const uint8_t *rgb565, int width, int height,
                           const reid_bbox_t *bbox);
esp_err_t reid_enroll_finish(uint8_t *out_id);

/* 识别流程 */
esp_err_t reid_identify(const uint8_t *rgb565, int width, int height,
                        const reid_bbox_t *bbox,
                        reid_result_t *out_result,
                        uint8_t *out_id,
                        float *out_score);

/* Gallery 管理 */
esp_err_t reid_gallery_load(void);
esp_err_t reid_gallery_save(void);
int       reid_gallery_count(void);
const reid_template_t *reid_gallery_get(int idx);

/* 查询身份信息 */
esp_err_t reid_get_identity_name(uint8_t id, char *buf, size_t buf_len);
```

### 5.2 UI 数据扩展（B 负责）

```c
// components/ui/include/ui_manager.h

typedef struct {
    // ... 已有字段 ...
    const char *identity_name;     // "小明" / "陌生人" / "未知"
    bool        identity_matched;  // true = 注册身份
    uint32_t    identity_study_seconds; // 当前身份本次专注秒数
} ui_model_t;
```

### 5.3 状态机事件（B 负责）

```c
typedef enum {
    APP_STATE_IDLE,
    APP_STATE_IDENTIFYING,
    APP_STATE_FOCUS,
    APP_STATE_PAUSE,
    APP_STATE_MONITORING,
    APP_STATE_ENROLLING,
    APP_STATE_ALERT,
} app_state_t;
```

---

## 6. 并行中的关键依赖与解耦策略

| 依赖关系 | 解耦方法 |
|----------|----------|
| B 需要 A 的识别结果 | Day 1 定好 `reid_lite.h`；A 先提供**桩函数**（固定返回 `REID_RESULT_SELF`），B 先调通流程 |
| C 需要 A/B 合并才能测闭环 | C 前期做平台验证 + 数据集；Day 8 再介入集成测试 |
| A 需要真实图片验证 | A 先用 PC 上保存的 RGB565 测试图；C 负责把真实设备图片导出给 A |
| 三人同时改 `sdkconfig` | 各自新增配置加 `REID_` / `APP_` 前缀，避免冲突；合并时统一 review |

---

## 7. 每日站会模板

每人回答：

1. 昨天完成了什么？
2. 今天计划做什么？
3. 阻塞/需要谁协助？
4. 是否改动了公共接口？

---

## 8. 风险与应对（并行视角）

| 风险 | 影响 | 应对 |
|------|------|------|
| A 的接口 Day 3 后还改 | B 返工 | Day 1 接口冻结；后续改动需三人评审 |
| C 烧录端口 2 天修不好 | 全组无法硬件验证 | 先用 QEMU/PC 模拟 + ESP-Prog 备用 |
| A 本地准确率始终 < 80% | 产品卖点不成立 | 缩小演示场景（同件衣服、固定角度），把换衣服列为 Phase 2 |
| B 改状态机引入死锁 | 设备卡住 | 每改完一个状态加日志；C 做压力测试 |
| 三人代码合并冲突多 | 延误 | 每天傍晚小合并；大功能分文件避免冲突 |

---

## 9. 结论

按本分工方案，3 人可在 **10~12 天** 内并行完成 Re-ID 集成：

- **A 主攻算法**：负责 `reid_lite` 组件，确保识别准确、存储稳定。
- **B 主攻应用**：负责状态机、UI、语音、在座/监控闭环融合。
- **C 主攻平台与验证**：负责烧录、TTS/网络、数据集、测试与文档。

**Day 1 必须完成接口对齐**，这是并行不返工的关键。建议会议输出：

1. 签字的 `reid_lite.h` 初版；
2. 状态机状态名与切换条件；
3. UI 字段扩展列表；
4. 三人分支命名与每日合并时间。
