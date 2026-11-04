# Track C 最终冻结回执

本线不再扩展核心源码。未运行 Cargo、PIO、HIL、设备安装、OTA 或全套 pytest。

## 最终本线范围

- `firmware/main/pet_p4_component_media.c/.h`：3 bank / 8 reader token、无锁 deferred release、一个固定 pending ticket 的 prepare/commit/abort。prepare/abort 不改变旧可见代；保留 old borrow；commit 不读写文件或分配内存。
- `firmware/main/pet_p4_miniapp_ext.inc/.h`：两阶段 adapter；B 的四个共享调用端已使用同名 API。A 修改的 UTC/parked/checkpoint 区域保留，未回退。`pet_p4_component_media_clients.h` 提供 snapshot / sample lease / renderer retire 接口。
- `firmware/main/pet_p4_component_ext.c/.h`：仅已解析 subscriber / 精确声明 alarm action 是 internal；新增纯有界绑定覆盖函数。`miniapp.c` 仅在 parse_save 后新增该校验调用，避开 A clock/checkpoint 与 B activation 区域。
- `pc/skills/petui/scripts/capability_ext.py`：同一声明作用域；`validate_generated_widget.py` 的按钮覆盖仅减去该合法 internal 集合，普通动作/能力校验保留。
- `pc/src-tauri/src/component_capability_ext.rs`：同一作用域及真实完整包回归。此前 sample/clip/big/FFmpeg 的边界修复保留。
- `pc/skills/petui/contracts/internal-actions-v1.json`：14 组共享正负例，含 subscriber 与 emit-only 区分、精确 alarm target、错误/撤销声明、恶意名字、普通动作仍拒绝、既有 touch 路径仍允许。
- `firmware/tests/component_internal_actions_test.py`、`component_media_owned_test.py`、`p4_capability_ext_test.c`：真实 C parser、完整 Python 包、事务失败保留/abort/commit、并发 borrow / deferred / worker retire 回归。
- `firmware/tests/fixtures/track_c_private/manifest.json`：7 包全部 offline-valid，deviceEvidence/installed/published 均为 false，无虚假物理 event/ring 按键。资源均复用既有素材；无依赖升级。

## 本线最终证据

```
python -m pytest firmware/tests/component_internal_actions_test.py firmware/tests/component_media_owned_test.py -q
17 passed in 1.73s
```

日志 `/tmp/cext-scope-transaction-tests.log`。该结果独立于 A 的 45 项、B 的 9 项；不相加冒称完整 HIL。

Rust 只做 rustfmt parser 检查，未跑 Cargo。统一 Rust 回归请包含：

- `component_capability_ext::tests::internal_action_scope_vectors_validate_complete_packages`
- `component_capability_ext::tests::full_host_packages_admit_referenced_samples_and_clips_only`
- `component_capability_ext::tests::native_process_output_timeout_and_failure_are_bounded`

UTC 与 hidden checkpoint 已由 A 接管完成；全局正式构建与 HIL 由主代理在源码冻结后重新绑定。旧 preflight 或 1200-query 结果不计作本轮新能力验收。
