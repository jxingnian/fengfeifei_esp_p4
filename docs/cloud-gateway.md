# 设备后台连接与配对

## 操作流程

1. 设备通过 `http://192.168.4.1` 配置 Wi-Fi 和后台地址，默认后台地址为 `https://iot.xingnian.vip`。
2. 设备联网后连接 `/ws/device/pair`，发送设备 UID、名称、固件版本和能力；后台只登记临时设备，不生成设备配对码。
3. 后台管理员在管理后台选择客户和可选后台用户，生成 6 位配对码。后台页面和小程序页面都显示这个后台生成的配对码。
4. 用户打开设备本地 `/cloud` 页面，输入后台显示的配对码并点击“提交配对码”。设备通过临时 WebSocket 发送 `{ "type": "pair", "code": "配对码" }`。
5. 服务端按配对码绑定的客户和后台用户创建设备，返回长期授权。设备保存授权后连接正式 `/ws/device`。

配对码有效期 5 分钟，只能使用一次。配对码不包含设备 UID，设备不能通过自报字段改变客户归属。小程序只显示后台生成的配对码，不创建或提交配对码。

## 本地接口

- `GET /api/cloud/status`：返回连接状态、设备 UID、设备 ID 和错误信息，不返回后台配对码或长期授权码。
- `POST /api/cloud/configure`：正文 `{ "serverUrl": "https://iot.xingnian.vip" }`，保存后台地址。
- `POST /api/cloud/pair`：正文 `{ "pairingCode": "123456" }`，提交设备网页输入的后台配对码。
- `POST /api/cloud/forget`：清除本地长期授权，后台设备记录仍需由管理员撤销。

所有本地写接口要求 `Content-Type: application/json` 和 `X-Cloud-Control: 1`，仅供可信局域网使用，不能映射到公网。
