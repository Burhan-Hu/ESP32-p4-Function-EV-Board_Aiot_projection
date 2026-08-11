# -*- coding: utf-8 -*-
"""Generate AI image prompts for 智学Guardian design document."""
from docx import Document
from docx.shared import Pt, Cm
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn

doc = Document()
for section in doc.sections:
    section.page_width = Cm(21.0)
    section.page_height = Cm(29.7)
    section.top_margin = Cm(2.5)
    section.bottom_margin = Cm(2.5)
    section.left_margin = Cm(2.8)
    section.right_margin = Cm(2.6)


def set_font(run, size=12, bold=False, cn='宋体', en='Times New Roman'):
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.name = en
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn('w:rFonts'))
    if rfonts is None:
        rfonts = rpr.makeelement(qn('w:rFonts'), {})
        rpr.append(rfonts)
    rfonts.set(qn('w:eastAsia'), cn)


def para(text='', size=12, bold=False, cn='宋体', align=None, indent=False,
         space_after=6, space_before=0, line=None):
    p = doc.add_paragraph()
    if align is not None:
        p.alignment = align
    pf = p.paragraph_format
    pf.space_after = Pt(space_after)
    pf.space_before = Pt(space_before)
    if line is not None:
        pf.line_spacing = line
    if indent:
        pf.first_line_indent = Pt(size * 2)
    if text:
        r = p.add_run(text)
        set_font(r, size, bold, cn)
    return p


def h1(text):
    return para(text, 16, True, '黑体', space_before=12, space_after=8)


def h2(text):
    return para(text, 14, True, '黑体', space_before=8, space_after=6)


def h3(text):
    return para(text, 12.5, True, '黑体', space_before=6, space_after=4)


def body(text):
    return para(text, 12, False, '宋体', indent=True, line=1.4)


def prompt_block(title, usage, prompt_en, prompt_cn, notes):
    h3(title)
    body('用途：' + usage)
    body('英文提示词（可直接复制到 Midjourney / DALL·E / Stable Diffusion / 即梦 / 可灵等）：')
    para(prompt_en, 10.5, False, '宋体', space_after=6, line=1.3)
    body('中文提示词（备用）：')
    para(prompt_cn, 10.5, False, '宋体', space_after=6, line=1.3)
    body('生成建议：' + notes)


para('', 12)
p = para('智学Guardian 设计文档配图 AI 生成提示词', 18, True, '黑体',
         align=WD_ALIGN_PARAGRAPH.CENTER, space_after=12)
body('本文档对应《智学Guardian——基于ESP32-P4的网课专注守护AIoT终端》设计文档中的全部占位插图，'
     '提供可直接用于 AI 图像生成工具的详细提示词。每个提示词包含风格、主体、构图、配色、标注方式等要素，'
     '确保生成的图片与文档内容一致，并适合学术论文/竞赛文档的排版风格。')

h1('一、图 4-1 系统感知层硬件连接示意图')
prompt_block(
    '1.1 提示词',
    '对应第四章 4.1 节，展示 ESP32-P4 主控与摄像头、PIR、麦克风、扬声器、LCD、Wi-Fi 从机的连接关系。',
    (
        'A clean technical hardware connection diagram for an ESP32-P4 AIoT learning companion device, '
        'flat vector illustration style, white background, light blue and orange accent colors. '
        'Center: an ESP32-P4-Function-EV-Board development board with labeled pins. '
        'Connected peripherals arranged around it with labeled arrows: '
        'a SC2336 MIPI-CSI camera module at the top, '
        'a small PIR motion sensor on the right, '
        'an ES8311 audio codec with microphone and speaker on the left, '
        'an LCD display screen below, '
        'and an ESP32-C6 Wi-Fi slave module connected via SDIO on the upper right. '
        'Each connection line is clean and uses different colors for MIPI-CSI, I2S, I2C, SDIO, and power. '
        'Minimalist icons, thin lines, Chinese and English labels, professional IoT product schematic look, '
        'no shadows, high resolution, suitable for academic design document.'
    ),
    (
        '一张干净的技术硬件连接示意图，展示基于ESP32-P4的AIoT学习伴侣设备。扁平矢量插画风格，白色背景，'
        '浅蓝色和橙色点缀。中心是ESP32-P4-Function-EV-Board开发板，标注引脚。周围排列连接外设：'
        '顶部是SC2336 MIPI-CSI摄像头模组，右侧是PIR人体红外传感器，左侧是ES8311音频编解码器及麦克风、扬声器，'
        '下方是LCD显示屏，右上方是经SDIO连接的ESP32-C6 Wi-Fi从机模组。连接线简洁，用不同颜色区分'
        'MIPI-CSI、I2S、I2C、SDIO和电源。极简图标、细线条、中英双语标注，专业物联网产品原理图风格，'
        '无阴影，高分辨率，适合学术设计文档。'
    ),
    '建议用 16:9 横图，导出后可在图中添加中文标注，或直接保留英文标注；若 AI 工具支持中文，可让标签直接显示中文。'
)

h1('二、图 4-2 OSNet 全尺度残差块与上半身裁剪示意图')
prompt_block(
    '2.1 提示词',
    '对应第四章 4.2 节，展示 OSNet 全尺度残差块结构，以及从人体检测框中裁剪上半身 60% 区域的预处理过程。',
    (
        'A two-part technical AI/ML diagram, clean flat vector illustration, white background, '
        'blue and orange color scheme. Left part: an OSNet omni-scale residual block architecture, '
        'showing multiple convolutional streams with different kernel sizes (1x1, 3x3, 5x5), '
        'a unified aggregation gate (AG) dynamically fusing multi-scale features, '
        'labeled arrows, feature maps as stacked rectangles, modern deep learning paper style. '
        'Right part: a person detection bounding box over a seated student at a desk, '
        'with the upper 60% region (head, shoulders, upper torso) highlighted in orange, '
        'and the lower body dimmed in gray. Arrows show crop -> resize 128x64 -> normalization. '
        'Clean labels, Chinese/English bilingual annotations, no photorealistic elements, '
        'academic diagram, high resolution.'
    ),
    (
        '一张由两部分组成的技术AI/ML示意图，干净扁平矢量插画，白色背景，蓝橙配色。左侧为OSNet全尺度残差块结构，'
        '展示多个不同卷积核尺寸（1x1、3x3、5x5）的卷积流，以及统一聚合门（AG）动态融合多尺度特征，'
        '带标注箭头，特征图以堆叠矩形表示，现代深度学习论文风格。右侧为一个坐在书桌前的学生人体检测框，'
        '上半部分约60%区域（头肩、躯干）用橙色高亮，下半身用灰色淡化。箭头展示裁剪→缩放至128×64→归一化流程。'
        '干净标签，中英双语标注，无需写实元素，学术图表风格，高分辨率。'
    ),
    '建议左右分栏，左侧结构图可稍抽象，右侧用简单人物剪影即可，避免复杂人脸细节。'
)

h1('三、图 4-3 模型训练-量化-部署流程图')
prompt_block(
    '3.1 提示词',
    '对应第四章 4.2 节，展示从 PyTorch 训练到 ONNX 转换、ESP-PPQ INT8 量化，再到 ESP32-P4 部署的完整流程。',
    (
        'A horizontal technical pipeline diagram for edge AI model deployment, flat vector illustration, '
        'white background, blue and orange gradient accents. Four main stages connected by thick arrows: '
        '1) "Training" box with PyTorch logo, PRCC dataset icon, Softmax + Triplet labels; '
        '2) "Conversion" box with ONNX logo, torch.onnx label; '
        '3) "Quantization" box with ESP-PPQ label, INT8 badge, calibration data icon; '
        '4) "Deployment" box with ESP32-P4 chip, ESP-DL logo, NPU icon, .espdl file label. '
        'Each stage has small icons: GPU, neural network layers, chip. '
        'Clean typography, Chinese/English bilingual labels, arrows show data flow, '
        'modern tech infographic style, no shadows, high resolution, suitable for design document.'
    ),
    (
        '一张横向边缘AI模型部署流程图，扁平矢量插画，白色背景，蓝橙渐变点缀。四个主要阶段由粗箭头连接：'
        '1)“训练”框，含PyTorch标识、PRCC数据集图标、Softmax+Triplet标签；'
        '2)“转换”框，含ONNX标识、torch.onnx标签；'
        '3)“量化”框，含ESP-PPQ标签、INT8徽章、校准数据图标；'
        '4)“部署”框，含ESP32-P4芯片、ESP-DL标识、NPU图标、.espdl文件标签。'
        '每个阶段配小图标：GPU、神经网络层、芯片。字体清晰，中英双语标注，箭头展示数据流，'
        '现代科技信息图风格，无阴影，高分辨率，适合设计文档。'
    ),
    '建议用 16:9 或 4:3 横图，确保四个阶段文字清晰可读；若 AI 无法生成准确商标，可用文字代替。'
)

h1('四、图 4-4 系统整体数据流与端云协同架构图')
prompt_block(
    '4.1 提示词',
    '对应第四章 4.6 节，展示从摄像头采集到端侧检测、Re-ID、状态机、UI，再到云端 ASR/VLM/TTS 的完整数据流。',
    (
        'A comprehensive end-to-end IoT system architecture diagram, clean flat vector illustration, '
        'white background, blue and orange color scheme. Three layers: '
        'bottom layer "Edge Device" with ESP32-P4 chip, SC2336 camera, PIR sensor, microphone, speaker, LCD; '
        'middle layer "Local AI Pipeline" with icons for human detection, OSNet Re-ID, Gallery, '
        'state machine, LVGL UI, SPIFFS storage; '
        'top layer "Cloud Services" with Baidu ASR, Doubao VLM, Doubao TTS v3. '
        'Curved arrows show data flow: camera frame -> detection -> Re-ID feature -> Gallery -> '
        'UI/state machine; wake word -> mic -> ASR -> intent parser -> cloud VLM/TTS -> speaker. '
        'HTTPS/TLS shield icon between edge and cloud. Clean labels, Chinese/English bilingual, '
        'modern tech diagram, no shadows, high resolution, suitable for academic document.'
    ),
    (
        '一张完整的端到端物联网系统架构图，干净扁平矢量插画，白色背景，蓝橙配色。分三层：'
        '底层为“边缘设备”，含ESP32-P4芯片、SC2336摄像头、PIR传感器、麦克风、扬声器、LCD；'
        '中间层为“端侧AI流水线”，含人体检测、OSNet Re-ID、Gallery、状态机、LVGL UI、SPIFFS存储等图标；'
        '顶层为“云端服务”，含百度ASR、豆包VLM、豆包TTS v3。'
        '曲线箭头展示数据流：摄像头帧→检测→Re-ID特征→Gallery→UI/状态机；唤醒词→麦克风→ASR→意图解析→云端VLM/TTS→扬声器。'
        '边缘与云端之间加HTTPS/TLS盾牌图标。标签清晰，中英双语，现代科技图风格，无阴影，高分辨率，适合学术文档。'
    ),
    '建议用 16:9 横图，分层清晰，云端可用云朵图标，端侧用设备图标。若空间不足，可拆分为两张。'
)

h1('五、图 5-1 整机外观与结构图')
prompt_block(
    '5.1 提示词',
    '对应第五章 5.1 节，展示智学Guardian 桌面终端的外观、结构、屏幕布局、摄像头位置、PIR 位置。',
    (
        'A 3D isometric product render of a desktop AIoT learning companion device, clean modern design, '
        'white background, soft shadows. The device has a rectangular LCD screen at the front, '
        'left half showing a live camera preview of a student studying, right half showing a Chinese UI '
        'with focus timer, total study time, and a small orange alert banner. '
        'A small SC2336 camera module is mounted on the top center of the screen, facing the seat. '
        'A PIR motion sensor and a microphone are placed at the front bezel. '
        'The case is made of matte white plastic with orange accent line. '
        'Side view shows USB-C port and speaker grille. '
        'Labeled diagram style with callouts for camera, PIR, mic, speaker, LCD, USB-C. '
        'No text on the device body, minimalistic, high resolution, suitable for design competition document.'
    ),
    (
        '一张3D等轴测产品渲染图，展示一款桌面AIoT学习伴侣终端，干净现代设计，白色背景，柔和阴影。'
        '设备正面为矩形LCD屏幕，左半部分显示学生学习实时画面，右半部分显示中文UI，'
        '包含专注计时、累计学习时长、橙色提醒条。屏幕顶部中央安装一个小型SC2336摄像头，朝向座位。'
        '正面边框处放置PIR人体传感器和麦克风。外壳为哑光白色塑料，带橙色装饰线。'
        '侧视图显示USB-C接口和扬声器格栅。带标注线说明摄像头、PIR、麦克风、扬声器、LCD、USB-C位置。'
        '设备本体无文字，极简风格，高分辨率，适合设计竞赛文档。'
    ),
    '建议用 3:4 或 1:1 竖图，突出屏幕UI和传感器位置；屏幕上的文字可后期用PS添加，确保清晰。'
)

h1('六、可选补充插图（建议）')
body('除上述占位图外，以下插图可进一步提升文档质量，建议根据版面酌情加入。')

h2('6.1 图 2-1 上半身 Re-ID 与人脸识别对比图')
para(
    'English prompt: A side-by-side comparison infographic, flat vector illustration, white background, '
    'blue and orange color scheme. Left side: "Face Recognition" showing a front-facing face with a green check, '
    'but below it three smaller scenes with red crosses: head down writing, side face looking at screen, '
    'hand covering face. Right side: "Upper-body Re-ID" showing a seated student from shoulders to torso, '
    'with green checks for head down, side face, turning around, wearing glasses, and partial occlusion. '
    'Simple icons, no realistic faces, clean labels in Chinese and English, academic diagram style, '
    'no shadows, high resolution.',
    10.5, False, '宋体', space_after=6, line=1.3
)

h2('6.2 图 3-1 系统功能模块图')
para(
    'English prompt: A circular or modular system function diagram, flat vector illustration, '
    'white background, blue and orange accents. Center: a large circle labeled "智学Guardian Core". '
    'Five surrounding modules: "Presence & Identity", "Voice Enrollment & Focus Timer", '
    '"Leave-Seat Alert", "Voice QA & Course Summary", "After-class Space Guard". '
    'Each module has a simple icon and connected arrows to the center. '
    'Clean Chinese/English labels, modern tech infographic, no shadows, high resolution.',
    10.5, False, '宋体', space_after=6, line=1.3
)

h2('6.3 图 4-5 端云协同关键帧唤醒流程图')
para(
    'English prompt: A vertical sequence diagram for edge-cloud collaboration, flat vector illustration, '
    'white background, blue and orange accents. Two swimlanes: left "ESP32-P4 Edge" and right "Cloud". '
    'Steps from top to bottom: wake word detected, microphone recording, keyframe selected locally, '
    'HTTPS upload to cloud, Baidu ASR returns text, intent parsing, if VQA needed upload image to Doubao VLM, '
    'VLM returns answer, Doubao TTS synthesizes voice, stream MP3 back to device, speaker plays. '
    'Clean arrows, Chinese/English labels, modern tech diagram, no shadows, high resolution.',
    10.5, False, '宋体', space_after=6, line=1.3
)

OUT = '智学Guardian_设计文档配图_AI生成提示词.docx'
doc.save(OUT)
print('saved:', OUT)
