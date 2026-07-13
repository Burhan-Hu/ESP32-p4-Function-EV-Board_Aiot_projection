# Guardian UI Overlay

本模块负责 ESP32-P4 屏幕上的视频叠层 UI。摄像头实时画面是背景，UI 只显示轻量 HUD、温和提醒、事件框选和课程总结遮罩。

## 分工边界

- UI 不做算法识别。
- UI 不发起网络请求。
- UI 不直接依赖云端 API。
- 算法模块只需要把状态和事件传给 `ui_manager_update()` / `ui_manager_push_event()`。

## 常用接口

- `ui_manager_update(const ui_model_t *model)`：更新 Wi-Fi、摄像头、隐私模式、学习时长、LearnScore 等全局状态。
- `ui_manager_push_event(const ui_event_t *event)`：推送算法识别事件，例如离座、手机干扰、趴桌、切屏、低头过久。
- `ui_manager_show_alert(const char *event_type, const char *message)`：兼容旧接口，显示一条温和提醒。
- `ui_manager_show_summary(const char *title, const char *summary)`：课程结束后显示总结遮罩。

## 事件显示策略

UI 会对事件做基础防打扰处理：低置信度、持续时间过短、短时间重复的同类事件不会立刻弹出大提醒。带有 `bbox` 或 `needs_review` 的事件会进入画面框选叠层。

真实屏幕绘制后续可在 LVGL 或 RGB565 framebuffer 叠字方案中接入，目前默认先通过日志验证 overlay 流程。
