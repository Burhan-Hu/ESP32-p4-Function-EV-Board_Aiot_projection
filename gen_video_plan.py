# -*- coding: utf-8 -*-
"""Generate the video shooting plan for 智学Guardian competition."""
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


def script(text):
    return para(text, 11, False, '宋体', space_after=4, line=1.3)


para('', 12)
para('智学Guardian 视频拍摄方案', 20, True, '黑体',
     align=WD_ALIGN_PARAGRAPH.CENTER, space_after=12)
body('本方案依据全国大学生物联网设计竞赛视频上传要求编写，用于指导《智学Guardian——基于ESP32-P4的'
     '网课专注守护AIoT终端》作品原型视频的拍摄与制作。目标是在 5–10 分钟内，清晰展示作品创意、'
     '技术架构与核心功能，并满足 H.264/AAC、1080p、≤200MB、MP4 格式等硬性要求。')

h1('一、视频规格与拍摄总则')
h2('1.1 竞赛视频硬性要求')
body('视频时长：5–10 分钟。建议控制在 8 分钟左右，留有冗余。')
body('视频大小：≤200MB。建议目标码率 5000–8000 kbps，8 分钟文件约 240–380MB 时需注意压缩，'
     '实际建议用 3500–5000 kbps，确保 8 分钟文件 ≤200MB。')
body('视频分辨率：≤1920×1080。统一使用 1920×1080（Full HD）。')
body('视频格式：.mp4 或 .webm。建议使用 .mp4。')
body('视频编码：H.264。建议使用 H.264 High Profile，Level 4.0/4.1。')
body('音频编码：AAC。建议使用 AAC 128–192 kbps，48 kHz，立体声。')
body('竞赛网站不带转码功能，必须在上传前用本地工具确认编码、大小、时长均符合要求。')
body('上传只有 2 次机会，务必先内部审核完整成片后再上传。')

h2('1.2 拍摄总则')
body('以“实物演示为主、PPT/示意图为辅”：突出真实设备运行效果，避免全片都是幻灯片。')
body('画面稳定、光线充足：使用三脚架固定相机/手机，避免手持抖动；避免逆光，确保 LCD 屏幕内容清晰可读。')
body('讲解与操作同步：每个功能演示遵循“一句话说明 → 实际操作 → 结果展示”的节奏。')
body('提前彩排：正式拍摄前至少完整走一遍，确保语音指令、离座提醒、看家模式等按预期触发。')
body('分段拍摄再剪辑：可按章节分段录制，后期用剪映/PR 拼接，减少重录成本。')

h1('二、视频结构与时长分配（建议 8 分钟）')
h2('2.1 整体结构')
body('建议按“引入—架构—功能—验证—总结”五段式组织，具体如下：')
script('00:00–00:50  开场与问题引入')
script('00:50–02:10  系统架构与核心创新')
script('02:10–06:40  核心功能全流程演示')
script('06:40–07:40  测试验证与稳定性展示')
script('07:40–08:00  总结与结束语')
body('总时长约 8 分钟，允许在 5–10 分钟范围内微调。')

h2('2.2 拍摄方式')
body('采用“主机位全景 + 特写机位屏幕/操作”双机位或单机位分两次拍摄。若只有一台设备，'
     '可先拍全景讲解，再拍屏幕特写，后期叠加或切换。')

h1('三、拍摄准备清单')
h2('3.1 硬件准备')
body('ESP32-P4-Function-EV-Board 整机一台，含 SC2336 摄像头、PIR、麦克风、扬声器、LCD 屏，已烧录最新固件。')
body('USB-C 供电线、稳定电源、插线板。')
body('三脚架 1–2 个，手机/相机 1–2 台（建议 4K 拍摄，后期输出 1080p）。')
body('补光灯 1 盏或自然光充足的室内环境。')
body('干净的书桌场景，摆放电脑/书本/水杯，模拟真实网课环境。')
body('另一台手机/相机用于拍摄 LCD 屏幕特写（若无双机位，可后期用录屏软件录制屏幕内容替代）。')

h2('3.2 软件与账号准备')
body('录制软件：OBS Studio（免费，推荐）或手机自带相机。')
body('剪辑软件：剪映专业版 / Premiere Pro / DaVinci Resolve。')
body('格式检查工具：MediaInfo 或格式工厂，用于确认编码、码率、时长、大小。')
body('云端服务已开通并可调用：百度 ASR、火山引擎豆包 VLM、豆包 TTS。')
body('设备已配网，Wi-Fi 稳定，确保语音问答、课程总结、看家模式云端比对能实时响应。')

h2('3.3 道具与脚本准备')
body('打印一张 A4 提示卡，写上语音指令列表，放在镜头外提醒：')
script('“你好小智” → 唤醒')
script('“记住我，我叫小胡” → 注册')
script('“开始学习” → 启动专注计时')
script('“结束学习” → 停止计时')
script('“我的学习记录” → 查询累计时长')
script('“开始监测” → 进入课后空间安全看护')
script('“停止监测” → 退出看护')
body('准备一件不同颜色/款式的外套，用于演示“跨衣着识别”。')

h1('四、分镜拍摄脚本')
h2('4.1 开场与问题引入（00:00–00:50）')
body('画面：主持人/队员出镜，背景为书桌与设备，或设备特写。')
body('讲解词：')
script('“大家好，我们是 XXX 团队，参赛作品是《智学Guardian——基于ESP32-P4的网课专注守护AIoT终端》。”')
script('“居家网课时，学生容易分心、频繁离座，家长和老师很难实时了解学习状态。”')
script('“普通摄像头只能录像，人脸识别在低头、侧脸时容易失效，而且存在隐私风险。”')
script('“因此我们设计了一款端侧智能、保护隐私、可语音交互的专注守护终端。”')
body('拍摄要点：语速适中，保持微笑，设备放在画面一侧；若不出镜，可用设备特写 + 字幕代替。')

h2('4.2 系统架构与核心创新（00:50–02:10）')
body('画面：切到 PPT/示意图，或实物 + 手绘架构图。')
body('讲解词：')
script('“系统采用端云协同架构：ESP32-P4 端侧运行行人检测和轻量化 OSNet 上半身 Re-ID 模型，”')
script('“实时判断在/离座、识别身份；云端接入百度语音识别、豆包视觉大模型和语音合成，”')
script('“实现自然语音交互。”')
script('“核心创新有三点：一是用上半身 Re-ID 替代人脸识别，对低头、侧脸、遮挡更鲁棒，且不采集人脸；”')
script('“二是模型 INT8 量化后仅约 493KB，可在 ESP32-P4 NPU 上实时推理；”')
script('“三是本地维护身份档案和累计专注时长，SPIFFS 持久化，断电不丢失。”')
body('拍摄要点：画面可配合设计文档中的图 4-4 端云协同架构图；讲到模型时展示图 4-2/4-3。')

h2('4.3 功能演示一：语音注册与身份识别（02:10–03:10）')
body('画面：设备全景 + LCD 屏幕特写。操作者站在/坐在摄像头前。')
body('操作步骤：')
script('1. 确认设备上电，LCD 显示摄像头画面与初始 UI。')
script('2. 操作者面对摄像头说：“你好小智，记住我，我叫小胡”。')
script('3. 设备语音回复“我记住你了，小胡”，LCD 身份栏显示“小胡”。')
script('4. 操作者离开画面，LCD 显示“未识别”；回来后再次识别为“小胡”。')
body('讲解词：')
script('“首先演示语音注册。说‘记住我，我叫小胡’，设备会采集多帧上半身特征，”')
script('“写入本地身份档案库，并语音确认。之后离开再回来，设备能自动重新识别。”')
body('拍摄要点：确保麦克风清晰收录指令；LCD 特写要能看到“小胡”字样。')

h2('4.4 功能演示二：专注计时与累计（03:10–04:10）')
body('画面：LCD 屏幕特写为主，展示本次时长、累计时长、状态栏变化。')
body('操作步骤：')
script('1. 注册身份后，说“开始学习”。')
script('2. LCD 状态栏从“等待课程开始”变为“正在学习中”，本次专注时长从 00:00 开始增加。')
script('3. 保持坐姿 20–30 秒，展示时长实时累计。')
script('4. 说“结束学习”，本次时长停止，累计时长增加。')
script('5. 再说“我的学习记录”，设备语音播报累计专注时长。')
body('讲解词：')
script('“说‘开始学习’，设备开始对当前身份计时，UI 显示‘正在学习中’；”')
script('“说‘结束学习’，本次时长累加到个人累计时长并保存；”')
script('“随时可以说‘我的学习记录’查询累计专注时长。”')
body('拍摄要点：特写“正在学习中”状态变化、本次/累计时长数字跳动。')

h2('4.5 功能演示三：疑似离座温和提醒（04:10–05:00）')
body('画面：先正常坐姿，然后起身离开座位，LCD 弹出橙色提醒条。')
body('操作步骤：')
script('1. 确保已注册并开始学习。')
script('2. 正常坐 10 秒，然后起身离开摄像头范围。')
script('3. 等待约 10–20 秒（冷却时间已过），LCD 弹出橙色“疑似离座”提醒条，显示真实置信度。')
script('4. 设备语音提醒“你已经离开座位一分钟了，回来我们继续学习吧”。')
script('5. 回到座位，提醒自动消失，身份恢复显示。')
body('讲解词：')
script('“当检测到学生离座，系统会经过置信度、时长、冷却三级过滤，”')
script('“避免短暂起身误报；确认离座后，会弹出温和提醒并语音提示。”')
body('拍摄要点：离开动作要自然，不要过快；确保弹窗和语音提醒被清晰记录。')

h2('4.6 功能演示四：陌生人/未识别与跨衣着识别（05:00–05:40）')
body('画面：LCD 特写，分别展示陌生人、未识别、换装后识别。')
body('操作步骤：')
script('1. 让一位未注册的同学进入画面，LCD 显示“陌生人”或“未识别”，不会自动注册。')
script('2. 清空画面，LCD 稳定显示“未识别”。')
script('3. 已注册同学换上不同颜色外套，重新进入画面，设备仍识别为“小胡”。')
body('讲解词：')
script('“未注册人员不会被误加入档案库；空背景稳定显示‘未识别’；”')
script('“即使更换衣物，上半身 Re-ID 仍能识别为同一身份。”')
body('拍摄要点：换装环节要明显，展示跨衣着能力。')

h2('4.7 功能演示五：语音问答与课程总结（05:40–06:20）')
body('画面：操作者面对设备，展示语音交互；LCD 显示摄像头画面。')
body('操作步骤：')
script('1. 唤醒设备：“你好小智”。')
script('2. 提问：“我现在在做什么？”或“帮我总结一下这节课的重点”。')
script('3. 设备调用豆包 VLM 分析当前画面，语音合成回答。')
body('讲解词：')
script('“通过‘你好小智’唤醒后，可以结合摄像头画面进行语音问答或课程总结，”')
script('“云端大模型负责语义理解，端侧负责实时感知。”')
body('拍摄要点：确保 Wi-Fi 稳定，提前测试 VLM 响应速度；若网络较慢，可剪辑掉等待时间。')

h2('4.8 功能演示六：课后空间安全看护（06:20–07:00）')
body('画面：设备全景 + LCD 特写，模拟用户离开后的异常检测。')
body('操作步骤：')
script('1. 已注册用户说“开始监测”。')
script('2. 用户离开座位，PIR 保持静止。')
script('3. 另一人进入画面或翻动桌上物品，触发 PIR。')
script('4. 设备判断不是白名单本人，语音报警“检测到异常，请注意”。')
script('5. 用户回来说“停止监测”，退出看护。')
body('讲解词：')
script('“学习结束或暂时离开时，可以说‘开始监测’进入课后空间安全看护模式；”')
script('“PIR 低功耗触发，Re-ID 确认是否为本人；陌生人或异常移动会触发语音报警。”')
body('拍摄要点：PIR 触发可能需要几秒，提前彩排；确保报警语音清晰。')

h2('4.9 测试验证与稳定性展示（07:00–07:40）')
body('画面：快速剪辑，展示不同姿态、不同光照下的识别效果；串口日志特写（可选）。')
body('操作步骤：')
script('1. 快速切换正坐、低头、侧脸、戴眼镜等姿态，LCD 均稳定显示“小胡”。')
script('2. 展示串口日志中 sim 相似度数值（如 0.85–0.99）。')
script('3. 断电重启设备，展示档案和累计时长仍在。')
body('讲解词：')
script('“在多种真实姿态下，系统都能稳定识别；”')
script('“通过相似度区间门控的 EMA 模板更新，长时间运行不漂移；”')
script('“SPIFFS 持久化保证断电数据不丢失。”')
body('拍摄要点：串口日志可用电脑屏幕录制，注意字体大小；重启后登录 Wi-Fi 需快速剪辑。')

h2('4.10 总结与结束语（07:40–08:00）')
body('画面：设备全景或团队合影。')
body('讲解词：')
script('“智学Guardian 以 ESP32-P4 为核心，结合上半身 Re-ID 与端云协同，”')
script('“实现了低成本、保护隐私、可语音交互的网课专注守护。”')
script('“感谢观看，欢迎批评指正。”')
body('拍摄要点：简洁收尾，可展示团队名称与作品名称字幕。')

h1('五、录制与导出参数设置')
h2('5.1 OBS Studio 录制设置（电脑录屏/采集卡）')
body('分辨率：1920×1080，缩放过滤器 Lanczos。')
body('帧率：30 fps。')
body('编码器：x264 或硬件编码（NVENC H.264），Profile 选 high。')
body('码率控制：VBR，目标 4500 kbps，最大 6000 kbps。')
body('音频：AAC，48 kHz，160 kbps，立体声。')
body('格式：MP4。')
body('录制前先做 30 秒测试，用 MediaInfo 检查编码、大小。')

h2('5.2 手机/相机拍摄设置')
body('分辨率：4K 或 1080p 60fps/30fps，后期导出 1080p。')
body('锁定曝光与白平衡，避免画面忽明忽暗。')
body('使用三脚架，关闭数码防抖以外的花俏功能。')
body('尽量使用外接麦克风或靠近讲解者，保证语音清晰。')

h2('5.3 剪辑导出设置（剪映/PR/DaVinci）')
body('序列/工程：1920×1080，30fps。')
body('导出格式：MP4。')
body('视频编码：H.264，Profile High，Level 4.1。')
body('码率：VBR 2 次，目标 4500 kbps，最大 6000 kbps。')
body('音频：AAC，48 kHz，160–192 kbps。')
body('导出后检查文件大小：8 分钟约 240–360MB 时偏高，建议目标 150–200MB；'
     '若超过 200MB，降低码率至 3500–4000 kbps 重新导出。')

h1('六、拍摄注意事项与常见问题')
h2('6.1 设备状态确认')
body('正式拍摄前，确保固件为最新版本，所有功能已按当前代码逻辑工作。')
body('提前清理 Gallery，只保留 1–2 个注册身份，避免演示时匹配到旧身份。')
body('确保 PIR 传感器固定稳定，避免误触发；如误触发频繁，可在拍摄时临时遮挡或调整角度。')

h2('6.2 语音指令成功率')
body('拍摄环境保持安静，避免背景噪音干扰 ASR。')
body('指令发音清晰、语速适中，距离麦克风 0.5–1 米。')
body('若某条指令识别失败，可立即重说一遍，后期剪掉失败片段；不要在正式片段中长时间沉默。')

h2('6.3 画面与字幕')
body('LCD 屏幕在强光下可能反光，适当调整角度或使用偏振膜。')
body('关键 UI 变化（如“正在学习中”“疑似离座”）可用剪映添加局部放大特效或字幕说明。')
body('片头片尾添加作品名称、团队名称字幕，提升专业感。')

h2('6.4 上传前最终检查')
body('用 MediaInfo 打开成片，确认：Format=MPEG-4，Codec=AVC/H.264，Audio=AAC，Duration 5–10min，Size≤200MB。')
body('在电脑、手机、平板上各播放一遍，确认画面、声音、字幕正常。')
body('检查是否有隐私泄露画面（如真实人脸、家庭环境敏感信息），必要时打码。')
body('确认无误后再上传，上传前保留一份本地备份。')

h1('七、备用方案')
body('若现场网络不稳定导致云端功能演示失败，可采取以下备用方案：')
body('方案一：提前录制好云端问答片段，现场只拍触发动作，后期拼接。')
body('方案二：将 VLM 回答改为本地预制回答，仅演示唤醒与指令识别流程。')
body('方案三：缩短云端演示时长，重点展示端侧 Re-ID、计时、离座提醒等离线功能。')

h1('八、拍摄流程执行表')
body('以下为可打印的执行 checklist，拍摄当天逐项打勾：')
script('□ 设备烧录最新固件，功能自测通过')
script('□ Gallery 清空，仅保留演示身份')
script('□ Wi-Fi、云端服务、ASR/VLM/TTS 已联通')
script('□ 三脚架、补光灯、充电宝、插线板就位')
script('□ 提示卡、换装外套、桌椅道具就位')
script('□ OBS/相机参数已按 1080p/30fps/H.264/AAC 设置')
script('□ 试拍 30 秒并用 MediaInfo 检查合格')
script('□ 完整彩排 1 遍，确认各环节触发正常')
script('□ 正式分段拍摄')
script('□ 剪辑、加字幕、导出')
script('□ MediaInfo 最终检查 + 多设备试播')
script('□ 上传前备份')

para('', 12)
body('本方案可直接打印作为拍摄执行手册。祝拍摄顺利！')

OUT = '智学Guardian_视频拍摄方案.docx'
doc.save(OUT)
print('saved:', OUT)
