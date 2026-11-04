"""p4-notify-v1: one bounded widget/notify payload shape, one per component per 30 s."""
import re
ENGINE='p4-notify-v1'
TOPIC='widget/notify'
COOLDOWN_MS=30000
TITLE_BYTES=63
TEXT_BYTES=120
def _text(v,maximum,nonempty):
    return isinstance(v,str) and (not nonempty or bool(v)) and len(v.encode())<=maximum and not any(ord(c)<32 or ord(c)==127 for c in v)
def valid_payload(p):
    return (isinstance(p,dict) and {'componentId','title'}<=set(p)<={'componentId','title','text','badge'}
        and isinstance(p['componentId'],str) and re.fullmatch(r'[a-z0-9_-]{1,47}',p['componentId']) is not None
        and _text(p['title'],TITLE_BYTES,True) and ('text' not in p or _text(p['text'],TEXT_BYTES,False))
        and ('badge' not in p or (type(p['badge']) is int and -10**9<=p['badge']<=10**9)))
def errors(runtime):
    """Package declaration: runtime.notify = {"title": <=63 B, "badge_var"?: int var}; transitions/tick.then may carry notify:true."""
    if not isinstance(runtime,dict):return []
    rules=[*runtime.get('transitions',[]),*[t.get('then',{}) for t in runtime.get('tick',[]) if isinstance(t,dict)]]
    uses=any(isinstance(r,dict) and 'notify' in r for r in rules)
    if 'notify' not in runtime and not uses:return []
    bad=['通知需要 p4-notify-v1：runtime.notify 的 title ≤63 字节、可选整数 badge_var，transition/tick.then 只能写 notify:true']
    try:
        n=runtime['notify'];assert isinstance(n,dict) and {'title'}<=set(n)<={'title','badge_var'} and _text(n['title'],TITLE_BYTES,True)
        if 'badge_var' in n:assert runtime.get('vars',{}).get(n['badge_var'],{}).get('type')=='int'
        assert all(r.get('notify') is True for r in rules if isinstance(r,dict) and 'notify' in r)
        return []
    except (AssertionError,KeyError,TypeError,AttributeError):return bad
class Limiter:
    """Per-component cooldown shared by the PC and the device; 8 slots, oldest reused."""
    def __init__(self):self.slots={}
    def admit(self,component,now_ms):
        last=self.slots.get(component)
        if last is not None and 0<=now_ms-last<COOLDOWN_MS:return False
        if component not in self.slots and len(self.slots)>=8:del self.slots[min(self.slots,key=self.slots.get)]
        self.slots[component]=now_ms;return True
def bubble_title(p):return f"{p['title']} · {p['badge']}" if 'badge' in p else p['title']
