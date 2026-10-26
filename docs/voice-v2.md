# 语音四阶段落地与验收

本次实现依照小智的独立编码、解码、输出任务和半双工资源管理；本地自动判停与后台有序提交、播放反馈是本项目增加的协作协议。没有直接照搬小智的栈大小：本工程 SILK 曾超过8KB，编码保持32KB，解码24KB。

## 1. 扬声器和本地诊断

ES8311 speaker open/reopen/enable后设置音量65并解除静音；RTC恢复时也恢复。PA GPIO53和本板I2S1引脚保持本板配置。DMA显式为6×160samples=60ms；播放结束写入整环静音并等70ms尾音，之后才关PA。

menuconfig → AI voice turn and playback → `VOICE_AI_SELF_TEST` 默认关闭。启用后启动阶段先播放约720ms PCM 400Hz低幅音，停300ms，再播放约720ms本地Opus编码/解码音；测试任务32KB栈，不连接云。两次都需实际听到，日志ESP_OK只证明API成功，不证明接线或喇叭有声。不能在线调用测试来抢占麦克风/RTC。

## 2. 一句话自动提交

唤醒回调只通知控制任务；单一采集源把60ms PCM放到编码队列，编码任务分3×20ms运行ESP-SR WebRTC VAD。连续120ms语音确认开始，保留最多300ms本轮前置音频；默认尾静音800ms（60ms判决粒度，实际840ms），最长单轮15秒，无讲话5秒退出。VAD模式3、时限、音量都在menuconfig可调，不使用简单幅度阈值替代语音检测。

编码队列由4增至16帧（960ms），放PSRAM；满丢旧，不阻塞8KB采集任务。最终提交边界包含末尾静音帧，边界之后的未拼满PCM不属于该轮，不需要拼入提交。上行音控分队列，audio_end带发送屏障，不能超越尾音；等待回复不再编码/发送环境声。

## 3. 下行限速与独立播放

先部署同目录旁 `xn_ai_server` 更新。新版固件用查询参数协商v2，后台仅为硬件连接选择client_interrupt，浏览器原配置保留。Opus输入/输出均设16k mono16，建议输出20ms。包边界由Coze提供，后台读取Opus TOC按实际时长发送；若Coze返回非原始Opus/无有效chat_id，需根据实际上游事件检查，不能把OGG/任意分片当裸Opus播放。

后台120秒/2MiB有界缓存吸收TTS突发；下发同时受24包/600ms播放反馈额度限制。压缩队列96包作故障保护，独立24KB解码任务→4项PCM队列→独立6KB输出任务，控制8KB任务不执行逐包播放。首音频后预留120ms启动缓冲吸收网络抖动，后台发送时钟避免小定时误差累积拖慢播放。PCM缓冲放PSRAM。坏包单独丢弃，正常流控不依赖丢包追赶。

## 4. 连续对话与恢复

回复全部排空自动开启新回合，5秒无人讲话则退回唤醒；提交后5秒日志提示等待，10秒无首音频取消。播放15秒无本地进度取消；后台10秒无播放反馈取消。每轮turnId、每次采集generation隔离旧包，Coze chat_id隔离取消后的旧回复。控制锁串行外部停止；RTC停止返回前在途编码、解码、写入均退出，不允许AI恢复任务在通话中抢回硬件。

## 验收与定位

- 自测两声均可听见；纯PCM无声先查功放/喇叭/音量/I2S；PCM有声、Opus无声查本地codec日志。
- 唤醒说一句后看`speech_start`→`speech_end/commit`→网关`Ordered audio commit sent`；尾静音后约840ms判停，编码队列不拥塞时上行随后应归零。
- 看后台`device_audio_control`、`local_turn_commit`、`conversation.chat.created`、`first_audio`，与设备`first_audio after commit`、`first_write`、`playback_drained`对应，分别衡量本地判停、传输、模型、解码/播放延迟。
- 正常长回复各层drop应为0；人工暂停反馈验证后台停止下发且10秒报错，不灌爆设备。混合丢弃计数已拆为capture、encode/TX、RX、decode、write；网关另有TX丢弃/发送失败/RX丢弃计数。
- 连续至少20轮；测试唤醒后不讲话、短停顿继续讲话、超过15秒、首包超时、回复中断Wi-Fi、播放中来电/挂断、取消后旧包晚到；不能串音、死锁或丢失后续唤醒。

主机检查：`node tests/voice-ai-task-boundaries.test.mjs`（源码约束）、`sh tests/run-voice-turn.sh`（Linux cc/python3，实际C策略测试）；后台`npm run typecheck`、`npm test`。实机验收和真实Coze响应时延需烧录后测，本地构建不能替代。

## 本次本地验证（2026-10-26）

- ESP-IDF 6.1 / ESP32-P4 完整构建和分区大小检查通过，应用分区剩余约45%。
- 后台 TypeScript 检查通过；Coze与下行流控14项测试通过，包括669包突发保序完整发送、24包/600ms额度、错误ACK、时钟抖动、取消、旧chat隔离、PCM样本边界。
- Linux GCC实际执行同一份C判停策略通过；固件任务边界源码检查、本地配网页面测试通过。
- 后台全量31项：30通过、1失败。失败是`test/api.test.ts:73`请求`/api/mini/devices/unknown/claim`期望400、实际404；本次没有修改该领取接口或该测试。
- 未部署后台、未烧录、未执行板上发声/真实Coze/20轮/断网与通话抢占实测。日志和测试入口已提供；不能将模拟流控通过当作实际扬声器出声证明。
