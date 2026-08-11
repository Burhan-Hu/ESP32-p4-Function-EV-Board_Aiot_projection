# 智学 Guardian 项目功能与验证说明

> 基于 ESP32-P4 的 AIoT 网课专注守护与课程总结终端。
> 本文档汇总系统已实现的所有功能、实现方法以及对应的验证操作，便于向老师演示与说明。

---

## 一、项目概述

本系统以 **ESP32-P4-Function-EV-Board** 为核心，搭载 SC2336 摄像头（1024×600 RGB565）、ES8311 音频编解码器、PIR 人体感应与 DSI 屏幕，实现了一个面向网课学习场景的 AIoT 伴学终端。系统通过端侧行人检测、跨外观行人重识别（Re-ID）、云端大语言模型（VLM）和语音合成（TTS），提供“你是谁、学了多久、是否专注、环境是否有异常”四大能力。

**核心硬件**：
- ESP32-P4（16 MB Flash / 32 MB PSRAM）
- SC2336 摄像头 1024×600 RGB565
- ES8311 音频芯片 + 扬声器
- PIR 人体红外传感器（GPIO20）
- DSI 屏幕显示

**软件栈**：ESP-IDF v5.5.2、ESP-DL、ESP-WHO、LVGL、豆包 TTS、百度 ASR、火山方舟 VLM。

---

## 二、功能总览

| 功能 | 一句话说明 | 技术要点 |
|------|------------|----------|
| 行人检测 | 检测画面是否有人，输出在座/离座状态 | ESP-WHO PedestrianDetect，每 250 ms 推理一次 |
| 跨外观 Re-ID | 识别画面中是谁，不受衣服、姿态、视角影响 | OSNet-x0.25 上半身，128 维特征，INT8 量化 |
| 持久化 Gallery | 注册身份、模板、累计专注时长掉电不丢失 | SPIFFS 分区 `/spiffs/reid_gallery.bin` |
| 语音注册 | 说名字完成身份注册 | 唤醒词 → ASR → 提取 Re-ID 特征 → 写入 Gallery |
| 专注计时 | 语音开始/结束学习，自动计时 | 基于 FreeRTOS tick 的秒级计时器 |
| 时长查询 | 语音查询当前/累计专注时长 | 从 Gallery 读取 total_focus_seconds 并语音播报 |
| 记录管理 | 语音删除/清空 Re-ID 记录或专注时长 | 支持按名字、当前身份或全部清除 |
| 监测模式 | 人离开后对房间环境变化进行监测 | PIR 触发 + VLM 前后帧对比 + Re-ID 白名单过滤 |
| UI 优化 | 显示当前/累计专注时长、身份、姿态等 | LVGL 自定义字体，减少刷新闪烁 |
| 语音交互 | 所有指令均通过“你好小智”唤醒后语音下达 | 百度 ASR + 豆包 TTS v3 |

---

## 三、各功能详细实现与验证

### 3.1 行人检测（human_detect）

**实现方法**：
- 使用 `components/human_detect/human_detect.cpp` 调用 ESP-WHO 的 `PedestrianDetect` 模型。
- 任务周期 `CONFIG_HUMAN_DETECT_INTERVAL_MS` 默认 250 ms，从摄像头帧读取器获取 RGB565 图像。
- 维护 `在座/离座` 状态，支持进入确认帧数和离开超时（默认 8000 ms）。
- 检测到人时回调 `on_human_present()`，人离开超过超时时间回调 `on_human_left()`。

**验证操作**：
1. 站在摄像头前，观察日志：
   ```text
   human_detect: person present
   mipi_isp_dsi: human present callback
   ```
2. 离开摄像头视野 8 秒以上，观察日志：
   ```text
   human_detect: person left, duration=... ms
   mipi_isp_dsi: human left callback, presence_duration=... ms
   ```
3. 观察 UI 左下角“在座 / 离座”状态是否正确切换。

---

### 3.2 跨外观行人重识别（Re-ID）

**实现方法**：
- 模型：`components/reid_model/reid_model.cpp` 加载 `reid_model` 分区中的 OSNet-x0.25 上半身模型（`osnet_upperbody.espdl`）。
- 输入：128×64 RGB，预处理使用 `dl::image::ImagePreprocessor` 做归一化。
- 输出：128 维 L2 归一化浮点特征向量。
- 人体区域：优先使用 `human_detect` 输出的 bbox（`human_detect_get_latest_bbox`），未检测到则回退到中心裁剪。
- 对 bbox 取上半部分（upper body）作为 Re-ID 输入，降低衣服下半身变化影响。
- 匹配：使用余弦相似度，默认阈值 0.65；匹配成功则更新命中次数并做 EMA 特征更新。

**关键代码**：
```cpp
reid_model_extract(s_frame_buffer, s_cam_width, s_cam_height, x1, y1, x2, y2, feature);
reid_gallery_query(feature, 0.65f, &sim); // 只识别，不注册
reid_gallery_update(feature, now_ms, &sim); // 识别或注册新身份
```

**验证操作**：
1. 注册身份后（见 3.4），站到摄像头前，观察日志：
   ```text
   mipi_isp_dsi: Re-ID using detect bbox (0,0,1023,599) score=...
   reid_gallery: matched 小胡 (id=1), sim=..., hits=...
   ```
2. 换衣服或侧身坐下，看是否仍能匹配到同一人（相似度应 ≥ 0.65）。
3. 观察 UI 左下角显示的身份名是否与注册名一致。

---

### 3.3 持久化 Re-ID Gallery

**实现方法**：
- `components/reid_model/reid_gallery.cpp` 维护最多 8 人的内存 Gallery。
- 每个条目包含：person_id、姓名、128 维特征、累计专注秒数、命中次数、最后 seen 时间。
- SPIFFS 分区 `storage` 位于 0xB10000，大小 256 KB。
- 保存文件为 `/spiffs/reid_gallery.bin`，二进制格式，包含 magic/version/dim/count 校验。
- 写 SPIFFS 操作放在内部 RAM 任务 `reid_gallery_save_task` 中，避免在 `human_detect` 任务中直接操作 flash 导致 cache 异常崩溃。

**验证操作**：
1. 注册一个身份并学习一段时间（如 5 秒）。
2. 说“结束学习”保存后，观察日志：
   ```text
   reid_gallery: added 5s focus to ... total=5s
   ```
3. 给设备断电，重新上电。
4. 启动日志应出现：
   ```text
   reid_gallery: gallery loaded, active=1
   ```
5. 再次站到摄像头前，UI 显示的“累计”时长应为断电前的值。

---

### 3.4 语音注册

**实现方法**：
- 唤醒词“你好小智”后，ASR 识别到“记住我，我叫 XXX”或“我叫 XXX”。
- `parse_enroll_name()` 从文本中提取名字。
- 调用 `reid_gallery_enroll_start(name)` 预留 Gallery 槽位。
- 立即提取当前 Re-ID 特征，调用 `reid_gallery_enroll_feed()` 与 `reid_gallery_enroll_finish()` 完成单帧注册。
- 当前身份立即更新，并语音播报“我记住你了，XXX。”

**验证操作**：
1. 站到摄像头前，正对镜头。
2. 唤醒后说：“记住我，我叫小胡。”
3. 观察日志：
   ```text
   voice enroll command, name=小胡
   enrollment started for '小胡'
   enrollment feed 1 samples
   ```
4. 听到语音：“我记住你了，小胡。”
5. UI 左下角显示“小胡”而非“陌生人”。

---

### 3.5 专注计时

**实现方法**：
- 当前身份 `s_current_person_id` 有效时，说“开始计时 / 开始学习 / 我要学习”触发 `focus_start()`。
- 记录开始时间 `s_study_start_ms`，设置 `s_study_active = true`。
- `focus_tick_task` 每秒调用 `update_focus_ui()` 刷新 UI 当前/累计时长。
- 人离开或说“结束学习 / 停止计时 / 不学了”触发 `focus_stop(true)`，将本次时长累加到 Gallery 并触发保存。

**验证操作**：
1. 注册并识别为“小胡”后，说：“开始计时。”
2. 听到“开始学习，加油！”
3. 观察 UI 专注页：
   - 当前 00:01 → 00:02 → 00:03 每秒增加
   - 累计也从 00:00 开始累加
4. 说“结束学习”。
5. 听到“本次学习结束，共 00:05。”
6. 日志出现：
   ```text
   focus stopped for 小胡 (id=1), duration=5s, saved=1
   ```

---

### 3.6 语音查询专注时长

**实现方法**：
- 识别到“我学了多久 / 我的学习记录 / 专注时长”等关键词触发 `focus_report()`。
- 若当前无缓存身份，先提取当前帧做一次 Re-ID 识别。
- 从 Gallery 读取 `total_focus_seconds`，如果正在学习则加上当前会话时长。
- 语音播报，例如“你累计专注 01:32。”

**验证操作**：
1. 已经有一些学习记录后，唤醒说：“我的学习记录。”
2. 观察日志：
   ```text
   focus report: 小胡 total=92s current=0s
   ```
3. 听到语音：“你累计专注 01:32。”

---

### 3.7 语音删除/清空记录

**实现方法**：
- 新增 `reid_gallery_delete_person()` 删除某个身份的所有记录。
- 新增 `reid_gallery_reset_focus_time()` 仅清空某身份的专注时长。
- 语音解析支持：
  - 全部清除：“清除所有记录 / 全部删除 / 全部清空” → `reid_gallery_reset()`
  - 删除当前：“删除我的记录 / 清除我的记录”
  - 删除指定人：“删除小胡 / 清除小胡”
  - 清空当前专注时长：“清空我的学习时长 / 我的学习时长清零”
  - 清空指定人专注时长：“清空小胡的学习时长 / 小胡的学习时长清零”

**验证操作**：
1. 唤醒后说：“清除所有记录。”
2. 听到“已清除所有记录。”
3. 断电重启，日志应出现：
   ```text
   reid_gallery: no existing gallery file, starting empty
   ```
4. 重新注册，然后说“清空我的学习时长”，听到“已清空 小胡 的专注时长。”
5. 再次查询“我的学习记录”，应听到“还没有学习记录。”

---

### 3.8 监测模式（Monitoring）

**实现方法**：
- 说“开始监测 / 开始监控 / 帮我看着”触发 `monitor_start()`。
- 将当前识别身份设为白名单 `s_monitor_whitelist_id`。
- 捕获当前画面为参考帧 `s_monitor_ref_jpeg`。
- 5 分钟内 PIR 触发时，先拍新帧，再用 Re-ID 判断是否为白名单本人：是则直接跳过，不调用 VLM；否则将参考帧与新帧一起发给火山方舟 VLM 对比。
- VLM 返回“NO_CHANGE”或“没有变化”则静默；否则语音播报变化内容。
- 说“结束监测 / 结束监控”或 5 分钟超时后退出。

**验证操作**：
1. 注册为“小胡”后，唤醒说：“开始监测。”
2. 日志应出现：
   ```text
   monitoring started
   monitor whitelist set to 小胡 (id=1, sim=...)
   captured 1024x600 JPEG, size=...
   ```
3. 本人继续在摄像头前走动，日志应出现：
   ```text
   monitor: whitelist identity 小胡 detected, skip VLM
   ```
4. 让另一个人进入画面，或改变环境，应触发 VLM 请求并语音播报变化。
5. 说“结束监测”退出。

---

### 3.9 UI 优化

**实现方法**：
- `ui_model_t` 增加 `identity_name`、`current_focus_seconds`、`total_focus_seconds` 字段。
- `focus_tick_task` 每秒只更新 timer 标签，避免每秒重建整个 LVGL 对象导致闪烁。
- 自定义 LVGL 字体 `ui_font_custom.c`，包含 551 个常用 CJK 字符，支持常见姓名用字。
- 专注页显示：身份名、当前时长、累计时长、在座/离座、姿态、页面状态等。

**验证操作**：
1. 观察专注页面是否显示“当前 00:XX”和“累计 00:XX”。
2. 观察学习时 UI 是否每秒平滑更新，而非整屏闪烁。
3. 注册姓名包含“胡”等字，观察 UI 是否显示正常，无方块。

---

### 3.10 语音交互（ASR + TTS）

**实现方法**：
- 唤醒词：使用乐鑫 WakeNet 模型“你好小智”，由 `wake_word` 组件检测。
- ASR：唤醒后录制 3 秒音频，调用百度语音识别 API，返回文字。
- 指令解析：在 `handle_voice_command()` 中识别注册、计时、查询、清除等关键词。
- TTS：采用豆包语音合成大模型 v3（24 kHz MP3），`cloud_api.c` 解析 NDJSON 响应、提取 base64 MP3 片段、minimp3 解码。
- 为了避免 MP3 解码在唤醒任务的小栈中溢出，新建 `tts_worker` 任务（32 KB 栈，位于 PSRAM），异步完成 TTS 请求、解码与播放。
- 音频输出：16 kHz、16 bit、单声道转立体声，硬件音量通过 `CONFIG_AUDIO_PLAYBACK_VOLUME` 控制。

**验证操作**：
1. 说“你好小智”，日志出现 `wake word detected`。
2. 说完命令（如“开始计时”）后，日志出现：
   ```text
   ASR result: 开始计时。
   TTS queued: 开始学习，加油！
   TTS worker playing ... samples
   ```
3. 听到清晰的语音播报，无爆音、无“滋啦”声。

---

## 四、系统架构图

```
┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐
│ SC2336 摄像头 │ → │ ISP/RGB565  │ → │ human_detect (行人) │
└─────────────┘    └─────────────┘    └─────────────────────┘
                                               │
                                               ▼
                                      ┌─────────────────────┐
                                      │ reid_model (OSNet)  │
                                      │ 提取 128-d 特征      │
                                      └─────────────────────┘
                                               │
                                               ▼
                                      ┌─────────────────────┐
                                      │ reid_gallery (持久化)│
                                      │ 身份匹配/注册/计时   │
                                      └─────────────────────┘
                                               │
           ┌──────────────────────────────────┼──────────────────────────────────┐
           │                                  │                                  │
           ▼                                  ▼                                  ▼
    ┌──────────────┐               ┌──────────────┐               ┌──────────────┐
    │   UI / LVGL  │               │  语音交互     │               │  监测模式     │
    │ 当前/累计时长 │               │ ASR / TTS    │               │ PIR + VLM    │
    └──────────────┘               └──────────────┘               └──────────────┘
```

---

## 五、关键文件清单

| 文件/组件 | 作用 |
|-----------|------|
| `main/mipi_isp_dsi_main.c` | 主应用：回调注册、语音指令解析、专注计时、监测逻辑 |
| `components/reid_model/reid_model.cpp` | OSNet 模型加载与推理 |
| `components/reid_model/reid_gallery.cpp` | 身份 Gallery、持久化、专注时长 |
| `components/human_detect/human_detect.cpp` | 行人检测与在座/离座状态 |
| `components/camera_capture/camera_capture.c` | RGB565 → JPEG 编码（用于 VLM 与监测） |
| `components/cloud_api/cloud_api.c` | 百度 ASR、豆包 TTS、火山 VLM |
| `components/audio/audio.c` | ES8311 I2S 音频播放与录音 |
| `components/ui/ui_lvgl.c` / `ui_font_custom.c` | LVGL UI 与自定义字体 |
| `tools_temp/gen_ui_font.py` | 自定义字体生成脚本 |

---

## 六、后续建议

1. **Re-ID 多模板与视角建模**：当前 Gallery 每人只保存一个 EMA 特征，换衣服、侧身/背面时匹配率下降。建议为每个人按视角（正面/侧面/背面）保存多个模板，并参考论文 MVMA 的跨视角损失思路。
2. **端侧注意力检测**：目前依赖云端 VLM 判断专注，成本高、延迟大。可在端侧加一个轻量分类器，基于姿态/头部朝向/面部位置判断是否在专注。
3. **隐私保护模式**：增加物理遮挡或“关闭摄像头”的语音指令，所有视觉推理仅在本地完成，VLM 不上传。
4. **云端成本优化**：监测模式可以只在状态变化时调用 VLM；TTS 可缓存常用提示音（如“开始学习”），避免每次都请求云端。
5. **多学生场景**：当前 Gallery 只支持 8 人，且身份在“陌生人”与注册名之间容易跳。建议为 Gallery 增加更稳定的匹配策略（如连续 N 帧一致才切换身份）。
6. **音频质量**：如仍有轻微底噪，可尝试给 ES8311 增加硬件低通滤波或降低 DAC 增益，同时检查扬声器与功放供电稳定性。

---

*文档版本：2026-07-19*
*对应固件：mipi_isp_dsi.bin 0x436180*
