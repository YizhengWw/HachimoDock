# T113 硬件宠物屏接入

## v1 目标

ZQBoard T113 只作为硬件显示终端，不运行桌面 `pet-claw`。板端 `pet-screen-agent` 连接到和电脑端 bridge 相同的 MQTT broker，订阅：

```text
desk/<desktopDeviceId>/state/+
desk/<desktopDeviceId>/speech/text
```

AP 直连只作为 fallback：板子热点固定使用 `192.168.44.1`，板端 broker 使用 `mqtt://192.168.44.1:1883`。Pet Manager 写入 secondary sink 配置后，`claw-on-desk` bridge 会把同一份 state/speech payload 副发布到板子 broker。

## Pet Manager 配置

当前测试阶段的 `Pet Manager` 主流程只做一件事：把本机电脑的 `desktop_device_id` 写成板子正在订阅的 ID，例如 `linux-pet-01`。这样电脑端 bridge 会直接发布到：

```text
desk/<desktop_device_id>/state/+
desk/<desktop_device_id>/speech/text
```

板子只要已经订阅同一个 `desktop_device_id`，就会跟随这台电脑的状态，不需要在 Pet Manager 里再做 `Linux 下发` 或 `remote-cli-binding`。

`Pet Manager` 当前保留：

- 读取并保存本机 `desktop_device_id`
- 保存本机 Pet Manager 的 MQTT broker 配置；优先级为进程环境 > Pet Manager 本机配置 > 相邻 `pet-claw/.env`
- 快捷写入测试 ID，默认 `linux-pet-01`
- 展示当前电脑会发布的 `state/+` 和 `speech/text` topic
- 展示当前 MQTT broker、本机 `pet-claw` bridge 运行状态、板端在线状态和板端订阅 topic
- 可直接启动 Pet Manager 内置 bridge；它在本机监听 `127.0.0.1:23333/state`，兼容现有 hook POST 协议，并按当前 `desktop_device_id` 发布 MQTT
- 如果已有外部 `pet-claw` bridge 在运行，Pet Manager 会识别并复用；不再要求普通用户本机安装 `pet-claw`
- 使用进程环境、Pet Manager 本机配置或 `pet-claw/.env` 里的 `MQTT_URL` 发布一次板端跟随测试，可切换 `idle / working / speaking / error / waiting_user`；历史的 `thinking` / `tool_running` 会归一为 `working`
- AP fallback 副发布器配置仍由底层 Tauri 命令保留，但不作为当前主流程入口展示

硬件屏绑定保存到：

```text
~/.openclaw/pet-screens.json
```

示例：

```json
{
  "screens": [
    {
      "boardDeviceId": "linux-pet-01",
      "name": "Desk Pet Screen",
      "host": "192.168.1.42",
      "mode": "sta",
      "brokerUrl": "mqtt://broker.local:1883",
      "desktopDeviceId": "skyler-mbp",
      "mqttNamespace": "desk"
    }
  ],
  "activeBoardDeviceId": "linux-pet-01"
}
```

## board-runtime 自动发现

板端最小包启动后会 retained 发布：

```text
openclaw/pet-screen/<boardDeviceId>/hello
openclaw/pet-screen/<boardDeviceId>/availability
```

Pet Manager 会从 hello payload 读取：

```json
{
  "boardDeviceId": "linux-pet-01",
  "localDeviceId": "linux-pet-01",
  "desktopDeviceId": "skyler-mbp",
  "mqttNamespace": "desk",
  "brokerUrl": "mqtt://broker.local:1883",
  "sourceStateTopic": "desk/skyler-mbp/state/+",
  "host": "192.168.1.42",
  "uiUrl": "http://192.168.1.42:18789"
}
```

如果 hello 没有显式 `desktopDeviceId`，Pet Manager 会从 `sourceStateTopic` 里反推 `desk/<desktopDeviceId>/state/+`；再缺失时才回退到本机当前 `pet-claw/device-config.json` 里的 device id。

Pet Manager 底层仍使用 `boardDeviceId` 做记录去重和 retained 清理范围识别。当前测试主流程不展示 discovery 候选清单，因为 retained hello 和本地历史都可能是旧数据。

兼容说明：旧 C 版 `board-server` 压缩包里仍会在 hello payload 或历史本地配置里出现 `screenId`。Pet Manager 不把它作为新概念展示或写出，只在读取旧包数据时把它当作 `boardDeviceId` 的 legacy fallback，这样旧 C 包可以继续和当前测试流打通。

## AP fallback

Pet Manager 的“启用 AP 副发布器”不会覆盖原有 `MQTT_URL`。它只写入：

```text
~/.openclaw/pet-screen-secondary-sink.json
```

`claw-on-desk` bridge 重启或热重载后读取该文件，并额外连接板子 broker 做副发布。正常 STA 模式下不需要启用这个配置。
