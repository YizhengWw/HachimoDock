# WorkBuddy Agent 支持

### Windows 空界面树恢复（0.1.92）

若只暴露少量窗口节点而没有可写输入框，先对 WorkBuddy 自身窗口执行一轮辅助功能请求，再刷新 UIA 根节点。采用 Windows 的 [AccessibleObjectFromWindow](https://learn.microsoft.com/en-us/windows/win32/api/oleacc/nf-oleacc-accessibleobjectfromwindow) 和 [Chromium 渲染窗口的 MSAA 客户端检测机制](https://raw.githubusercontent.com/chromium/chromium/main/content/browser/renderer_host/legacy_render_widget_host_win.cc)。请求仅限已核对进程归属的主窗口及最多四个渲染子窗口，每次录音最多一轮；不更改系统设置、修改启动参数或重启 WorkBuddy。

收到辅助功能应答并不代表有可写输入框，更不代表已经输入文字。只有重新取得并验证所属窗口、编辑器身份、可写性和草稿内容后才继续。超时仍由外层 10 秒看门狗终止；诊断增加请求数、MSAA 应答数、渲染窗口数及客户端检测应答数。该路径已做模拟流程与编译验证，尚未在 Windows WorkBuddy 真机验收。

WorkBuddy 是独立渠道，不与 CodeBuddy、Claude 或 Codex 混用。支持本地安装识别、独立形象绑定、任务状态与气泡跟随、已收到事件的会话列表、当前可见输入框语音草稿，以及组件生成 Skill 安装。使用现有 PC → USB 状态协议，无需修改固件。

## 使用

1. 安装并启动 WorkBuddy 一次，完成它自身的初始化。
2. 在 Pet Manager 的「Agent 与形象」重新扫描，选择 WorkBuddy 并启用跟随。
3. Pet Manager 在 WorkBuddy 的 `settings.json` 合并状态 Hook，保留其他配置；首次更改前创建 `settings.json.pet-manager-backup`。按 WorkBuddy 自身要求允许 Hook，并在新会话/新任务中验证状态更新；必要时重启 WorkBuddy。
4. 在 WorkBuddy 执行任务，设备随工作、等待确认、完成与错误事件改变形象状态。权限操作仍在 WorkBuddy 确认。
5. 语音输入前，在 WorkBuddy 打开目标对话并保留一个主窗口。长按设备语音键说话，松开后将完整结果一次追加为草稿；再次录音追加文字，确认键发送。识别中间结果仅在 Pet Manager 显示，不反复重写编辑器；最终写入读回成功后才显示草稿待确认。macOS 需要辅助功能权限，写入期间保持目标对话在前台；Windows 两个应用应使用相同权限级别。对话或输入框变化时停止写入，不自动改投其他会话。
6. 在组件中心点击「创建组件」，安装组件生成 Skill 后，在 WorkBuddy 中使用 `petui`。安装位置为其配置目录下的 `skills/petui`；不会自动执行生成任务。

默认使用用户目录下 `.workbuddy`；自定义部署可用 `WORKBUDDY_CONFIG_DIR`。只检测标准 macOS/Windows 安装路径及 `WORKBUDDY_INSTALL_DIR`，不扫描登录凭据，不使用 CodeBuddy 的配置目录回退。配置损坏或 `disableAllHooks` 开启时不覆盖或擅自启用。

## 边界与隐私

- Windows 的输入定位等待窗口恢复并重新读取 UIA 根节点，优先验证当前聚焦输入框；无有效焦点时再查 Control/Raw 树、Chromium 渲染子窗口及限时区域探测。只接受属于目标窗口的可写编辑器，Document 控件还须具备 textbox/editor 语义；保留 Runtime ID 和草稿准确读回，无焦点时多个候选不按分数猜测。失败诊断含节点/可读/可写数量或固定的超时阶段，不含输入文字或账号数据。0.1.90 及之前的 Windows 启动超时可能误标为 ChatGPT（Codex），不代表实际路由到 Codex；0.1.91 已按真实 Agent 修正文案。跨进程 UIA 调用卡住时，仍由 10 秒看门狗终止，不无限等待。

- macOS 首次读取输入框为零时，在应用置前后进行一次辅助功能树恢复；首次初始化失败不会被永久缓存。恢复后仍要求唯一、稳定的输入框，多个输入框不自动挑选。若仍失败，请点击目标对话的输入框后重新录音；此类定位错误发生在写入之前，不会发送消息。

- 按键语音走 PC 原生可见输入框通道，不走后台 Session Bus 注入。当前不提供自动创建/恢复桌面会话或历史数据库读取；请手动打开目标对话。设备自身的实时对话不受影响。
- macOS 优先直接设置输入框选区；富文本编辑器忽略多行/表情选区时，仅向已锁定的 WorkBuddy 进程发送全选。确认原文被稳定选中后，再向同一进程投递粘贴；不发送系统级全选或粘贴快捷键，不回退全局键盘事件。读回最终草稿后恢复剪贴板；已聚焦窗口不反复激活。两端最终写入均等待结果，不自动重试不确定的 WorkBuddy 写入；若提示未确认，请先检查输入框，避免重复录入。
- 「诊断与测试」为 WorkBuddy 提供「只写入草稿」入口，复用正式按键语音的写入和读回流程；请先停止语音监听，测试不调用 ASR、不发消息、不触发 Agent 任务。
- 会话列表只包含本次 Bridge 收到的事件，以通用名称显示；不扫描历史数据库，Bridge 重启后等待新事件重新建立列表。
- Hook 向经过身份检查的本机 Bridge 转发 Agent ID、会话 ID、状态以及最多 320 字的当前提问/回复摘要，供已连接设备气泡显示。优先采用 Stop 的 `last_assistant_message`；缺少时只读该 Hook 指定、位于 WorkBuddy `projects` 下且文件名匹配当前会话的 transcript 尾部（最多 256 KiB），校验本轮 generation ID（如有），不跨用户轮次复用旧回复。
- 不转发工具参数/输出、思考过程、完整 transcript、凭据字段或 transcript 路径；摘要隐藏代码块、常见密钥格式和本地用户路径。摘要会出现在设备屏幕上，请留意周围可见性；自动脱敏不能识别所有自由文本敏感信息。Hook 输出仍为非决策型 `{}`，不产生 allow/deny 决定。
- 只在用户选择/启用 WorkBuddy 渠道时安装 Hook。停用渠道后不跟随其状态；如需完整卸载，可在 WorkBuddy 设置中删除命令含 `workbuddy-pet-manager-hook.js` 的 Hook，保留其他 Hook。不要用旧备份覆盖后续新设置。

接口参考：[腾讯官方 Hooks 文档](https://www.codebuddy.ai/docs/cli/hooks)。WorkBuddy 的独立路径通过本机已安装客户端核对；适配代码为本项目实现，未复制或分发 WorkBuddy 客户端代码。

验证涵盖配置合并、备份、幂等、格式损坏保护、事件隐私、权限不自动批准、回复摘要、受限 transcript 读取及本地 Session Bus HTTP 回归。0.1.88 已通过应用诊断入口实际调用 macOS 正式原生写入路径，验证多行/表情旧草稿的连续追加、准确读回且未观察到截图蒙层；测试草稿已清理、未发送消息。本机真实回复摘要也已由运行中的 Bridge 接收。Windows 原生输入、实体麦克风按键全链路与设备屏幕视觉仍需对应环境验收。
