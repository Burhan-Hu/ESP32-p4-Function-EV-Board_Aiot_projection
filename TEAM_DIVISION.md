# ESP32-P4 AIoT 项目团队分工方案

## 一、项目目标

基于 ESP32-P4-Function-EV-Board，构建一个多模态 AIoT 智能体终端，实现：

- **视觉问答**：语音唤醒 → 拍照 → 云端 VLM 回答 → 屏幕显示 + 语音播报
- **环境监控**：PIR 检测环境变化 → 关键帧截取 → 事件记录/提醒
- **人走灯灭**：ESP-WHO 本地人体检测 → LED 自动开关
- **状态与展示**：全局状态机 + 屏幕 UI 状态显示

---

## 二、团队角色与任务划分

团队按功能模块划分为 **4 个角色**，每个角色有明确交付物和接口边界。

---

### 角色 1：人机交互主链路

**核心任务：** 实现从语音唤醒到云端回答再到语音/屏幕输出的完整交互链路。

具体工作：
- 集成 ESP-SR 语音唤醒模型，管理唤醒词触发
- 集成 ASR 语音识别，将用户语音转为文本指令
- 解析指令类型：摄像头问答 / 环境监控任务
- 实现关键帧提取与缓存（清晰度 + 帧差）
- 对接云端 VLM / ASR / TTS API
- 管理 PIR 中断，实现环境变化监控模式
- 触发屏幕 UI 更新和扬声器语音播报

交付物：
- `components/hci/hci_engine.h`：人机交互引擎接口
- `components/keyframe/keyframe.h`：关键帧提取与缓存
- `components/cloud_api/cloud_api.h`：云端 API 封装
- `components/monitor/env_monitor.h`：PIR 环境监控接口

---

### 角色 2：ESP-WHO 人体检测与物联网执行

**核心任务：** 基于 ESP-WHO 实现本地人体检测，并控制 LED 实现人走灯灭。

具体工作：
- 集成 ESP-WHO 人体检测模型
- 从摄像头视频流持续推理，维护"是否有人"状态
- 设计人员存在/离开的判断策略（超时时间、置信度阈值）
- 控制 LED 灯带实现"开灯/关灯"模拟
- 向状态机提供人员存在状态查询接口

交付物：
- `components/human_detect/human_detect.h`：人体检测接口
- `components/led_controller/led_controller.h`：LED 控制接口
- 人走灯灭逻辑实现

---

### 角色 3：系统状态机

**核心任务：** 设计并实现全局状态机，管理各模式之间的切换与互斥。

具体工作：
- 定义系统运行状态：IDLE、LISTENING、CAPTURING、ASKING、SPEAKING、MONITORING、ALERT 等
- 设计状态转换条件和事件触发机制
- 协调人机交互链路、环境监控、人走灯灭三条独立运行线程
- 提供状态查询和状态切换接口给 UI 和其他模块
- 处理异常状态和错误降级

交付物：
- `components/state_machine/state_machine.h`：状态机接口
- 状态转换图和状态说明文档

---

### 角色 4：UI 设计与展示

**核心任务：** 设计并实现屏幕 UI，展示系统状态、答案文本和关键帧图像。

具体工作：
- 基于 LVGL v9 设计 UI 页面和控件布局
- 设计不同系统状态对应的 UI 样式（空闲、监听中、思考中、播报中、监控中、提醒）
- 实现答案文本显示、关键帧缩略图显示、状态图标显示
- 与状态机联动，根据状态自动切换 UI
- 设计演示场景的 UI 流程

交付物：
- `components/ui/ui_manager.h`：UI 管理接口
- UI 设计稿或布局说明
- 各状态对应的 UI 效果图

---

## 三、PIR 功能定位说明

在本项目中，PIR 不再用于"人走灯灭"。人走灯灭功能由 **ESP-WHO 人体检测** 实现。

PIR 的新定位是：**环境变化监控传感器**。

### PIR 触发场景

```text
用户开启监控模式
    ↓
PIR 检测到红外热源运动（如有人进入画面区域）
    ↓
触发关键帧截取
    ↓
保存事件到缓冲区 / 上报 / 屏幕提示
```

### PIR 与关键帧的关系

| 触发源 | 作用 |
|--------|------|
| PIR | 快速检测运动事件，唤醒系统截取关键帧 |
| 关键帧提取 | 保存事件发生前后的清晰画面，用于后续查看或 VLM 分析 |

### 注意

PIR 对"热源运动"敏感，对"普通物体被移动"（如书本、椅子）不够敏感。如果需求是检测任意画面变化，应额外使用**帧差算法**作为补充。

---

## 四、模块接口约定

四个角色的代码通过以下 C 接口交互。

### 4.1 人机交互引擎接口（角色 1 提供）

```c
/* hci_engine.h */
#pragma once
#include "esp_err.h"

/* 初始化人机交互引擎 */
esp_err_t hci_engine_init(void);

/* 启动语音唤醒监听 */
esp_err_t hci_start_listening(void);

/* 进入环境监控模式 */
esp_err_t hci_start_monitoring(void);

/* 退出环境监控模式 */
esp_err_t hci_stop_monitoring(void);
```

---

### 4.2 关键帧接口（角色 1 提供）

```c
/* keyframe.h */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *jpeg_buf;
    size_t jpeg_len;
    int64_t timestamp_ms;
    float clarity_score;
    float motion_score;
} keyframe_t;

/* 每收到一帧图像时调用 */
void keyframe_feed(const uint8_t *rgb565, int w, int h);

/* 获取当前最佳关键帧 */
const keyframe_t *keyframe_get_best(void);

/* 事件触发时保存前中后帧 */
void keyframe_save_event_group(void);
```

---

### 4.3 云端 AI 接口（角色 1 提供）

```c
/* cloud_api.h */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char *text;   /* 调用者需调用 cloud_response_free 释放 */
    int len;
} cloud_response_t;

/* VLM 视觉问答 */
esp_err_t cloud_vlm_ask(const char *question,
                        const uint8_t *jpeg_data,
                        size_t jpeg_len,
                        cloud_response_t *out);

/* ASR 语音识别 */
esp_err_t cloud_asr_transcribe(const int16_t *pcm_data,
                               int pcm_samples,
                               cloud_response_t *out);

/* TTS 语音合成 */
esp_err_t cloud_tts_speak(const char *text,
                          uint8_t **pcm_out,
                          size_t *pcm_len);

void cloud_response_free(cloud_response_t *resp);
```

---

### 4.4 人体检测接口（角色 2 提供）

```c
/* human_detect.h */
#pragma once
#include <stdbool.h>

/* 初始化 ESP-WHO 人体检测 */
int human_detect_init(void);

/* 启动人体检测 */
int human_detect_start(void);

/* 当前是否检测到人体 */
bool human_is_present(void);

/* 设置人离开超时时间（毫秒） */
void human_set_leave_timeout(uint32_t ms);

/* 人员离开回调（角色 2 内部使用，也可通知状态机） */
typedef void (*human_left_callback_t)(uint32_t duration_ms);
void human_register_left_callback(human_left_callback_t cb);
```

---

### 4.5 LED 控制接口（角色 2 提供）

```c
/* led_controller.h */
#pragma once

/* 初始化 LED PWM */
int led_controller_init(void);

/* 开灯 */
void led_turn_on(void);

/* 关灯 */
void led_turn_off(void);

/* 设置亮度 0~100 */
void led_set_brightness(int percent);

/* 查询当前状态 */
bool led_is_on(void);
```

---

### 4.6 系统状态机接口（角色 3 提供）

```c
/* state_machine.h */
#pragma once

typedef enum {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_CAPTURING,
    STATE_ASKING,
    STATE_SPEAKING,
    STATE_MONITORING,
    STATE_ALERT
} system_state_t;

/* 初始化状态机 */
void state_machine_init(void);

/* 主循环调用 */
void state_machine_tick(void);

/* 切换到指定状态 */
void state_transition_to(system_state_t new_state);

/* 获取当前状态 */
system_state_t state_get_current(void);
```

---

### 4.7 UI 管理接口（角色 4 提供）

```c
/* ui_manager.h */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum {
    UI_STATE_IDLE,
    UI_STATE_LISTENING,
    UI_STATE_THINKING,
    UI_STATE_SPEAKING,
    UI_STATE_MONITORING,
    UI_STATE_ALERT
} ui_state_t;

/* 初始化 UI */
void ui_init(void);

/* 设置 UI 状态 */
void ui_set_state(ui_state_t state);

/* 显示答案文本 */
void ui_show_answer(const char *text);

/* 显示关键帧缩略图 */
void ui_show_keyframe(const uint8_t *jpeg, size_t len);

/* 显示状态提示 */
void ui_show_status(const char *status);
```

---

## 五、开发流程与协作规范

### 5.1 并行开发原则

- 角色 1 先完成音频通路和云端 API 的 PC 端验证
- 角色 2 先完成 ESP-WHO 模型集成和 LED 驱动
- 角色 3 和角色 4 紧密协作，先定义状态和 UI 的对应关系
- 各模块通过接口 stub 在 PC 上独立验证逻辑

### 5.2 Git 分支策略

```
main/              # 仅在板子验证通过的代码
feature/hci/       # 人机交互主链路
feature/keyframe/  # 关键帧提取
feature/cloud/     # 云端 API
feature/human_detect/  # ESP-WHO 人体检测
feature/led/       # LED 控制
feature/state/     # 状态机
feature/ui/        # 屏幕 UI
```

### 5.3 集成节奏

- 每周固定时间合并 feature 分支到 main
- 每次合并后在开发板上完整测试一遍
- 测试结果以日志和视频形式记录
- 发现问题后回退到对应 feature 分支修复

### 5.4 接口变更流程

任何接口变更必须经过相关角色共同确认，避免单方面修改导致集成失败。

---

## 六、项目阶段与里程碑

### 阶段 1：基础能力验证

**目标：** 摄像头、麦克风、扬声器、Wi-Fi、屏幕全部可用。

| 角色 | 任务 |
|------|------|
| 角色 1 | 验证音频录音/播放、准备云端 API 账号 |
| 角色 2 | 确认 LED 驱动和摄像头帧访问方式 |
| 角色 3 | 设计全局状态定义 |
| 角色 4 | 设计 UI 页面框架 |

**验收标准：** 板子能拍照、录音、联网、显示、控制 LED。

---

### 阶段 2：人机交互主链路

**目标：** 实现"语音唤醒 → 拍照 → VLM → TTS → 播报 + 显示"。

| 角色 | 任务 |
|------|------|
| 角色 1 | 完成唤醒、ASR、关键帧、VLM、TTS 链路 |
| 角色 3 | 定义 ASKING/SPEAKING 等状态 |
| 角色 4 | 实现答案显示和状态切换 UI |
| 角色 2 | 确保摄像头帧可被角色 1 稳定获取 |

**验收标准：** 说唤醒词 + 问题，系统自动拍照并在屏幕和扬声器返回答案。

---

### 阶段 3：环境监控功能

**目标：** PIR 触发 → 关键帧截取 → 事件记录/提醒。

| 角色 | 任务 |
|------|------|
| 角色 1 | 实现 PIR 中断 + 关键帧事件保存 |
| 角色 3 | 定义 MONITORING/ALERT 状态 |
| 角色 4 | 实现监控模式 UI 和事件提醒 UI |
| 角色 2 | 配合确认摄像头帧共享无冲突 |

**验收标准：** 开启监控模式后，有人进入区域时屏幕提示并保存关键帧。

---

### 阶段 4：人走灯灭联动

**目标：** ESP-WHO 检测人体 → 人离开后 LED 自动熄灭。

| 角色 | 任务 |
|------|------|
| 角色 2 | 集成 ESP-WHO，实现人体检测和 LED 控制 |
| 角色 3 | 将人体存在状态纳入系统状态 |
| 角色 4 | 在 UI 上显示人员存在状态 |
| 角色 1 | 确保不与人机交互链路抢摄像头资源 |

**验收标准：** 人离开后若干秒，LED 自动熄灭；人来了自动亮起。

---

### 阶段 5：联调与演示准备

**目标：** 四个演示场景全部跑通，文档视频齐全。

| 角色 | 任务 |
|------|------|
| 角色 1 | 确保问答和监控链路稳定 |
| 角色 2 | 确保人走灯灭稳定 |
| 角色 3 | 完善状态转换和异常处理 |
| 角色 4 | 完善 UI、编写演示脚本、录制演示视频 |

---

## 七、外设采购清单

| 外设 | 规格 | 数量 | 负责角色 | 用途 |
|------|------|------|---------|------|
| 无源喇叭 | 4Ω 3W，带腔体 | 1 | 角色 1 | TTS 语音播报 |
| PIR 传感器 | AM312 或 HC-SR501 | 1 | 角色 1 | 环境变化监控 |
| LED 灯带/灯珠 | WS2812B 或白光 LED + 驱动 | 1 | 角色 2 | 模拟灯光开关 |

---

## 八、文档维护

本文件为活文档。随着项目进展，接口变更、阶段调整、里程碑更新都应及时同步到本文档中。建议每次里程碑评审后更新一次。
