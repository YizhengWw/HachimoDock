(function attachPetClawMainViewModels(root) {
    const TASK_STATUS_META = {
        running: { label: "进行中", tone: "running" },
        pending: { label: "待处理", tone: "pending" },
        completed: { label: "已完成", tone: "completed" },
        failed: { label: "异常", tone: "failed" }
    };

    const AGENT_VIEW_MODELS = {
        companion: {
            homeStatus: {
                summary: "今天有点饿了",
                satiety: 62,
                spendTokens: 12043,
                spendCny: 3.2,
                appSpendList: [
                    {
                        name: "Claude Code",
                        badge: "CC",
                        processCount: 3,
                        spendTokens: 4320,
                        spendCny: 1.38,
                        accent: "#7da8ff",
                        mock: true
                    },
                    {
                        name: "Cursor",
                        badge: "CU",
                        processCount: 5,
                        spendTokens: 3891,
                        spendCny: 1.02,
                        accent: "#ff9d72",
                        mock: true
                    },
                    {
                        name: "ChatGPT",
                        badge: "CG",
                        processCount: 2,
                        spendTokens: 3832,
                        spendCny: 0.8,
                        accent: "#69d7b3",
                        mock: true
                    }
                ],
                rewardProgress: 78,
                idleTray: "它刚完成一轮回复，现在想吃点东西。",
                helperText: "左右滑打开状态页。"
            },
            details: [
                {
                    id: "detail-first-chat",
                    title: "小鱼干徽章",
                    caption: "它第一次完整接住你的消息，并稳稳把回应播报出来。",
                    tag: "纪念品",
                    type: "纪念品",
                    mark: "🐟",
                    state: "owned",
                    method: "完成第一次连续对话"
                },
                {
                    id: "detail-scarf",
                    title: "围巾装扮",
                    caption: "天气转凉的时候，它最喜欢披着这条围巾值班。",
                    tag: "装扮",
                    type: "装扮",
                    mark: "🧣",
                    state: "equipped",
                    method: "连续投喂 3 次"
                },
                {
                    id: "detail-wave-loop",
                    title: "打招呼动作",
                    caption: "新生成的 5 秒动作片段已经在显影，稍后可以加入循环。",
                    tag: "动作",
                    type: "动作",
                    mark: "👋",
                    state: "rendering",
                    method: "生图完成后自动拼进 5 秒动作循环"
                },
                {
                    id: "detail-night-watch",
                    title: "夜班饭碗",
                    caption: "深夜连续工作后留下的纪念品，代表它陪你熬过第一轮高频任务。",
                    tag: "成就",
                    type: "成就",
                    mark: "🍚",
                    state: "locked",
                    progress: 78,
                    method: "累计消耗达到 ¥10 解锁"
                },
                {
                    id: "detail-shell",
                    title: "贝壳纪念品",
                    caption: "属于陪伴模式的安静收集物，没有额外功能，只用来记住一次稳定陪伴。",
                    tag: "纪念品",
                    type: "纪念品",
                    mark: "🐚",
                    state: "owned",
                    method: "完成 10 次正常播报"
                },
                {
                    id: "detail-can",
                    title: "罐头道具",
                    caption: "投喂时最常出现的小道具，用来让它短时间恢复精神。",
                    tag: "道具",
                    type: "道具",
                    mark: "🥫",
                    state: "owned",
                    method: "累计投喂 5 次"
                }
            ],
            taskPerspective: {
                sourceLabel: "水獭管家视角",
                emptyMessage: "当前没有需要照看的事项。",
                emptyHint: "你可以回主页继续互动，或稍后刷新任务。",
                sourceMap: {
                    workstream: "照看清单",
                    "工作流": "照看清单",
                    "当前会话": "陪伴对话"
                },
                statusLabels: {
                    running: "照看中",
                    pending: "待安顿",
                    completed: "已安顿",
                    failed: "需处理"
                }
            }
        },
        ops: {
            homeStatus: {
                summary: "稿子写了一半，有点累了",
                satiety: 54,
                spendTokens: 18320,
                spendCny: 4.8,
                appSpendList: [
                    {
                        name: "Cursor",
                        badge: "CU",
                        processCount: 5,
                        spendTokens: 6890,
                        spendCny: 1.92,
                        accent: "#ff9d72",
                        mock: true
                    },
                    {
                        name: "Claude Code",
                        badge: "CC",
                        processCount: 3,
                        spendTokens: 5240,
                        spendCny: 1.36,
                        accent: "#7da8ff",
                        mock: true
                    },
                    {
                        name: "ChatGPT",
                        badge: "CG",
                        processCount: 2,
                        spendTokens: 6190,
                        spendCny: 1.52,
                        accent: "#69d7b3",
                        mock: true
                    }
                ],
                rewardProgress: 42,
                idleTray: "它正在翻上一轮草稿，等你继续派活。",
                helperText: "左右滑打开状态页。"
            },
            details: [
                {
                    id: "detail-terrier-draft",
                    title: "梗犬首篇草稿",
                    caption: "围绕桌宠日常整理出的第一版内容，已经能拿来继续扩写。",
                    tag: "纪念品",
                    type: "纪念品",
                    mark: "📝",
                    state: "owned",
                    method: "完成一次完整内容草稿"
                },
                {
                    id: "detail-terrier-angle",
                    title: "选题角度库",
                    caption: "把一次普通互动拆成多个内容切角，准备给不同平台复用。",
                    tag: "动作",
                    type: "动作",
                    mark: "💡",
                    state: "rendering",
                    method: "新的视频动作正在等待拼接"
                },
                {
                    id: "detail-terrier-shoot",
                    title: "拍摄参考板",
                    caption: "从桌宠动作里挑出适合做封面和短视频封帧的画面节奏。",
                    tag: "装扮",
                    type: "装扮",
                    mark: "📷",
                    state: "equipped",
                    method: "完成第一次封面挑选"
                },
                {
                    id: "detail-terrier-post",
                    title: "发稿节点",
                    caption: "把任务同步、字幕播报和角色切换整合成一条可以发的更新说明。",
                    tag: "成就",
                    type: "成就",
                    mark: "📡",
                    state: "locked",
                    progress: 42,
                    method: "累计消耗达到 ¥12 解锁"
                }
            ],
            taskPerspective: {
                sourceLabel: "梗犬助手视角",
                emptyMessage: "当前没有新的选题或排期。",
                emptyHint: "可以先回主页切内容灵感，或等待下一条任务。",
                sourceMap: {
                    workstream: "内容排期",
                    "工作流": "内容排期",
                    "当前会话": "灵感池"
                },
                statusLabels: {
                    running: "赶稿中",
                    pending: "选题中",
                    completed: "已发稿",
                    failed: "卡稿中"
                }
            }
        }
    };

    const DETAIL_STATE_LABELS = {
        owned: "已获得",
        equipped: "已装备",
        rendering: "显影中",
        locked: "未解锁"
    };

    const PET_CLAW_MAIN_VIEW_MODELS = Object.freeze({
        TASK_STATUS_META,
        AGENT_VIEW_MODELS,
        DETAIL_STATE_LABELS
    });

    root.PET_CLAW_MAIN_VIEW_MODELS = PET_CLAW_MAIN_VIEW_MODELS;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = PET_CLAW_MAIN_VIEW_MODELS;
    }
})(typeof window !== "undefined" ? window : globalThis);
