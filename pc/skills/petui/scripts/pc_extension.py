"""Bounded declarative PC companion and device-surface validation (no execution)."""
import ipaddress
import re
from urllib.parse import urlsplit


def surface_errors(s):
    if isinstance(s,dict) and s.get('engine')=='p4-app-surface-v1':
        from app_surface import app_surface_errors
        return app_surface_errors(s)
    errors = []
    def integer(o, k, lo, hi, default=None):
        v = o.get(k, default)
        if type(v) is not int or not lo <= v <= hi:
            raise ValueError(k)
        return v
    try:
        if not isinstance(s, dict) or not {"background", "elements"} <= set(s) or set(s)-{"background","elements","images","input"}: raise ValueError("surface")
        touch='input' in s
        if touch and s['input']!='p4-media-touch-v1':raise ValueError("input")
        integer(s, "background", 0, 65535)
        if not isinstance(s["elements"], list) or not 1 <= len(s["elements"]) <= (32 if touch else 20): raise ValueError("elements")
        images=s.get("images",[]); ids={}; pixels=0
        if not isinstance(images,list) or ("images" in s and not 1<=len(images)<=4): raise ValueError("images")
        for im in images:
            if not isinstance(im,dict) or set(im)!={"id","asset","frame_width","frame_height","frames","fps"}: raise ValueError("image")
            if not isinstance(im["id"],str) or not re.fullmatch(r"[a-z0-9_-]{1,15}",im["id"]) or im["id"] in ids: raise ValueError("id")
            if not isinstance(im["asset"],str) or not re.fullmatch(r"assets/[A-Za-z0-9_.-]+\.png",im["asset"]) or ".." in im["asset"]: raise ValueError("asset")
            iw=integer(im,"frame_width",8,512);ih=integer(im,"frame_height",8,240)
            integer(im,"frames",1,1);integer(im,"fps",1,1);ids[im["id"]]=(iw,ih);pixels+=iw*ih
        if pixels>98304: raise ValueError("pixels")
        for n in s["elements"]:
            if not isinstance(n, dict) or set(n) - {"type", "x", "y", "w", "h", "color", "page", "scale", "radius", "bind", "text", "image", "rotate_ms","action"}: raise ValueError("node")
            if 'action' in n and (not touch or n['action'] not in {'media.toggle','media.queue','media.previous','media.next','media.up','media.down','media.select'}):raise ValueError("action")
            if n.get("type") not in {"panel", "text", "meter", "ticks", "list", "transport", "image"}: raise ValueError("type")
            x, y = integer(n, "x", 0, 639), integer(n, "y", 0, 479)
            w, h = integer(n, "w", 1, 640-x), integer(n, "h", 1, 480-y)
            integer(n, "color", 0, 65535)
            if n["type"]=="image":
                if n.get("image") not in ids: raise ValueError("image ref")
                if "rotate_ms" in n:
                    integer(n,"rotate_ms",8000,60000)
                    if w!=h or w>320 or ids[n["image"]][0]!=ids[n["image"]][1]: raise ValueError("rotation")
            elif "image" in n or "rotate_ms" in n: raise ValueError("image fields")
            scale = integer(n, "scale", 1, 3, 1)
            integer(n, "page", 0, 2, 2)
            integer(n, "radius", 0, min(24, w//2, h//2), 0)
            if "bind" in n and n["bind"] not in {"title", "subtitle", "status", "message", "volume", "index", "count", "level"}: raise ValueError("bind")
            if "text" in n and (not isinstance(n["text"], str) or len(n["text"].encode()) > 120 or any(ord(c)<32 or ord(c)==127 for c in n["text"])): raise ValueError("text")
            if n["type"] == "text" and (("text" in n) == ("bind" in n) or h < 32*scale): raise ValueError("text content")
            if n["type"] == "meter" and n.get("bind") not in {"volume", "level"}: raise ValueError("meter")
            if n["type"] == "list" and h < 48: raise ValueError("list")
    except (ValueError, TypeError, KeyError):
        errors.append("surface 需要有界布局：最多 20 个节点、640×480 范围及受控数据绑定")
    return errors


def extension_errors(e):
    def obj(v, required, optional=()):
        if not isinstance(v, dict) or not set(required) <= set(v) or set(v)-set(required)-set(optional): raise ValueError("fields")
    def text(v, maximum):
        if not isinstance(v, str) or not v.strip() or len(v.encode()) > maximum or any(ord(c)<32 or ord(c)==127 for c in v): raise ValueError("text")
    def url(v):
        text(v, 2048); u=urlsplit(v)
        if u.scheme not in {"http", "https"} or not u.hostname or u.username or u.password or u.fragment or u.port not in {None, 80, 443}: raise ValueError("url")
        return u
    def pointer(v):
        if not isinstance(v,str) or len(v.encode())>128 or (v and not v.startswith('/')) or any(ord(c)<32 for c in v): raise ValueError("pointer")
    try:
        obj(e, {"schemaVersion", "engine", "permissions", "search", "panel"}, {"discovery"})
        if type(e["schemaVersion"]) is not int or e["schemaVersion"] != 1 or e["engine"] != "pc-declarative-media-v1": raise ValueError("engine")
        p=e["permissions"];obj(p, {"networkHosts", "publicAudio", "storage"})
        if p["publicAudio"] is not True or p["storage"] is not True or not isinstance(p["networkHosts"],list) or not 1 <= len(p["networkHosts"]) <= 4: raise ValueError("permissions")
        for h in p["networkHosts"]:
            text(h,253)
            if '.' not in h or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?", x) for x in h.split('.')): raise ValueError("host")
            try: ipaddress.ip_address(h)
            except ValueError: pass
            else: raise ValueError("literal host")
        s=e["search"];obj(s,{"endpoint", "queryParam", "params", "itemsPath", "fields"},{"browseQuery"})
        if s.get("browseQuery") is not None:
            text(s["browseQuery"],120)
            if len(s["browseQuery"])>30: raise ValueError("browseQuery")
        u=url(s["endpoint"])
        if u.scheme!='https' or u.query or u.hostname not in p["networkHosts"]: raise ValueError("scope")
        if not isinstance(s["params"],dict) or len(s["params"])>12 or s["queryParam"] in s["params"]: raise ValueError("params")
        for key in [s["queryParam"], *s["params"]]:
            if not re.fullmatch(r"[A-Za-z0-9_]{1,40}",key): raise ValueError("query")
        for v in s["params"].values(): text(v,128)
        pointer(s["itemsPath"])
        f=s["fields"];obj(f,{"id","title","url"},{"fallbackUrl","homepage","codec","hls","available"})
        for v in f.values():
            if v is not None: pointer(v)
        if 'discovery' in e:
            d=e['discovery'];obj(d,{'endpoints','locationEndpoint','cityField','countryField','country','aliases','stationRegion','popularity','votes','nationalCount','localSupplement','refreshSec','autoPlayOnEnter'})
            if not isinstance(d['endpoints'],list) or not 1<=len(d['endpoints'])<=3:raise ValueError('endpoints')
            for raw in [*d['endpoints'],d['locationEndpoint']]:
                u=url(raw)
                if u.scheme!='https' or u.hostname not in p['networkHosts']:raise ValueError('scope')
            for k in ['cityField','countryField','stationRegion','popularity','votes']:pointer(d[k])
            if not re.fullmatch('[A-Z]{2}',d['country']) or type(d['autoPlayOnEnter']) is not bool:raise ValueError('policy')
            for k,lo,hi in [('nationalCount',1,100),('localSupplement',1,30),('refreshSec',3600,604800)]:
                if type(d[k]) is not int or not lo<=d[k]<=hi:raise ValueError(k)
            if not isinstance(d['aliases'],dict) or len(d['aliases'])>80:raise ValueError('aliases')
            for a,b in d['aliases'].items():text(a,80);text(b,80)
            if s['itemsPath']!='':raise ValueError('discovery rows must be a root array')
        ui=e["panel"];obj(ui,{"title","description","searchPlaceholder","attribution","attributionUrl","sections"})
        for k,maximum in [("title",120),("description",1200),("searchPlaceholder",180),("attribution",120)]: text(ui[k],maximum)
        url(ui["attributionUrl"])
        sections=ui["sections"]
        if not isinstance(sections,list) or not 1<=len(sections)<=5 or len(set(sections))!=len(sections) or set(sections)-{"search","library","history","volume","sleep"}: raise ValueError("sections")
    except (ValueError,TypeError,KeyError,AttributeError):
        return ["pc/extension.json 配置无效：需要受控域名、GET 搜索映射与声明式面板"]
    return []
