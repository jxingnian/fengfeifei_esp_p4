# 设备状态机组件使用文档

## 概述

设备状态机组件用于管理设备的状态转换，支持待机、AI对话、语音通话、视频通话等状态。组件本身只负责状态管理和切换逻辑，具体业务由应用层实现。

## 文件说明

- `device_state_machine.h` - 头文件，定义API接口
- `device_state_machine.c` - 实现文件，状态机核心逻辑
- `device_state_machine_example.c` - 使用示例（可选编译）

## 状态定义

| 状态 | 说明 |
|------|------|
| `DEVICE_STATE_IDLE` | 待机状态 |
| `DEVICE_STATE_AI_CONVERSATION` | AI对话状态 |
| `DEVICE_STATE_VOICE_CALL` | 语音通话状态 |
| `DEVICE_STATE_VIDEO_CALL` | 视频通话状态 |

## 事件定义

| 事件 | 说明 | 触发场景 |
|------|------|---------|
| `EVENT_WAKE_WORD_DETECTED` | 检测到唤醒词 | 语音唤醒检测到关键词 |
| `EVENT_INCOMING_VOICE_CALL` | 语音来电 | 收到语音通话请求 |
| `EVENT_INCOMING_VIDEO_CALL` | 视频来电 | 收到视频通话请求 |
| `EVENT_CALL_ENDED` | 通话结束 | 对方挂断或网络断开 |
| `EVENT_CONVERSATION_ENDED` | 对话结束 | AI对话正常结束 |
| `EVENT_CONVERSATION_TIMEOUT` | 对话超时 | 长时间无交互 |
| `EVENT_USER_HANGUP` | 用户挂断 | 用户主动结束通话 |

## 状态转换规则

```
待机状态 (IDLE)
  ├─ 唤醒词 → AI对话状态
  ├─ 语音来电 → 语音通话状态
  └─ 视频来电 → 视频通话状态

AI对话状态 (AI_CONVERSATION)
  ├─ 对话结束 → 待机状态
  ├─ 对话超时 → 待机状态
  ├─ 语音来电 → 语音通话状态（先停止对话）
  └─ 视频来电 → 视频通话状态（先停止对话）

语音通话状态 (VOICE_CALL)
  ├─ 通话结束 → 待机状态
  └─ 用户挂断 → 待机状态

视频通话状态 (VIDEO_CALL)
  ├─ 通话结束 → 待机状态
  └─ 用户挂断 → 待机状态
```

## 快速开始

### 1. 实现回调函数

```c
static void on_start_conversation_callback(void)
{
    // 启动AI对话：初始化语音识别、TTS、连接AI服务
}

static void on_stop_conversation_callback(void)
{
    // 停止AI对话：立即停止识别、停止TTS、取消AI请求
    // 注意：必须快速返回，不能阻塞！
}

static void on_start_voice_call_callback(void)
{
    // 接听语音通话：初始化音频通道
}

static void on_stop_voice_call_callback(void)
{
    // 结束语音通话：释放音频资源
}

static void on_start_video_call_callback(void)
{
    // 接听视频通话：初始化音视频通道
}

static void on_stop_video_call_callback(void)
{
    // 结束视频通话：释放音视频资源
}

static void on_state_changed_callback(device_state_t old_state, device_state_t new_state)
{
    // 状态变化通知：更新UI、记录日志、控制LED等
}
```

### 2. 初始化状态机

```c
void app_init(void)
{
    device_state_callbacks_t callbacks = {
        .on_start_conversation = on_start_conversation_callback,
        .on_stop_conversation = on_stop_conversation_callback,
        .on_start_voice_call = on_start_voice_call_callback,
        .on_stop_voice_call = on_stop_voice_call_callback,
        .on_start_video_call = on_start_video_call_callback,
        .on_stop_video_call = on_stop_video_call_callback,
        .on_state_changed = on_state_changed_callback,
    };

    if (!device_state_machine_init(&callbacks)) {
        ESP_LOGE(TAG, "状态机初始化失败");
    }
}
```

### 3. 发送事件

```c
// 检测到唤醒词
void on_wake_word_detected(void)
{
    device_state_send_event(EVENT_WAKE_WORD_DETECTED);
}

// 收到来电
void on_incoming_call(bool is_video)
{
    if (is_video) {
        device_state_send_event(EVENT_INCOMING_VIDEO_CALL);
    } else {
        device_state_send_event(EVENT_INCOMING_VOICE_CALL);
    }
}

// 对话结束
void on_ai_conversation_done(void)
{
    device_state_send_event(EVENT_CONVERSATION_ENDED);
}

// 通话结束
void on_call_ended(void)
{
    device_state_send_event(EVENT_CALL_ENDED);
}
```

### 4. 查询状态

```c
// 获取当前状态
device_state_t current = device_state_get_current();
ESP_LOGI(TAG, "当前状态: %s", device_state_get_name(current));

// 检查能否转换到某个状态
if (device_state_can_transit(DEVICE_STATE_VOICE_CALL)) {
    ESP_LOGI(TAG, "可以转换到语音通话状态");
}
```

## 典型使用场景

### 场景1：正常AI对话流程

```
1. 待机状态，用户说"你好小飞"
2. 唤醒词检测 → 发送 EVENT_WAKE_WORD_DETECTED
3. 状态机切换到 AI_CONVERSATION，调用 on_start_conversation
4. 用户与AI对话...
5. 对话结束 → 发送 EVENT_CONVERSATION_ENDED
6. 状态机切换到 IDLE，调用 on_stop_conversation
```

### 场景2：对话中来电（高优先级打断）

```
1. AI对话进行中（AI_CONVERSATION状态）
2. 收到语音来电 → 发送 EVENT_INCOMING_VOICE_CALL
3. 状态机先调用 on_stop_conversation（停止对话）
4. 然后调用 on_start_voice_call（接听电话）
5. 切换到 VOICE_CALL 状态
6. 通话结束 → 发送 EVENT_CALL_ENDED
7. 状态机切换到 IDLE，调用 on_stop_voice_call
```

### 场景3：通话状态拒绝唤醒词

```
1. 通话进行中（VOICE_CALL状态）
2. 用户说"你好小飞"（检测到唤醒词）
3. 发送 EVENT_WAKE_WORD_DETECTED
4. 状态机查找转换表，没有匹配项
5. 忽略该事件，继续保持通话状态
```

## 注意事项

### 1. 回调函数实现要点

- **快速返回**：回调函数不能阻塞，必须快速执行完毕
- **异步处理**：如果有耗时操作，应该启动任务异步处理
- **资源清理**：stop类回调必须立即清理资源，避免冲突

### 2. 线程安全

- 状态转换使用互斥锁保护，线程安全
- 可以在任何任务中调用 `device_state_send_event()`
- 事件通过队列处理，避免阻塞

### 3. 优先级处理

状态优先级：**通话 > 对话 > 待机**

- 通话状态下不响应唤醒词
- 对话状态下来电会立即切换
- 待机状态响应所有事件

### 4. 错误处理

```c
// 事件发送失败的处理
if (!device_state_send_event(EVENT_WAKE_WORD_DETECTED)) {
    ESP_LOGW(TAG, "事件发送失败，可能是队列满或状态机未初始化");
}
```

## API参考

### 初始化与反初始化

```c
bool device_state_machine_init(const device_state_callbacks_t *callbacks);
void device_state_machine_deinit(void);
```

### 事件发送

```c
bool device_state_send_event(device_event_t event);
```

### 状态查询

```c
device_state_t device_state_get_current(void);
bool device_state_can_transit(device_state_t target_state);
```

### 调试辅助

```c
const char* device_state_get_name(device_state_t state);
const char* device_event_get_name(device_event_t event);
```

## 调试建议

1. **启用日志**：组件会输出详细的状态转换日志
2. **监控状态变化**：在 `on_state_changed` 中记录状态历史
3. **检查非法转换**：如果事件被忽略，会输出警告日志
4. **使用示例代码**：参考 `device_state_machine_example.c` 进行测试

## 扩展建议

如需添加新状态或新事件：

1. 在头文件中添加枚举定义
2. 在 `device_state_machine.c` 的转换表中添加规则
3. 实现对应的动作函数
4. 更新 `get_name` 函数以支持调试

## 示例编译

如果要编译示例代码，在 CMakeLists.txt 中添加：

```cmake
set(COMPONENT_SRCS 
    "device_state_machine.c"
    "device_state_machine_example.c"  # 添加这一行
    # ...其他源文件
)
```
