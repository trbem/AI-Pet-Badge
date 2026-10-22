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

默认 DASHSCOPE_SERVICE=standard；如果老师提供业务空间 ID，填写 DASHSCOPE_WORKSPACE_ID，工具会使用该地域的专属域名；否则使用官方既有 DashScope 域名。Token Plan 需显式指定 token-plan 模式，使用北京套餐专属域名。工具不把 Key 发往第三方接口，不跟随 API 的重定向。系统环境变量优先于 .env。

## 先查询，再创建

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py check
```

check 只查询第一页音色列表，不上传音频、不创建或删除音色、不合成语音。成功仅证明该地域的列表接口可访问，不证明目标模型的创建或合成权限。失败只报告 HTTP 状态与错误码，不打印密钥、完整响应或音频数据。401 可能是 Key、地域不匹配或学校中转凭证；403 需提供方检查权限与服务状态；网络错误不能证明 Key 无效。若老师确认是学校/第三方接口，需要其接口地址与语音 API 文档，仅提供聊天接口可能无法复刻音色。

通用接口模式会在联网前识别套餐 Key 并提示入口不匹配，不据此前缀断言整个套餐没有语音能力。Token Plan 的 OpenAI 文本地址为 https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1，Anthropic 文本地址为 https://token-plan.cn-beijing.maas.aliyuncs.com/apps/anthropic；两者是协议不同的文本端点，不应直接作为音色复刻地址。

官方文档说明 Token Plan 支持在 AI 工具扩展内调用独立的语音合成端点，路径为 /api/v1/services/audio/tts/SpeechSynthesizer。语音合成支持不等于音色注册接口必然可用。工具中的 token-plan 模式用于 Agent 交互素材验证，不能据此认定已打通勋章常驻后端；正式应用后端仍按适用的通用 API 方案接入。参考：[Token Plan 文本接入与使用范围](https://help.aliyun.com/zh/model-studio/more-tools)、[Token Plan 多模态与语音合成](https://help.aliyun.com/zh/model-studio/token-plan-multimodal-gen)。

在当前交互中，按同一 Token Plan 域名尝试通用音色注册路径的只读查询：

```powershell
.\.venv\Scripts\python.exe tools\bailian_voice.py --service token-plan check
```

该路径映射属于验证尝试，不是已由 Token Plan 文档确认的音色注册能力。当前实际返回 404 InvalidParameter、Model not exist，需要老师或平台确认音色注册接口、模型与套餐权限，或提供可访问 CosyVoice 的通用 Key。工具不会自动改用其他注册模型，不重复创建音色。

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

clone 会向云端创建音色，不自动重试。创建成功立即将音色 ID、模型、地域、接口模式及端点保存到本地 artifacts/voice/created-*.json，然后查询状态。就绪后更新 .env 中的 TTS_VOICE_ID、TTS_MODEL、DASHSCOPE_REGION、DASHSCOPE_SERVICE，保留其他配置。请求超时不等于创建失败，先运行以下命令按项目默认前缀查询音色 ID（若创建时修改了前缀，查询也应指定相同 --prefix），再决定是否重建：

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

2026-10-22：用户已填写本地 API Key，随后提供北京 Token Plan 的两种文本协议地址。修复代理绕过列表的 IPv6 单主机 CIDR 兼容问题后，北京、新加坡通用音色列表接口均返回 HTTP 401、InvalidApiKey；此结果不能证明 Token Plan Key 失效。按匹配的 Token Plan 域名尝试 voice-enrollment/list_voice 返回 HTTP 404、InvalidParameter，脱敏检查错误消息为 Model not exist。音色注册未跑通，需要提供方进一步确认支持范围；没有创建云端音色、没有进行合成试听。完整密钥未输出，也未修改本地 Key。

11 项离线测试通过：错误不回显密钥、API 重定向不转发密钥、创建超时不重复提交、绑定模型不匹配时阻止合成、音频下载与重定向不带密钥、已有试听文件时阻止合成、本地 WAV 编码、IPv6 单主机 CIDR 代理兼容、套餐 Key 与通用入口混用识别、Token Plan 独立查询路由，以及模型不存在时不误报 Key 失效。实际音色创建与试听仍需可用的音色注册接口和声音样本验证。

官方参考：[API Key 配置](https://help.aliyun.com/zh/model-studio/get-api-key)、[音色复刻指南](https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide)、[音色复刻 HTTP API](https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api)、[HTTP 语音合成](https://help.aliyun.com/zh/model-studio/non-realtime-tts-user-guide)。
