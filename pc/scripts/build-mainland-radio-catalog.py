"""Build pc/catalog.json for city-radio from the rechecked china-radio rows (mainland, alive only)."""
import hashlib, json, re, sys
from urllib.parse import urlsplit
from collections import defaultdict

SRC, OUT = sys.argv[1], sys.argv[2]
# Apply the policy AGAIN to checked_url: an upstream alternative can differ
# from the primary URL tested by the initial candidate filter. Unknown hosts
# and literal IPs are omitted pending review, never guessed from a .cn suffix.
APPROVED_HOSTS = {
    'lhttp.qingting.fm', 'ls.qingting.fm', 'live.xmcdn.com',
    'satellitepull.cnr.cn', 'ngcdn001.cnr.cn', 'ngcdn002.cnr.cn',
    'lzlive.vojs.cn', 'livecdn.pull.cdbs.com.cn', 'stream.hndt.com',
    'stream3.hndt.com', 'live.shaoxing.com.cn', 'audiolive302.iqilu.com',
    'zbbf2.ahbztv.com', 'live.ahsz.tv', 'm3u8.channel.wsrtv.com.cn',
    'live.hnxttv.com', 'l.cztvcloud.com',
}
def eligible(r):
    if not r.get('alive') or r.get('p') in {'中国香港','中国澳门','中国台湾'} or r.get('c')=='外语':return False
    if any(k in r.get('n','').upper() for k in ('CGTN','CRI','国际台','环球','香港','澳门','台湾','台海','大湾区之声')):return False
    u=urlsplit(r.get('checked_url',''))
    return u.scheme in {'http','https'} and u.hostname in APPROVED_HOSTS and not u.username and not u.password and u.port in (None,80,443) and not u.fragment
rows = [r for r in json.load(open(SRC, encoding="utf-8"))["results"] if eligible(r)]

# Editorial order for the national list: central first, then provinces roughly by audience size.
PROVINCE_ORDER = ["中央", "北京", "上海", "广东", "江苏", "浙江", "山东", "四川", "河南", "湖北", "湖南", "河北", "福建", "安徽",
                  "陕西", "辽宁", "重庆", "天津", "江西", "广西", "云南", "山西", "黑龙江", "吉林", "贵州", "内蒙古", "新疆",
                  "甘肃", "海南", "宁夏", "青海", "西藏", "其他"]
CAPITAL = {"广东": "广州", "江苏": "南京", "浙江": "杭州", "山东": "济南", "四川": "成都", "河南": "郑州", "湖北": "武汉", "湖南": "长沙",
           "河北": "石家庄", "福建": "福州", "安徽": "合肥", "陕西": "西安", "辽宁": "沈阳", "江西": "南昌", "广西": "南宁", "云南": "昆明",
           "山西": "太原", "黑龙江": "哈尔滨", "吉林": "长春", "贵州": "贵阳", "内蒙古": "呼和浩特", "新疆": "乌鲁木齐", "甘肃": "兰州",
           "海南": "海口", "宁夏": "银川", "青海": "西宁", "西藏": "拉萨"}
CATEGORY_WEIGHT = {"新闻": 60, "综合": 50, "交通": 40, "音乐": 30, "经济": 20, "生活": 10}
KEYWORDS = r"(新闻|综合|交通|音乐|经济|文艺|生活|人民广播|广播|电台|之声|调频|FM|AM|卫视)"

def clean_name(n, prov):
    n = re.sub(r"\s+", " ", n).strip()
    n = re.sub(r"^(CNR|cnr)\s*", "央广 ", n)
    n = re.sub(rf"^{re.escape(prov)}\s*[-–—·]\s*", "", n)          # "江苏-扬州新闻广播" -> "扬州新闻广播"
    return n[:50]

LONG_CITIES = ("哈尔滨", "嘉峪关", "锡林浩特", "石家庄", "呼和浩特", "乌鲁木齐", "张家口", "秦皇岛", "连云港", "驻马店", "平顶山", "攀枝花", "景德镇", "马鞍山",
               "六盘水", "牡丹江", "齐齐哈尔", "佳木斯", "日喀则", "克拉玛依", "阿克苏", "葫芦岛", "鄂尔多斯", "巴彦淖尔", "锡林郭勒",
               "呼伦贝尔", "张家界", "湘西", "黔东南", "黔南", "黔西南", "大兴安岭", "西双版纳", "德宏", "怒江", "迪庆", "博尔塔拉", "巴音郭楞")

def city_of(name, prov):
    """Province-level station keeps the province; otherwise the leading city name (2 chars unless a known longer city)."""
    if name.startswith(prov) or name.startswith("央广"): return prov
    for city in LONG_CITIES:
        if name.startswith(city): return city
    m = re.match(r"^([一-鿿]{2})", name)
    return m.group(1) if m else prov

by_prov = defaultdict(list)
seen_urls, seen_names = set(), set()
dropped_ascii = 0
for r in rows:
    name = clean_name(r["n"], r["p"])
    if not re.search(r"[一-鿿]", name): dropped_ascii += 1; continue   # romanised duplicates
    key = re.sub(r"\s", "", name).lower()
    if r["checked_url"] in seen_urls or key in seen_names: continue
    seen_urls.add(r["checked_url"]); seen_names.add(key)
    by_prov[r["p"]].append(dict(r, name=name))

def flagship_score(r, prov):
    n = r["name"]; s = CATEGORY_WEIGHT.get(r["c"], 0)
    if prov != "中央":
        if n.startswith(prov) or n.startswith(prov + "之声"): s += 400
        if CAPITAL.get(prov) and CAPITAL[prov] in n: s += 200
        if re.search(r"人民广播电台|区人民|县人民|县|区", n): s -= 200
    if "伴音" in n: s -= 500
    if re.search(r"卖药|老年|少儿|戏曲|故事|长书", n): s -= 100
    return -s

catalog = []
for prov, items in by_prov.items():
    pidx = PROVINCE_ORDER.index(prov) if prov in PROVINCE_ORDER else len(PROVINCE_ORDER)
    items.sort(key=lambda r: (flagship_score(r, prov), len(r["name"])))
    for i, r in enumerate(items):
        if prov == "中央": rank = 1000 - i
        elif i == 0: rank = 600 - pidx
        elif i == 1: rank = 300 - pidx
        else: rank = max(1, 100 - pidx - i)
        if "伴音" in r["name"]: rank = min(rank, 20)   # TV audio feeds are not radio flagships
        url = r["checked_url"]
        catalog.append({
            "id": "cr-" + hashlib.sha1(url.encode()).hexdigest()[:12],
            "name": r["name"],
            "url": url,
            "region": city_of(r["name"], prov) if prov not in ("中央", "其他") else prov,
            "province": prov,
            "category": r["c"],
            "hls": url.split("?")[0].lower().endswith((".m3u8", ".m3u")),
            "available": True,
            "rank": rank,
            "votes": 0,
        })
catalog.sort(key=lambda s: (-s["rank"], s["province"], s["name"]))
json.dump(catalog, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
import os
print("stations", len(catalog), "bytes", os.path.getsize(OUT), "dropped ascii-only names", dropped_ascii)
print("national top 30:")
for s in catalog[:30]: print("  ", s["rank"], s["province"], s["region"], "|", s["name"])
print("regions sample:", sorted(set(s["region"] for s in catalog))[:60])
