"""Bounded input policy shared by packaging and behavioral tests."""
ENGINE = "p4-input-v1"
def input_errors(runtime, buttons=None):
    if "input" not in runtime:
        return []
    try:
        p=runtime["input"]
        assert isinstance(p,dict) and not set(p)-{"engine","repeat","press","swipe","confirm","drag","cooldown","hold"}
        assert p.get("engine")==ENGINE
        actions={t["on"] for t in runtime["transitions"]}
        for key in ("repeat","press"):
            a=p.get(key,[])
            assert isinstance(a,list) and len(a)<=8 and all(isinstance(x,str) for x in a)
            assert len(set(a))==len(a) and set(a)<=actions
        swipe=p.get("swipe",{})
        assert isinstance(swipe,dict) and not set(swipe)-{"left","right","up","down"}
        assert all(isinstance(x,str) and x in actions for x in swipe.values())
        confirm=p.get("confirm",{})
        assert isinstance(confirm,dict) and len(confirm)<=3
        assert all(a in actions and isinstance(s,str) and 1<=len(s.encode("utf-8"))<=120 for a,s in confirm.items())
        cooldown=p.get("cooldown",{})
        assert isinstance(cooldown,dict) and len(cooldown)<=3
        assert all(a in actions and type(ms) is int and 250<=ms<=60000 for a,ms in cooldown.items())
        assert not cooldown or not any(runtime.get(k) for k in ("host","media","instrument"))
        if "drag" in p:
            d=p["drag"]
            assert isinstance(d,dict) and set(d)=={"entity","axis"} and d["axis"] in ("x","y","xy")
            assert isinstance(d["entity"],str) and 1<=len(d["entity"])<=15
            assert runtime.get("surface",{}).get("scene_view",{}).get("engine")=="p4-scene-surface-v1"
            assert any(e["id"]==d["entity"] and e.get("bounds","clamp")=="clamp" for e in runtime["scene"]["entities"])
        if p.get("press"):
            allowed={"instrument.strike"} if runtime.get("instrument") else {runtime.get("game",{}).get("actions",{}).get("flap")}
            assert set(p["press"])<=allowed
        if "hold" in p:
            a=p["hold"]
            assert isinstance(a,str) and a in actions and runtime.get("scene",{}).get("motion",{}).get("engine")=="p4-motion-v1"
            assert all(a not in p.get(k,{}) for k in ("repeat","press","confirm","cooldown")) and "drag" not in p
        if buttons is not None:
            by_action={b["action"]:b["event"] for b in buttons}
            assert all(by_action.get(a) in ("knob.rotate_ccw","knob.rotate_cw","joystick.up","joystick.down") for a in p.get("repeat",[]))
            assert all(by_action.get(a)=="button.sw1.short_press" for a in p.get("press",[]))
            assert "hold" not in p or by_action.get(p["hold"])=="button.sw1.short_press"
        return []
    except (AssertionError,TypeError,KeyError,AttributeError):
        return ["input 必须使用 p4-input-v1、已声明动作和有界方向/按下策略；即时按下仅用于拍翅或敲击"]
