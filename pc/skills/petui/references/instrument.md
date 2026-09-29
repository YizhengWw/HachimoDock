# 本地木鱼音效工具

- 使用 v4 运行时、kind=tool、唯一 source `percussion.wooden-fish`。
- 须先检查设备 `widgetInstrument=p4-instrument-v1`；没有该能力必须升级固件，不能退回假播放。
- `instrument` 对象恰好包含 source、effect_volume、ambience_volume；两项音量均为 0–100 整数，建议 70/18。
- vars={}，states 至少一个初始状态。不要声明 data、media、scene、game、pages，tick=[]。
- transitions 恰好为一个 `{"on":"instrument.strike","from":"*"}`；buttons 恰好一个同名动作，默认 button.sw1.short_press。全局退出与实时对话优先，不声明 SW3 或 hold。
- 每次输入一次敲击，原生回弹与声波；最多四个叠加余音，不排队补敲。没有计数、自动敲击或胜负。
- 声音是本地木质敲击合成音和原创轻柔伴奏，不依赖网络、PC 持续传音频或用户歌曲列表。
- 默认敲击音已定为 A「清脆短促」；55 ms 落槌后音画接触，随后衰减回弹。不增加音色选择功能。
- 离开组件淡出；录音/实时对话/音频流抢占立即清除音效。再次回到组件重新淡入，旧随身听不会自动恢复。
- 暖木色原生表面的木鱼、木槌图层由固件提供，包内 assets/.keep 即可。连按从木槌当前位置衔接，不瞬间回到起点。不要把原生能力解释为任意第三方高分辨率场景能力。
- PC 首次试听须由用户点击，切走/关闭预览停止；设备打开组件即开始低音量伴奏。浏览器与硬件调度不同，不能以浏览器试听代替实机验收。
- 执行 validate_generated_widget.py、smoke_test_widget_game.py 后原子发布。烟测检查声明与可达反馈模型；产品 firmware/tests/test_instrument_core.py 编译实际 C DSP 验证音量、重叠、回弹和停止。报告两者范围，不宣称已验证实体扬声器。
