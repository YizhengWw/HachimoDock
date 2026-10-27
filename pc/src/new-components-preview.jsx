// Local QA only: real declarations and snapshots exported by the production Rust
// interpreter. These fixture switches never pretend to call a live device.
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import {AppSurface,ComponentAppPreview} from './component-center/AppSurface.jsx';
import hopRuntime from '../new-components/cloud-hop/runtime/widget.json';
import hopBindings from '../new-components/cloud-hop/buttons.json';
import hopBg from '../new-components/cloud-hop/assets/background.png';
import hopHero from '../new-components/cloud-hop/assets/hero.png';
import hopDigits from '../new-components/cloud-hop/assets/digits.png';
import hopLabels from '../new-components/cloud-hop/assets/labels.png';
import {initialApp} from './component-center/app-runtime.js';
import calendarRuntime from '../new-components/daily-almanac/runtime/widget.json';
import calendarBindings from '../new-components/daily-almanac/buttons.json';
import calendarSnapshots from '../new-components/qa/calendar-snapshots.json';
import bg from '../new-components/daily-almanac/assets/background.png';
import digits from '../new-components/daily-almanac/assets/digits.png';
import labels from '../new-components/daily-almanac/assets/labels.png';
import bookRuntime from '../new-components/book-pocket/runtime/widget.json';
import bookBindings from '../new-components/book-pocket/buttons.json';
import bookSnapshots from '../new-components/qa/book-snapshots.json';
import bookBg from '../new-components/book-pocket/assets/background.png';
import bookDigits from '../new-components/book-pocket/assets/digits.png';
import bookLabels from '../new-components/book-pocket/assets/labels.png';
const books=new URLSearchParams(location.search).get('component')==='books';
const hopping=new URLSearchParams(location.search).get('component')==='hop';
const runtime=books?bookRuntime:calendarRuntime,bindings=books?bookBindings:calendarBindings,snapshots=books?bookSnapshots:calendarSnapshots;
const images=[{id:'background',dataUrl:books?bookBg:bg},{id:'digits',dataUrl:books?bookDigits:digits},{id:'labels',dataUrl:books?bookLabels:labels}];
function Preview(){
  const [view,setView]=useState(books?'reading':'today'),[last,setLast]=useState('');
  return <main><style>{`body{margin:0;background:#e9e6df;color:#40382f;font-family:"PingFang SC",sans-serif}main{width:900px;max-width:94%;margin:36px auto}h1{font-size:26px}p{line-height:1.8;color:#71675e}.screen{width:640px;height:480px;border-radius:18px;overflow:hidden;box-shadow:0 15px 44px #463a2726}nav{display:flex;gap:10px;margin:24px 0}button{border:1px solid #d5cbbc;border-radius:10px;padding:10px 16px;background:#fffcf6;color:#40382f}button[aria-pressed=true]{background:#ab4c39;color:white}`}</style>
    <h1>{hopping?'云端跳一跳':books?'每天一本书':'每日黄历'} · 原生 640 × 480</h1><p>{hopping?'按住下方绿色按钮蓄力，松开跳跃。这里运行与固件同源的真实玩法核心。':'读取真实资源包与 Rust 宿主快照。切换的是离线验收场景，不会修改设备、用户记录或播放声音。'}<br/><a href="?component=calendar">查看黄历</a> · <a href="?component=books">查看书籍导读</a> · <a href="?component=hop">试玩跳一跳</a></p>
    {!hopping&&<nav>{Object.keys(snapshots).map(key=><button aria-pressed={key===view} key={key} onClick={()=>setView(key)}>{({'today':'春节当天','details':'宜忌详情','detail-page-2':'详情第二页','leap-month':'闰月','solar-term':'清明节气','reading':'阅读首页','second':'阅读第二页','last':'阅读末页','category':'分类选择'})[key]}</button>)}</nav>}
    <div className="screen">{hopping?<ComponentAppPreview component={{appRuntime:hopRuntime,defaultBindings:hopBindings,sceneSprites:[{id:'background',dataUrl:hopBg},{id:'hero',dataUrl:hopHero},{id:'digits',dataUrl:hopDigits},{id:'labels',dataUrl:hopLabels}]}} active interactive/>:<AppSurface runtime={runtime} snapshot={{...initialApp(runtime),...snapshots[view]}} images={images} bindings={bindings} onAction={setLast}/>}</div>
    <p>点击命中记录：{last||'尚未点击'}。真实状态切换和边界由 Rust 集成测试验证；最终设备字形与触屏还需实测。</p>
  </main>;
}
createRoot(document.getElementById('root')).render(<Preview/>);
