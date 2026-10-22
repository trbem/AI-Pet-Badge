# 音色复刻与试听

老师提供的 API Key 若能访问百炼官方语音接口，可以通过本地程序使用，不要求操作者另注册主账号。API Key 不等同于控制台登录账号；地域、业务空间与模型授权由提供方确认。仅凭 Key 文本无法判断这些信息，也不能据一次鉴权失败认定 Key 无效。

本项目使用 CosyVoice 创建定制音色，得到音色 ID，再提交文字生成声音。参考样本建议 10～20 秒，单人清晰说话，避免音乐、杂音、其他人声和唱歌。创建和合成必须使用同一个目标模型。

## 配置

在开发目录打开 PowerShell。已安装的桌面 .venv 包含所需依赖；换电脑后先执行：

```powershell
Set-Location D:\ai_pet_badge_dev
.\.venv\Scripts\python.exe -m pip install -r backend\requirements.txt
```

将 backend/.env.example 复制为 backend/.env，在本地编辑 DASHSCOPE_API_KEY。不要覆盖已有 .env，不要把密钥发到聊天或提交仓库。DASHSCOPE_REGION 默认 cn-beijing，TTS_MODEL 默认 cosyvoice-v3.5-flash；这是待验证的初始配置，并不代表已识别老师的 Key。

如果老师提供业务空间 ID，填写 DASHSCOPE_WORKSPACE_ID，工具会使用该地域的专属域名；否则使用官方既有 DashScope 域名。工具只访问北京或新加坡官方 API，不会把 Key 发往第三方接口，不跟随 API 的重定向。系统环境变量优先于 .env。

## 先查询，再创建

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py check
```

check 只查询第一页音色列表，不上传音频、不创建或删除音色、不合成语音。成功仅证明该地域的列表接口可访问，不证明目标模型的创建或合成权限。失败只报告 HTTP 状态与错误码，不打印密钥、完整响应或音频数据。401 可能是 Key、地域不匹配或学校中转凭证；403 需提供方检查权限与服务状态；网络错误不能证明 Key 无效。若老师确认是学校/第三方接口，需要其接口地址与语音 API 文档，仅提供聊天接口可能无法复刻音色。

工具会在联网前识别 sk-sp- 套餐 Key 格式，提示更换可调用 CosyVoice 的百炼通用 API Key。官方说明 Coding Plan/Token Plan 套餐凭证与通用 Key 不同，不能据同为 sk- 前缀就混用。工具不会为音色复刻改用编程套餐端点。参考：[通用与套餐 API Key](https://help.aliyun.com/en/model-studio/get-api-key)、[Coding Plan 凭证与端点说明](https://help.aliyun.com/zh/model-studio/coding-plan-faq)。

若本机代理绕过列表包含 IPv6 单主机 CIDR（如 ::1/128），工具会仅在当前进程内转换为等价单主机表示，兼容 HTTPX 0.28；不修改系统代理，保留代理和证书配置。

确认是新加坡 Key 时，可临时指定地域（模型也应按老师提供的信息修改）：

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py --region ap-southeast-1 check
```

## 创建音色

将声音样本放在本地，例如 D:\AI Pet Badge\voice\sample.wav。工具支持 WAV、MP3、M4A，通过 HTTPS 直接提交 Base64 音频，无需搭建公开文件服务器。文件不超过 10MB；WAV 本地检查 16-bit PCM、至少 16kHz、1～2 声道、5～60 秒。MP3/M4A 的时长与声音质量仍需人工或服务端检查。

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py clone 'D:\AI Pet Badge\voice\sample.wav'
```

clone 会向云端创建音色，不自动重试。创建成功立即将音色 ID、模型、地域及端点保存到本地 artifacts/voice/created-*.json，然后查询状态。就绪后更新 .env 中的 TTS_VOICE_ID、TTS_MODEL、DASHSCOPE_REGION，保留其他配置。请求超时不等于创建失败，先运行以下命令按项目默认前缀查询音色 ID（若创建时修改了前缀，查询也应指定相同 --prefix），再决定是否重建：

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py list
```

若已有 ID 但仍在处理，可运行：

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py status --voice-id '创建返回的音色ID'
```

## 合成试听

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py speak --text '你好呀，今天也要开开心心的。'
```

工具先验证音色绑定模型和就绪状态，再提交一次合成，下载为 artifacts/voice/preview.wav。合成按服务计费，工具不自动反复试听；如输出已存在，使用新的 --output 文件名。下载音频不携带 API Key。试听确认后再集成板端语音链路，不改变当前已验收的动画固件。

## 当前验证范围

2026-10-22：用户已填写本地 API Key。修复代理绕过列表的 IPv6 单主机 CIDR 兼容问题后，真实查询北京、新加坡官方音色列表接口，均返回 HTTP 401、InvalidApiKey。仅检查格式确认凭证符合套餐专用 Key 前缀，未输出完整密钥；没有配置覆盖或空白字符问题。需要提供方确认并更换可访问 CosyVoice 的通用 API Key，未创建云端音色、未进行合成试听。

9 项离线测试通过：错误不回显密钥、API 重定向不转发密钥、创建超时不重复提交、绑定模型不匹配时阻止合成、音频下载与重定向不带密钥、已有试听文件时阻止合成、本地 WAV 编码、IPv6 单主机 CIDR 代理兼容，以及套餐 Key 联网前识别。实际音色创建与试听仍需通用 Key 和声音样本验证。

官方参考：[API Key 配置](https://help.aliyun.com/zh/model-studio/get-api-key)、[音色复刻指南](https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide)、[音色复刻 HTTP API](https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api)、[HTTP 语音合成](https://help.aliyun.com/zh/model-studio/non-realtime-tts-user-guide)。
