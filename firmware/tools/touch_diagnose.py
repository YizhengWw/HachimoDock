"""Classify a device's touch bring-up from a diagnostics/status payload. Offline only.

Input: a JSON file (or '-' for stdin) holding either the raw `{"topic":"diagnostics/status",
"payload":{...}}` line, the payload itself, or anything that contains a `touchHealth` object
(`pet_p4_diagnostics.c` fills it from `pet_p4_touch_health()`). Nothing here opens a serial port;
capture the line with the usual device tooling first.

Stages follow `pet_p4_touch_init()` in `pet_p4_touch.c`:
  not_started -> i2c_init -> probe -> identify_register_touch -> panel_io -> controller -> queue -> task -> ready

Usage:
  python firmware/tools/touch_diagnose.py status.json [--json]
"""
from __future__ import annotations
import argparse
import json
import sys
from typing import Any

OK = "ESP_OK"
TIMEOUT = "ESP_ERR_TIMEOUT"
NOT_FOUND = "ESP_ERR_NOT_FOUND"
NOT_SUPPORTED = "ESP_ERR_NOT_SUPPORTED"
NO_MEM = "ESP_ERR_NO_MEM"

CATEGORIES = ("display", "bus", "hardware", "driver", "memory", "runtime", "healthy", "unknown")


def find_touch_health(value: Any) -> dict[str, Any] | None:
    """Depth-first search for the `touchHealth` object anywhere in the captured JSON."""
    if isinstance(value, dict):
        if isinstance(value.get("touchHealth"), dict):
            return value["touchHealth"]
        for child in value.values():
            found = find_touch_health(child)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = find_touch_health(child)
            if found is not None:
                return found
    return None


def classify(health: dict[str, Any]) -> dict[str, Any]:
    """Map stage + errors + counters to one category, the evidence used and the next step.

    The categories are deliberately coarse: they tell the person with the board which part to
    look at (panel flex, shared I2C bus, driver identification, memory, polling) and nothing more.
    """
    stage = str(health.get("stage", ""))
    error = str(health.get("error", ""))
    probes = {key: str(health.get(key, "")) for key in ("primaryProbe", "backupProbe", "probe0x15", "probe0x38", "probe0x10", "probe0x18")}
    evidence = {"stage": stage, "error": error, **probes, "controller": health.get("controller"), "chipId": health.get("chipId"), "address": health.get("address")}

    def result(category: str, verdict: str, next_step: str) -> dict[str, Any]:
        assert category in CATEGORIES
        return {"category": category, "verdict": verdict, "next_step": next_step, "evidence": evidence}

    if stage in ("", "not_started"):
        return result("display", "触控初始化从未运行：主流程只在 LCD 初始化成功后才调用 pet_p4_touch_init()",
                      "先看同一份 diagnostics 里的显示/LCD 状态与启动日志；触控不是第一故障点")
    if stage == "i2c_init":
        return result("bus", f"共享 I2C 总线初始化失败（{error}）；音频编解码器也走这条总线",
                      "检查 I2C 上拉与 SDA/SCL 是否被拉死；确认音频 I2C 是否同样报错（pet_p4_audio.c 的 bsp_i2c_init）")
    if stage == "probe":
        if TIMEOUT in (probes["primaryProbe"], probes["backupProbe"], error):
            return result("bus", "GT911 地址探测超时：总线上有设备把 SDA/SCL 拉住，或总线时钟异常",
                          "断开触控排线后重试探测；若仍超时是总线/其他器件问题，不是触控面板")
        codec_alive = OK in (probes["probe0x10"], probes["probe0x18"])
        if probes["probe0x15"] != OK and probes["probe0x38"] != OK:
            if codec_alive:
                return result("hardware", "总线正常（0x10/0x18 有应答），但 0x5D/0x14/0x15/0x38 全无应答：触控控制器不在总线上",
                              "检查触控排线是否插反/未插、面板是否带触控层、触控供电；固件无需改动")
            return result("hardware", "所有探测地址都无应答，且音频编解码地址也无应答：整条总线上没有设备应答",
                          "优先怀疑总线供电或排线；确认音频是否同样不可用后再归类为触控硬件问题")
        return result("driver", "0x15/0x38 有应答但停在 probe 阶段：地址应答后未进入识别",
                      "少见；对照 pet_p4_touch_init() 的 probe 分支与 g_touch_io 创建错误")
    if stage == "identify_register_touch":
        if error == NOT_SUPPORTED:
            return result("driver", f"地址 0x{int(health.get('address') or 0):02x} 应答，但芯片 ID 0x{int(health.get('chipId') or 0):02x} 既不是 CST826(0x11) 也不是 FT6336U(0x64)",
                          "记录 chipId/vendorId，为该控制器补识别与寄存器映射（固件改动，单独立项）")
        return result("driver", f"地址应答但读芯片 ID 失败（{error}）：总线在识别时不稳定或控制器未上电完成",
                      "加长上电等待或重试识别；若间歇出现按总线质量处理")
    if stage in ("panel_io", "controller"):
        return result("driver", f"控制器已识别但驱动创建失败（{stage}: {error}）",
                      "对照 esp_lcd_touch GT911 驱动与 I2C 时钟配置；这是固件问题")
    if stage in ("queue", "task") or error == NO_MEM:
        return result("memory", "触控队列或任务创建失败（内存不足）",
                      "看启动时的堆/任务栈预算；不是触控硬件问题")
    if stage == "ready":
        samples = int(health.get("samples") or 0)
        consecutive = int(health.get("consecutiveErrors") or 0)
        poll_errors = int(health.get("pollErrors") or 0)
        contacts = int(health.get("contacts") or 0)
        if samples == 0:
            return result("runtime", "初始化完成但采样任务没有产生样本", "确认任务是否被更高优先级任务饿死；看 pollErrors 是否同步增长")
        if consecutive >= 3 or (poll_errors and poll_errors * 10 >= samples):
            return result("runtime", f"就绪但轮询持续出错（pollErrors={poll_errors}, consecutiveErrors={consecutive}, samples={samples}）",
                          "总线在运行时不稳定：检查排线接触与 I2C 时钟；控制器可能间歇掉线")
        if contacts == 0:
            return result("healthy", f"控制器 {health.get('controller')} 就绪、轮询正常，但还没有观察到任何触点（samples={samples}）",
                          "在设备上按一下屏幕后再抓一次 diagnostics；或运行 diagnostics/touch-test 看 testScore")
        return result("healthy", f"控制器 {health.get('controller')} 正常：samples={samples}, contacts={contacts}", "可以在 PC 端按能力文件启用触屏组件")
    return result("unknown", f"未知阶段 {stage!r}（error={error}）", "核对固件版本是否与本工具的阶段表一致")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", help="JSON file with a diagnostics/status line or payload, or '-' for stdin")
    parser.add_argument("--json", action="store_true", help="print the classification as JSON")
    args = parser.parse_args(argv)
    raw = sys.stdin.read() if args.source == "-" else open(args.source, encoding="utf-8").read()
    # Accept a whole JSONL capture: use the last line that parses and carries touchHealth.
    health = None
    for line in reversed([line for line in raw.splitlines() if line.strip()] or [raw]):
        try:
            health = find_touch_health(json.loads(line))
        except json.JSONDecodeError:
            continue
        if health is not None:
            break
    if health is None:
        try:
            health = find_touch_health(json.loads(raw))
        except json.JSONDecodeError:
            pass
    if health is None:
        print("no touchHealth object found; capture diagnostics/query -> diagnostics/status first", file=sys.stderr)
        return 2
    verdict = classify(health)
    if args.json:
        print(json.dumps(verdict, ensure_ascii=False, indent=2))
    else:
        print(f"[{verdict['category']}] {verdict['verdict']}")
        print(f"next: {verdict['next_step']}")
        print("evidence: " + ", ".join(f"{k}={v}" for k, v in verdict["evidence"].items() if v not in (None, "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
