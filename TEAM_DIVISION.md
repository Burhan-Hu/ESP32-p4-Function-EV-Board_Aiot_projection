# ESP32-P4 AIoT 项目团队分工方案

## 一、团队约束

- 团队共 **3 人**
- 仅有 **1 人（A）** 能经常接触开发板
- 另外 **2 人（B、C）** 主要负责不依赖硬件的算法、云端、逻辑开发
- 所有代码通过 Git 协作，由 A 定期集成到主工程并在板子上验证

---

## 二、角色与职责

### A：硬件负责人 / 集成测试负责人

**唯一接触开发板的人员。**

主要职责：
- 负责开发板上的所有驱动调试、固件烧录、现场测试
- 每周将 B、C 提交的模块代码集成到主工程
- 记录并反馈板子上的运行日志、截图、视频给 B、C
- 负责最终联调、演示准备、现场问题排查

**原则：** A 的时间应优先用于硬件相关工作和集成，尽量不承担纯业务逻辑开发。

---

### B：云端 AI 接口负责人

**不依赖开发板，全部工作可在 PC 上完成。**

主要职责：
- 负责 VLM（视觉问答）云端 API 对接
- 负责 ASR（语音识别）云端 API 对接
- 负责 TTS（语音合成）云端 API 对接
- 设计云端请求的错误处理、超时重试、JSON 解析、内存管理
- 先在 PC 上用 Python/curl 调通接口，再封装为 C 模块

---

### C：本地算法与状态机负责人

**大部分工作可在 PC 上完成，少数情况需要 A 在板子上验证。**

主要职责：
- 关键帧筛选算法（拉普拉斯清晰度 + 帧差）
- 系统状态机设计（IDLE / MONITORING / TRIGGERED / CONFIRMING / ACTION）
- PIR + LED 联动逻辑
- 事件日志 JSON 格式设计与存储
- 屏幕 UI / LVGL 布局设计

---

## 三、模块接口约定

三人必须按以下接口开发，A 集成时直接调用。

### 3.1 云端 AI 接口（B 提供）

文件：`components/cloud_api/cloud_api.h`

```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char *text;   /* 云端返回的文本，调用者需释放 */
    int len;
} cloud_response_t;

/* VLM 视觉问答：传入问题 + JPEG 图像，返回文本答案 */
esp_err_t cloud_vlm_ask(const char *question,
                        const uint8_t *jpeg_data,
                        size_t jpeg_len,
                        cloud_response_t *out);

/* ASR 语音识别：传入 PCM 音频数据，返回识别文本 */
esp_err_t cloud_asr_transcribe(const int16_t *pcm_data,
                               int pcm_samples,
                               cloud_response_t *out);

/* TTS 语音合成：传入文本，返回 PCM 音频数据 */
esp_err_t cloud_tts_speak(const char *text,
                          uint8_t **pcm_out,
                          size_t *pcm_len);

/* 释放 cloud_response_t 内部内存 */
void cloud_response_free(cloud_response_t *resp);
```

---

### 3.2 智能家居状态机接口（C 提供）

文件：`components/smart_home/smart_home.h`

```c
#pragma once
#include <stdbool.h>

/* 系统运行状态 */
typedef enum {
    STATE_IDLE,
    STATE_MONITORING,
    STATE_TRIGGERED,
    STATE_CONFIRMING,
    STATE_ACTION
} system_state_t;

/* PIR 检测到人体运动时由 A 调用 */
void on_pir_motion_detected(void);

/* 画面变化分数超过阈值时由 A 调用 */
void on_frame_change_detected(float score);

/* 获取当前 LED 状态，A 用于 UI 显示 */
bool led_is_on(void);

/* 状态机主循环，A 在 main loop 中周期性调用 */
void state_machine_tick(void);
```

---

### 3.3 关键帧筛选接口（C 提供）

文件：`components/keyframe/keyframe.h`

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *jpeg_buf;
    size_t jpeg_len;
    int64_t timestamp_ms;
    float clarity_score;
} keyframe_t;

/* 每收到一帧图像时由 A 调用 */
void keyframe_feed(const uint8_t *rgb565, int w, int h);

/* 用户唤醒或询问时，获取当前最佳关键帧 */
const keyframe_t *keyframe_get_best(void);

/* 事件触发时保存前中后关键帧组 */
void keyframe_save_event_group(void);
```

---

## 四、无硬件开发方法

### 4.1 B 的云端开发流程

1. 在 PC 上注册百度飞桨星河社区账号，获取 API Token
2. 用 Python 脚本调通 VLM / ASR / TTS 接口
3. 将 Python 逻辑改写为 C 代码
4. A 将 `cloud_api.c` 集成到 ESP32 工程

B 需要 A 配合提供：
- 开发板拍摄的一张真实 JPEG 图片（确认压缩后大小）
- 板子联网后 HTTP 请求的日志（确认能成功发送请求）

---

### 4.2 C 的本地逻辑开发流程

1. 在 PC 上用 OpenCV 或纯 C 实现关键帧筛选算法
2. 用模拟输入测试状态机跳转
3. 用静态图片或视频文件验证关键帧选择效果
4. A 将代码集成到板子后，替换模拟输入为真实 GPIO / 摄像头事件

示例 PC 模拟程序：

```c
/* 用于在 PC 上验证状态机 */
int main(void) {
    on_pir_motion_detected();
    on_frame_change_detected(85.0f);

    for (int i = 0; i < 100; i++) {
        state_machine_tick();
        usleep(100000);
    }
    return 0;
}
```

---

## 五、Git 分支策略

```
main/              # A 维护，仅在板子验证通过的代码
feature/cloud/     # B 开发云端 API
feature/keyframe/  # C 开发关键帧筛选
feature/state/     # C 开发状态机与联动逻辑
feature/ui/        # C 或 A 开发屏幕 UI
```

**合并节奏：**
- B、C 每周至少向对应 feature 分支推送一次代码
- A 每周固定时间合并 feature 分支到 main，并在板子上测试
- 测试不通过时，A 在 issue/群聊中反馈日志，由 B/C 修复后重新提交

---

## 六、项目阶段与里程碑

### 阶段 1：云端 AI 主链路（优先级最高）

**目标：** 实现"触发 → 拍照 → VLM → TTS → 播报"核心链路。

| 负责人 | 任务 |
|--------|------|
| B | 百度飞桨 VLM / TTS API 调通并封装为 C 模块 |
| A | 在板子上集成 cloud_api，用串口命令触发拍照问答 |
| C | 准备屏幕 UI 状态显示框架 |

**验收标准：** 对着摄像头提问，系统能语音回答。

---

### 阶段 2：语音交互闭环

**目标：** 用语音替代手动触发。

| 负责人 | 任务 |
|--------|------|
| A | 集成 ESP-SR WakeNet，烧录唤醒词模型 |
| B | ASR 录音转文字接口封装 |
| C | 设计唤醒后交互状态机 |

**验收标准：** 说"小智小智，这是什么"，系统自动拍照并回答。

---

### 阶段 3：本地智能增强

**目标：** 减少云端调用，增加事件记忆。

| 负责人 | 任务 |
|--------|------|
| C | 关键帧筛选算法 + 事件关键帧组保存 |
| A | 在板子上验证 PSRAM 缓冲区和实时性 |
| B | 配合设计 VLM 提问策略（如"刚才发生什么"） |

**验收标准：** 画面变化时主动提醒，能回答"刚才发生什么"。

---

### 阶段 4：物联网执行闭环

**目标：** 实现人走灯灭等物理联动。

| 负责人 | 任务 |
|--------|------|
| A | PIR GPIO 中断 + LED PWM 驱动 |
| C | 状态机与 PIR/LED 联动逻辑 |
| B | 配合设计语音确认对话流程 |

**验收标准：** 人离开后 LED 自动熄灭，或语音命令控制 LED。

---

## 七、外设采购清单

| 外设 | 型号/规格 | 数量 | 负责人 | 用途 |
|------|----------|------|--------|------|
| 无源喇叭 | 4Ω 3W，带腔体 | 1 | A | TTS 语音播报 |
| PIR 传感器 | AM312 或 HC-SR501 | 1 | A | 人体运动检测 |
| LED 灯带/灯珠 | WS2812B 或白光 LED + 驱动 | 1 | A | 模拟灯光开关 |

**说明：** 不采购光照传感器，改用 PIR + 视频检测算法共同实现人走灯灭。

---

## 八、沟通与协作规范

1. **每周固定时间线上同步：** 汇报进度、对齐接口、解决阻塞
2. **A 每次烧录后必须保存完整日志：** 便于 B/C 远程分析
3. **B/C 提交代码前必须在 PC 上编译/测试通过：** 减少 A 的集成返工
4. **接口变更必须三方确认：** 避免单方面修改导致集成失败
5. **使用 GitHub Issues 或群聊记录问题：** 每个问题明确负责人和截止时间

---

## 九、立即执行项

| 负责人 | 任务 | 截止时间 |
|--------|------|---------|
| A | 关闭音频 loopback 测试，保持板子基础环境干净 | 本周 |
| B | 注册百度飞桨账号，用 Python 调通 VLM 拍照问答 | 本周 |
| C | 在 PC 上用 OpenCV 实现关键帧筛选原型 | 本周 |

---

## 十、文档维护

本文件为活文档，随着项目进展和分工调整应及时更新。任何接口变更、角色调整、里程碑变动都应同步修改本文件。
