# AI Pet Badge · 爱莉希雅桌面陪伴原型

基于微雪 ESP32-S3-Touch-LCD-1.85B 的课程项目。当前固件实现淡粉背景的角色动画、待机轮播与触摸切换；中文语音对话将在后续阶段接入。

## 硬件与显示

- 360 × 360 圆屏，16 MiB Flash，8 MiB PSRAM。
- 行走、跳跃、坐下三个动作，视频去绿幕后统一画布和比例，目标 15 FPS。
- 素材暂存 Flash，逐帧 zlib 压缩，使用两个 PSRAM 图像缓冲；不依赖 TF 卡。
- 点击切换动作；USB 诊断命令为 STATUS、NEXT、TONE。TONE 只用于手动扬声器测试。

## 构建与烧录

使用 ESP-IDF **5.5.3**。组件版本在 main/idf_component.yml 和 dependencies.lock 中固定。板级驱动来自 Waveshare 官方仓库提交 139e6db584f3737fcfc6a958ee83b79fb69d317c，保留各文件的上游许可。

工程路径需无空格。Windows SDK 入口见 tools/Enter-Idf.ps1，安装目录不同时调整该脚本。

```powershell
py -3.11 -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r tools\requirements-assets.txt
.\tools\Build-Badge.ps1 -AssetSource 'D:\AI Pet Badge\donz' -Flash -Port COM9
```

素材目录中需要 walk.mp4、jump.mp4、sit.mp4；素材包已生成时可以省略 -AssetSource。COM9 是本次实机识别结果，其他机器请先确认端口。脚本按本次构建产生的 flash_args 烧录。

```powershell
. .\tools\Enter-Idf.ps1
python tools\board_console.py --port COM9 --reset --seconds 15 --output artifacts\boot.log
python tools\board_console.py --port COM9 --command STATUS --seconds 3 --output artifacts\status.log
```

## 目录

- firmware/ai_pet_badge：自研应用与板级组件。
- tools：素材转换、构建与 USB 诊断工具。
- backend：语音后端环境准备，业务链路待接入。
- docs/MVP_PLAN.md：已确认范围、协作方式与阶段安排。

原始角色视频、声音样本、密钥、生成的动画包和构建输出保留在本地；源码仓库包含转换流程与配置样例。
