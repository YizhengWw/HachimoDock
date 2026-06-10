# Hachimiao
A desktop pet that reacts to your Claude Code sessions in real-time — thinking, typing,  juggling, sleeping, and more.
<div style="max-width:980px;margin:0 auto;padding:8px 0 40px 0;color:#1d1d1f;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',Arial,sans-serif;line-height:1.75">
<div style="position:relative;overflow:hidden;border-radius:34px;background:linear-gradient(135deg,#f8fbff 0%,#eef5ff 52%,#fff6ee 100%);border:1px solid rgba(226,233,243,0.95);box-shadow:0 24px 70px rgba(25,40,70,0.12);padding:26px;margin:16px 0 16px 0">
  <h1 style="margin:0 0 8px 0;color:#121826;font-size:42px;line-height:1.12;font-weight:850;letter-spacing:0">Hachimiao</h1>
  <p style="margin:0 0 18px 0;color:#4d5a6c;font-size:16px;line-height:1.75">Agent 的专属小屏：桌面常驻、实体陪伴，把 CLI Agent 的状态与回应变成可见、可触碰的桌面宠物。</p>
  <img src="https://image.lceda.cn/oshwhub/pullImage/cecb8d736582436b939c4fa78db3eebc.png" alt="image_01.png" style="width:100%;display:block;border-radius:18px" />
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:10px 12px;margin:0 0 18px 0;box-shadow:0 8px 22px rgba(25,40,70,0.05)">
  <span style="display:inline-block;color:#7a8494;font-size:13px;font-weight:700;margin-right:10px">目录</span><a href="#1-项目简介" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">1. 项目简介</a><a href="#2-核心亮点" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">2. 核心亮点</a><a href="#3-软件开发" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">3. 软件开发</a><a href="#4-硬件复刻" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">4. 硬件复刻</a><a href="#5-使用指南" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">5. 使用指南</a><a href="#6-附录与维护" style="display:inline-block;padding:7px 11px;border-radius:999px;background:#fff;border:1px solid #e4ebf5;color:#2367d8;text-decoration:none;font-size:13px;font-weight:700;margin-right:8px">6. 附录与维护</a>
</div>
<span id="1-项目简介"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">1. 项目简介</h2>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;align-items:start;margin:12px 0 24px 0"><div><figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/b40469d726074c91aac4db4526b3bc00.jpeg" alt="image_02.jpeg" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure></div><div><figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/875e49ef6bd54d28b4a01e32f5549e96.png" alt="image_03.png" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure></div></div>
<p style="margin:0 0 10px 0;color:#4d5a6c;font-size:15px;line-height:1.75">Hachimiao - Agent的专属小屏（桌面常驻、实体陪伴）</p>
<div style="display:grid;grid-template-columns:1fr;gap:10px;margin-top:10px">
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px;box-shadow:0 8px 22px rgba(25,40,70,0.045)">
  <div style="width:34px;height:4px;border-radius:99px;background:#3673fe;margin-bottom:14px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:18px;line-height:1.4;font-weight:760">项目定位</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">将PC上运行的各类Agent（Codex、Claude Code、OpenClaw 等），具象为工位上的一只可见、可触碰的桌面宠物 - Agent 在思考，它跟着思考；Agent 在调工具，它开始工作；任务完成它庆祝，任务报错它发愁，它不是“虚拟”助理，而是有状态、有回应、有存在感的 AI搭子。核心特点：</p>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px;box-shadow:0 8px 22px rgba(25,40,70,0.045)">
  <div style="width:34px;height:4px;border-radius:99px;background:#1fb88f;margin-bottom:14px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:18px;line-height:1.4;font-weight:760">抬头可见</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">“抬头可见”把 Agent 的状态、进度、待决策事项、任务结果翻译成前台可见的宠物行为，用户只需看一眼设备上的表情、动作和短标签，就能知道 Agent 正在做什么</p>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px;box-shadow:0 8px 22px rgba(25,40,70,0.045)">
  <div style="width:34px;height:4px;border-radius:99px;background:#ff8a2a;margin-bottom:14px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:18px;line-height:1.4;font-weight:760">开口即用</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">“开口即用”不需要先打开电脑窗口、找IM聊天框，开口就能说话/下命令，办公、写作、开发、查资料、记录想法</p>
</div>
</div>
</div>
<span id="2-核心亮点"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">2. 核心亮点</h2>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:20px;padding:18px;margin-bottom:14px;box-shadow:0 10px 26px rgba(25,40,70,0.05)">
  <div style="width:34px;height:4px;border-radius:99px;background:#3673fe;margin-bottom:12px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:19px;line-height:1.38;font-weight:780">Agent 状态跟随</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">Agent状态跟随：桌面端与硬件屏宠物状态实时同步，用表情、动作、颜色和短标签表达 Agent 状态；在Agent状态变化时提供字幕、轻提醒和提示信息。</p>
  <div style="margin-top:12px">
  <img src="https://image.lceda.cn/oshwhub/pullImage/369ca3da496442379a84795c2db3d7a8.gif" alt="image_04.gif" style="width:100%;display:block;border-radius:18px" />
  <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:10px;margin-top:10px">
    <img src="https://image.lceda.cn/oshwhub/pullImage/be759dc4ce584dcc8f036ee129c6ce4c.png" alt="image_05.png" style="width:100%;display:block;border-radius:18px" />
    <img src="https://image.lceda.cn/oshwhub/pullImage/e3a5b26f6d1e4c238972d06631d4f9b3.png" alt="image_06.png" style="width:100%;display:block;border-radius:18px" />
  </div>
</div>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:20px;padding:18px;margin-bottom:14px;box-shadow:0 10px 26px rgba(25,40,70,0.05)">
  <div style="width:34px;height:4px;border-radius:99px;background:#1fb88f;margin-bottom:12px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:19px;line-height:1.38;font-weight:780">空闲与触摸反馈</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">在Agent空闲时，pet会自己玩耍，呈现多种待机状态；你也可以和pet玩耍，触摸屏幕，pet会很开心。</p>
  <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:12px;align-items:start"><img src="https://image.lceda.cn/oshwhub/pullImage/975c1fe0a7ac4af39cec57e6bb342ba8.gif" alt="image_07.gif" style="width:100%;display:block;border-radius:18px" /></div>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:20px;padding:18px;margin-bottom:14px;box-shadow:0 10px 26px rgba(25,40,70,0.05)">
  <div style="width:34px;height:4px;border-radius:99px;background:#ff8a2a;margin-bottom:12px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:19px;line-height:1.38;font-weight:780">Agent 语音交互</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">Agent语音交互：支持通过设备的麦克风与Agent的任意session进行对话交互，抛弃打字的繁琐</p>
  
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:20px;padding:18px;margin-bottom:14px;box-shadow:0 10px 26px rgba(25,40,70,0.05)">
  <div style="width:34px;height:4px;border-radius:99px;background:#7c5cff;margin-bottom:12px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:19px;line-height:1.38;font-weight:780">自定义形象</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">自定义形象：内置西高地小狗形象（16 个状态动画），并可通过上传自己的宠物照片、头像或原创角色生成新形象，同时支持从本机Codex pet库或pet社区导入其它网友生成的pet形象。</p>
  <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px;margin-top:12px;align-items:start"><img src="https://image.lceda.cn/oshwhub/pullImage/3d12378c3ebc486ab50717fc32aae77b.png" alt="image_08.png" style="width:100%;display:block;border-radius:18px" /><img src="https://image.lceda.cn/oshwhub/pullImage/312b1ff537e444c684b47646b0ab3937.png" alt="image_09.png" style="width:100%;display:block;border-radius:18px" /></div>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:20px;padding:18px;margin-bottom:14px;box-shadow:0 10px 26px rgba(25,40,70,0.05)">
  <div style="width:34px;height:4px;border-radius:99px;background:#e05d9f;margin-bottom:12px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:19px;line-height:1.38;font-weight:780">自定义组件</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">自定义组件：内置摸鱼倒计时、番茄钟、喝水提醒、Token 消耗四个组件；支持用自然语言一句话生成新组件并下发到设备。</p>
  <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px;margin-top:12px;align-items:start"><img src="https://image.lceda.cn/oshwhub/pullImage/5b70a63790c54cd79487572b29c73836.jpeg" alt="image_10.jpeg" style="width:100%;display:block;border-radius:18px" /><img src="https://image.lceda.cn/oshwhub/pullImage/171866a2265246bd998bc25cca0bf3e8.png" alt="image_11.png" style="width:100%;display:block;border-radius:18px" /></div>
</div>
</div>
<span id="3-软件开发"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">3. 软件开发</h2>
<p style="margin:0 0 18px 0;color:#4d5a6c;font-size:16px;line-height:1.95">开发环境、构建与本地运行说明请查看项目开发文档。</p>
<a href="https://git.n.xiaomi.com/prodx/claw-pet-manager/-/blob/main/docs/developer-setup_zh_Hans.md" style="display:inline-block;padding:13px 24px;background:#3673fe;color:#fff;text-decoration:none;border-radius:999px;font-size:15px;font-weight:750;box-shadow:0 10px 24px rgba(54,115,254,0.25)">查看 GitHub / 开发文档</a>
<p style="margin:12px 0 0 0;color:#7a8494;font-size:13px;line-height:1.7;word-break:break-all">https://git.n.xiaomi.com/prodx/claw-pet-manager/-/blob/main/docs/developer-setup_zh_Hans.md</p>
</div>
<span id="4-硬件复刻"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">4. 硬件复刻</h2>
<h3 style="margin:0 0 14px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">4.1 硬件BOM</h3>
<figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/e5f382ac43a14288bf8fc12c305f498d.png" alt="image_12.png" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure>
<div style="overflow-x:auto;border-radius:20px;border:1px solid #dfe7f1;box-shadow:0 12px 32px rgba(20,35,55,0.08);margin:18px 0 24px 0">
<table style="width:100%;min-width:760px;border-collapse:collapse;background:#fff"><thead><tr><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">类别</th><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">模块/器件</th><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">位号/接口</th><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">器件编号</th><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">数量</th><th style="padding:14px 12px;text-align:left;background:#172033;color:#fff;font-size:14px;font-weight:700;border-bottom:1px solid #26364f">备注</th></tr></thead><tbody><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">开发板</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">Raspberry Pi Zero 2 WH/</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">插入 U1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">需预焊 40pin 排针版本</td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">SD卡</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">microSD 卡</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">TF 卡槽</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">16GB以上，A1/A2 级别优先</td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">屏幕</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">2.8 寸 240x320 SPI TFT 触摸屏</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">插入 U2</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">以11PIN 带 XPT2046 触摸版本为例；8PIN 无触摸版本作为同尺寸备选；选择焊接排针、ILI9341版本</td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">旋钮</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">可按下旋钮模块 / EC11 编码器模块</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">接 H2</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">麦克风</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">INMP441麦克风模块</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">接 H1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">喇叭</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">2011小腔体喇叭，1.25端子，带双面胶</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">接 CN7</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">2011 8欧1瓦1.25P</td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">辅料</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">杜邦线</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">麦克风、旋钮</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">15</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">10cm即可、母对母</td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">自攻螺丝 M2 * 8 mm</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">屏幕固定</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">4</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">用于固定屏幕</td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">自攻螺丝 M2 * 5 mm</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">结构固定</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">2</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">用于固定其他构件</td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">Micro-USB公转Type-C母转接线；</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">内部开发板引出</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">10cm；MicroUSB公上弯转Type-C母直【mic2-tpc1】</td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">Type-C转Type-A转接线</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">设备连接到电脑</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">参考购买链接</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">USB 2.0的即可，4芯及以上的，不能只是充电的</td></tr><tr style="background:#f7f9fc"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">3D打印外壳</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">3D 打印 / CNC 外壳</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">整机结构</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">嘉立创3D打印</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">1</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">外壳、后盖、内部固定支架等</td></tr><tr style="background:#ffffff"><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">定制PCB底版</td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top"></td><td style="padding:13px 12px;color:#435064;font-size:13px;line-height:1.65;border-bottom:1px solid #e9eef5;vertical-align:top">底板BOM见下↓</td></tr></tbody></table>
</div>
<h3 style="margin:28px 0 14px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">4.2 结构件与装配</h3>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;align-items:start;margin:12px 0 24px 0"><div><figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/2b90752cf9fb49a594d7518e799746a6.jpeg" alt="image_13.jpeg" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure></div><div><figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/8fa5bdc86b40485f8e8010a9ed18b27e.jpeg" alt="image_14.jpeg" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure></div></div>
<figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/b084c52c00414207afa83c7701971da4.jpeg" alt="image_15.jpeg" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure>
<h3 style="margin:24px 0 12px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">4.4 常见 Q&A</h3><div style="display:grid;grid-template-columns:1fr;gap:10px"><div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px">
  <strong style="display:block;color:#172033;font-size:15px;line-height:1.6;margin-bottom:6px">设备首次启动后没有被管理端发现怎么办？</strong>
  <span style="color:#5d6878;font-size:14px;line-height:1.75">先确认设备已接通电源并进入等待连接/配网状态；如果走 USB 直连，请重新扫描串口；如果走 Wi-Fi，请确认电脑与设备在同一网络或按 AP 配网流程重新绑定。</span>
</div><div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px">
  <strong style="display:block;color:#172033;font-size:15px;line-height:1.6;margin-bottom:6px">为什么 Agent 状态没有同步到屏幕？</strong>
  <span style="color:#5d6878;font-size:14px;line-height:1.75">检查 Pet Manager 是否已经检测到本机 CLI Agent，并确认当前渠道已绑定形象；同时确认 USB serial 或 MQTT 通道在线。</span>
</div><div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px">
  <strong style="display:block;color:#172033;font-size:15px;line-height:1.6;margin-bottom:6px">自定义形象或组件下发失败怎么办？</strong>
  <span style="color:#5d6878;font-size:14px;line-height:1.75">优先确认设备在线；离线设备需要 USB 直连或上线后再安装。组件包也可以通过拖拽 .clawpkg 目录或 zip 手动加入。</span>
</div></div>
<div style="background:linear-gradient(135deg,#fff1f1 0%,#f7f3ff 52%,#eef4ff 100%);border-radius:24px;padding:16px;margin:14px 0 18px 0;border:1px solid rgba(255,255,255,0.7);box-shadow:0 14px 38px rgba(30,45,80,0.07)">
  <h3 style="margin:0 0 10px 0;color:#20242c;font-size:23px;line-height:1.3;font-weight:820">4.3 板子工艺信息</h3>
  <div style="background:rgba(255,255,255,0.72);border:1px solid rgba(255,255,255,0.72);border-radius:22px;padding:16px;box-shadow:0 12px 32px rgba(40,50,80,0.08)">
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:center;margin-bottom:12px">
      <div style="height:190px;border-radius:18px;background:rgba(255,255,255,0.62);display:flex;align-items:center;justify-content:center;overflow:hidden">
        <img src="https://image.lceda.cn/oshwhub/pullImage/d4dc0c05bc91416b92506099d638765b.png" alt="image_16.png" style="width:100%;height:100%;object-fit:contain;display:block" />
      </div>
      <div style="height:190px;border-radius:18px;background:rgba(255,255,255,0.62);display:flex;align-items:center;justify-content:center;overflow:hidden">
        <img src="https://image.lceda.cn/oshwhub/pullImage/1ac73804a9e04ab0b9ac023703b70fa3.png" alt="image_17.png" style="width:100%;height:100%;object-fit:contain;display:block" />
      </div>
    </div>
    <div style="display:flex;align-items:center;gap:10px;margin-bottom:8px">
      <span style="width:10px;height:10px;border-radius:50%;background:#147cff;display:inline-block"></span>
      <h3 style="margin:0;color:#20242c;font-size:19px;line-height:1.3;font-weight:760">Hachimiao 定制 PCB</h3>
    </div>
    <div style="display:flex;justify-content:space-between;gap:18px;border-bottom:1px solid rgba(60,70,90,0.10);padding:8px 0;color:#8a909a;font-size:14px;line-height:1.45">
  <span>关键厚度</span>
  <strong style="color:#ff3b30;font-size:15px;text-align:right">1.6 mm</strong>
</div><div style="display:flex;justify-content:space-between;gap:18px;border-bottom:1px solid rgba(60,70,90,0.10);padding:8px 0;color:#8a909a;font-size:14px;line-height:1.45">
  <span>板子层数</span>
  <strong style="color:#414852;font-size:15px;text-align:right">双层板</strong>
</div><div style="display:flex;justify-content:space-between;gap:18px;border-bottom:1px solid rgba(60,70,90,0.10);padding:8px 0;color:#8a909a;font-size:14px;line-height:1.45">
  <span>尺寸</span>
  <strong style="color:#414852;font-size:15px;text-align:right">71 mm * 30.5 mm</strong>
</div><div style="display:flex;justify-content:space-between;gap:18px;border-bottom:1px solid rgba(60,70,90,0.10);padding:8px 0;color:#8a909a;font-size:14px;line-height:1.45">
  <span>焊接</span>
  <strong style="color:#414852;font-size:15px;text-align:right">音频处理部分（功放、麦克风）可能需要加热台；其余器件使用烙铁即可。若不需要音频交互，可考虑打裸板自行焊接排针排母，不影响产品核心功能。</strong>
</div>
  </div>
</div>
</div>
<span id="5-使用指南"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">5. 使用指南</h2>
<p style="margin:0 0 10px 0;color:#4d5a6c;font-size:15px;line-height:1.75">完整的软件安装、构建、配置、二次开发和故障排查，请以 GitHub / 开发文档为准。</p>
<figure style="margin:18px 0 24px 0;break-inside:avoid">
  <img src="https://image.lceda.cn/oshwhub/pullImage/17aeced03fb54724b4cc552eaea332c9.png" alt="image_18.png" style="width:100%;display:block;border-radius:24px;box-shadow:0 16px 44px rgba(20,35,55,0.13);border:1px solid rgba(230,236,245,0.95);background:#fff" />
</figure>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px;margin-top:12px">
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px;box-shadow:0 8px 22px rgba(25,40,70,0.045)">
  <div style="width:34px;height:4px;border-radius:99px;background:#3673fe;margin-bottom:14px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:18px;line-height:1.4;font-weight:760">管理端（PC）</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">负责设备绑定、Agent 检测与跟随、形象管理、组件中心、按钮配置、语音入口和连接诊断。</p>
</div>
<div style="background:#f7f9fc;border:1px solid #e4ebf5;border-radius:18px;padding:16px;box-shadow:0 8px 22px rgba(25,40,70,0.045)">
  <div style="width:34px;height:4px;border-radius:99px;background:#1fb88f;margin-bottom:14px"></div>
  <h3 style="margin:0 0 8px 0;color:#172033;font-size:18px;line-height:1.4;font-weight:760">展示端（设备）</h3>
  <p style="margin:0;color:#536071;font-size:15px;line-height:1.75">常驻显示宠物状态与负一屏组件，处理触摸、旋钮、按钮输入，通过 USB serial 或 MQTT 接收桌面端下发的状态流。</p>
</div>
</div>
</div>
<span id="6-附录与维护"></span><div style="background:#fff;border:1px solid #e6edf6;border-radius:24px;padding:22px;margin-bottom:22px;box-shadow:0 12px 34px rgba(25,40,70,0.06)"><h2 style="margin:0 0 14px 0;color:#172033;font-size:25px;line-height:1.3;font-weight:800">6. 附录与维护</h2>
<h3 style="margin:0 0 14px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">6.1 Contributing</h3>
<p style="margin:0 0 10px 0;color:#4d5a6c;font-size:15px;line-height:1.75">欢迎提交 issue、discussion 和 pull request。建议贡献方向：</p>
<ul style="margin:0 0 18px 0;padding-left:22px;color:#4d5a6c;font-size:15px;line-height:1.9"><li style="margin-bottom:8px">修复 Pet Manager 或板端运行时问题。</li><li style="margin-bottom:8px">适配新的硬件屏、主控板、结构件或外壳形态。</li><li style="margin-bottom:8px">创作宠物资源、动画素材和字幕样式。</li><li style="margin-bottom:8px">开发新的 .clawpkg 负一屏组件。</li><li style="margin-bottom:8px">补充装配教程、烧录说明和故障排查。</li><li style="margin-bottom:8px">改进 Agent 状态协议和第三方 Agent 接入。</li></ul>
<h3 style="margin:24px 0 12px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">6.2 License</h3>
<p style="margin:0 0 10px 0;color:#4d5a6c;font-size:15px;line-height:1.75">本项目建议按内容类型分别声明许可证，不建议只使用一个笼统协议。</p>
<ul style="margin:0 0 18px 0;padding-left:22px;color:#4d5a6c;font-size:15px;line-height:1.9"><li style="margin-bottom:8px">软件代码 License：[待补充，例如 Apache-2.0 / MIT]</li><li style="margin-bottom:8px">硬件设计 License：[待补充，例如 CERN-OHL-S-2.0 / CERN-OHL-P-2.0]</li><li style="margin-bottom:8px">3D 结构件 License：[待补充，例如 CC BY-SA 4.0]</li><li style="margin-bottom:8px">官方宠物素材 License：[待补充]</li><li style="margin-bottom:8px">第三方资源声明：THIRD_PARTY_NOTICES.md</li></ul>
<h3 style="margin:24px 0 12px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">6.3 Security Issues</h3>
<p style="margin:0 0 10px 0;color:#4d5a6c;font-size:15px;line-height:1.75">如果发现安全问题，请不要直接公开敏感细节。请通过项目维护者提供的安全反馈渠道联系，我们会尽快确认和处理。</p>
<h3 style="margin:24px 0 12px 0;color:#172033;font-size:21px;line-height:1.45;font-weight:760">6.4 Contact</h3>
<ul style="margin:0 0 18px 0;padding-left:22px;color:#4d5a6c;font-size:15px;line-height:1.9"><li style="margin-bottom:8px">Maintainer：</li><li style="margin-bottom:8px">Collaborator：</li><li style="margin-bottom:8px">GitHub：[待补充]</li><li style="margin-bottom:8px">嘉立创项目页：[待补充]</li><li style="margin-bottom:8px">社区 / 交流群：[待补充]</li><li style="margin-bottom:8px">安全反馈邮箱：[待补充]</li></ul>
<div style="background:linear-gradient(135deg,#f8fbff 0%,#eef5ff 55%,#fff6ee 100%);border:1px solid #e4ebf5;border-radius:26px;padding:24px;margin-top:20px;text-align:center;box-shadow:0 12px 34px rgba(25,40,70,0.06)">
  <div style="margin:0 0 8px 0;color:#172033;font-size:24px;line-height:1.35;font-weight:800">更多信息</div>
  <p style="margin:0 0 18px 0;color:#5d6878;font-size:14px;line-height:1.8">项目主页、交流群、模型/资源下载入口可在这里集中放置，发布前替换为你的真实链接与二维码。</p>
  <div style="display:flex;justify-content:center;gap:28px;flex-wrap:wrap;margin-bottom:18px">
    <div style="text-align:center;min-width:150px">
  <div style="width:138px;height:138px;margin:0 auto 10px auto;border-radius:22px;background:#fff;border:1px dashed #cfd8e6;display:flex;align-items:center;justify-content:center;color:#9aa6b6;font-size:13px;line-height:1.5">二维码<br />待补充</div>
  <strong style="display:block;color:#172033;font-size:14px;line-height:1.5">交流群</strong>
  <span style="display:block;color:#7a8494;font-size:12px;line-height:1.6">交流复刻与玩法</span>
</div>
    <div style="text-align:center;min-width:150px">
  <div style="width:138px;height:138px;margin:0 auto 10px auto;border-radius:22px;background:#fff;border:1px dashed #cfd8e6;display:flex;align-items:center;justify-content:center;color:#9aa6b6;font-size:13px;line-height:1.5">二维码<br />待补充</div>
  <strong style="display:block;color:#172033;font-size:14px;line-height:1.5">项目主页 / 资源</strong>
  <span style="display:block;color:#7a8494;font-size:12px;line-height:1.6">模型、附件与其它平台入口</span>
</div>
  </div>
  <div style="display:flex;justify-content:center;gap:10px;flex-wrap:wrap">
    <a href="#" style="display:inline-block;padding:10px 18px;background:#3673fe;color:#fff;text-decoration:none;border-radius:999px;font-size:14px;font-weight:750">GitHub / 代码主页待补充</a>
    <a href="#" style="display:inline-block;padding:10px 18px;background:#172033;color:#fff;text-decoration:none;border-radius:999px;font-size:14px;font-weight:750">其它平台主页待补充</a>
  </div>
</div>
</div></div>
