// Existing components only. Never writes the installed library or publishes remotely.
import fs from "node:fs";
import path from "node:path";
import {fileURLToPath} from "node:url";
import {appDashboard,initialApp} from "../src/component-center/app-runtime.js";
const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"..");
const [source,out]=process.argv.slice(2);
if(!source||!out||path.resolve(source)===path.resolve(out))throw Error("Independent source and output directories required");
const read=p=>JSON.parse(fs.readFileSync(p,"utf8"));
const write=(p,v)=>fs.writeFileSync(p,JSON.stringify(v,null,2)+"\n");
// Mechanisms are declared before copying any package. Art and identities stay intact.
const mechanisms={
  "block-combo":["保留 SW2 旋转；新增摇杆按下暂停","暂停时停止时钟，继续不补帧","跨重开保存最高分"],
  "bloomfrog_companion":["亲密度封顶 100","亲密度/喂食/抚摸次数存档","动作反馈和触屏与实体键一致"],
  "computer-status":["扩大翻页触屏命中区","保存最后查看页","保留真实指标和过期标识"],
  "drink-reminder":["确认才增加记录","到期暂停后仍恢复到期状态","保存间隔/记录，重进暂停，主动继续"],
  "flappy-bird":["保留 50% 纵向速度","暂停时不能拍翅/意外重开","最高分存档"],
  "fresh-fruit-slice":["保留三模式、连击、大小差异","检查退出暂停与重开条件","离线验证碰撞和结算"],
  "hachimi-break":["五分钟稍后状态不丢失","进行中离开后暂停恢复","触屏选择休息动作"],
  "hachimi-garden":["触屏调节时长","保留专注进度和花朵","修正图像/大字过度放大"],
  "hachimi-home":["直接点绿植/摆件切换","提供完成布置动作","保留物件和摆放存档"],
  "hachimi-learn":["合并已经审校的 1000 条问答","保留分类随机袋和全文翻页","宠物音色自动读，退出停止"],
  "hachimi-notes":["长文转待办使用有界标题且保留全文","防重复保存和迟到回执","真实录音验收仍延后"],
  "hachimi-sounds":["播放时更换定时明确语义","剩余时间可见","退出/实时对话停止"],
  "hachimi-travel":["旅行中查看明信片可正常翻页","下一站预选不重置旅程","保留默认卡和真实收集区别"],
  "hachimi-weather":["排除已完成待办","设备选城与手动优先","网络失败仍显示城市"],
  "snake-turn":["说明相对蛇头左/右转","暂停/继续与最高分","暂停不接受转向操作"],
  "stock-watchlist":["保留行情实时数据和风险提示","设备触屏翻页","数据空/过期测试"],
  "token-usage":["显示实际可用统计维度","固定数据与预览边界","各统计页与触屏一致"],
  "tomato-clock":["暂停时重置/跳过仍暂停","保存计时和完成轮数","结束只计一次，恢复不补时"],
  "two-key-pong":["暂停/继续与最高分","保留当前用户熟悉的挡板动作","暂停不继续计分"],
  "upcoming-todos":["扩大翻页触屏命中区","保存阅读页","蓝色待办与绿色完成、空列表和翻页"],
  "wooden-fish":["触屏敲击与主键同一真实动作","保持音效节奏和小体积素材","不改变全局退出"],
};
fs.mkdirSync(out,{recursive:true});
write(path.join(out,"mechanisms.json"),mechanisms);
function button(action,text,x,y,w=100,color=63454,foreground=16970){
  return {type:"button",x,y,w,h:48,color,foreground,font_size:16,radius:14,padding:10,text,action};
}
function mapped(action,event,label){return{action,event,label,control:event.startsWith("button.sw2")?"SW2":"前方摇杆"};}
function persist(r,vars,states,page=false){r.persist={version:r.persist?.version||1,vars,states:states||Object.fromEntries(r.states.map(s=>[s,s])),...(page?{page:true}:{})};}
for(const [id,checks] of Object.entries(mechanisms)){
  const from=fs.existsSync(path.join(source,id))?path.join(source,id):path.join(pc,"builtin-clawpkgs",id);
  const root=path.join(out,"packages",id);
  if(fs.existsSync(root))throw Error(`Refusing to replace ${root}`);
  fs.cpSync(from,root,{recursive:true});
  const r=read(path.join(root,"runtime/widget.json")),c=read(path.join(root,"component.json")),b=read(path.join(root,"buttons.json"));
  if(c.id!==id)throw Error("Identity mismatch");
  const v=c.version.split(".").map(Number);v[2]++;c.version=v.join(".");
  if(["snake-turn","flappy-bird","block-combo","two-key-pong"].includes(id)){
    const g=r.game||r.scene,active=g.playing_state||g.active_state;
    r.states.push("paused");r.vars.best={type:"int",init:0};
    r.lifecycle={pause_action:"session.pause",record_var:"best"};
    r.transitions.push({from:active,on:"session.pause",to:"paused"},{from:"paused",on:"session.pause",to:active});
    if(id==="flappy-bird")r.transitions=r.transitions.flatMap(t=>t.on==="flappy.flap"?["ready","playing","result"].map(from=>({...t,from})):t);
    persist(r,["best"],Object.fromEntries(r.states.map(s=>[s,r.initial_state])));
    b.push(mapped("session.pause",id==="block-combo"?"button.encoder.short_press":"button.sw2.short_press","暂停/继续"));
    // Shared pause affordance, not a fake restart of the native game.
    const bg=r.surface.background,fg=r.surface.elements.find(n=>n.bind==="title").color;
    r.surface.elements.push(button("session.pause","暂停/继续",480,52,132,bg,fg));
    r.surface.elements.push({type:"text",x:30,y:56,w:130,h:24,color:fg,font_size:15,bind:"badge"});
    r.dashboard.badge={var:"best"};
    r.surface.elements.push({type:"text",x:162,y:56,w:300,h:24,color:fg,font_size:14,text:"历史最高 · 离开自动暂停"});
    for(const [key,value] of Object.entries(r.dashboard)){
      if(value?.switch_state)value.switch_state.paused=({headline:"已暂停",metricUnit:"先继续游戏",footer:"暂停中 · 按暂停键继续",eyebrow:"休息一下",note:"PAUSED",visualSprite:"timer"})[key]||"已暂停";
    }
    if(id==="snake-turn"){
      for(const n of r.surface.elements)if(n.text==="摇杆左右转向")n.text="相对蛇头左右转向";
      c.description="吃到食物增长，碰到身体或边界结束；左右是相对蛇头转向。支持暂停并保存最高分。";
    }
  }
  if(id==="bloomfrog_companion"){
    r.limits={bond:[0,100],bond_max:[100,100],pets:[0,1000000000],meals:[0,1000000000]};
    persist(r,["bond","pets","meals"]);
    r.dashboard.note="亲密度上限 100；互动次数会保存。";
    r.transitions.find(t=>t.on==="feed_pet").set.reply="吃饱啦，谢谢你！";
  }
  if(id==="drink-reminder"){
    r.states.push("paused_due");
    r.transitions.find(t=>t.from==="due"&&t.on==="reminder.pause_resume").to="paused_due";
    r.transitions.push({from:"paused_due",on:"reminder.pause_resume",to:"due"});
    r.transitions[0].set.remaining_min={var:"interval_min"};
    r.dashboard.headline.switch_state.paused_due="喝水提醒已暂存。";
    r.dashboard.metricUnit.switch_state.paused_due="恢复提醒";
    r.dashboard.visualSprite.switch_state.paused_due="timer";
    r.limits={remaining_min:[0,60],interval_min:[60,60],drink_count_today:[0,1000000000]};
    persist(r,Object.keys(r.vars),{counting:"paused",paused:"paused",due:"paused_due",paused_due:"paused_due"},true);
    r.dashboard.metricLabel.switch_page.records="累计确认次数";
    r.dashboard.note.switch_page.countdown="每隔 60 分钟；重新进入后点继续。";
  }
  if(id==="tomato-clock"){
    for(const t of r.transitions)if(t.from.endsWith("_paused")&&t.to&&!t.on.endsWith("start_pause"))t.to+="_paused";
    persist(r,Object.keys(r.vars),{focus:"focus_paused",focus_paused:"focus_paused",rest:"rest_paused",rest_paused:"rest_paused"});
    r.limits={remaining_s:[0,1500],phase_total_s:[300,1500],completed_sessions:[0,1000000000]};
  }
  if(id==="hachimi-break"){
    persist(r,["rounds","left"],{idle:"idle",run:"paused",paused:"paused",done:"done",later:"later"},true);
    r.surface.elements.push(button("previous","上个动作",390,366,106,65502,18924),button("next","下个动作",506,366,106,65502,18924));
  }
  if(id==="hachimi-garden"){
    r.surface.elements.push(button("less","−5 分钟",366,366,116,65502,8677),button("more","+5 分钟",492,366,122,65502,8677));
  }
  if(id==="hachimi-home"){
    r.vars.editing={type:"int",init:0};
    r.transitions.find(t=>t.on==="previous").inc={plant:1};
    r.transitions.find(t=>t.on==="next").inc={object:1};
    r.transitions.push({from:"*",on:"finish",to:"idle"});
    b.push(mapped("finish","button.encoder.short_press","完成布置"));
    for(const t of r.transitions)if(t.to)t.set={...t.set,editing:["plant","object","place"].includes(t.to)?1:0};
    r.surface.elements.push({...button("finish","完成布置",328,366,142,65502,20998),show:{var:"editing",min:1,max:1}});
    r.dashboard.note.switch_state.plant="点绿植换一株；点完成保存布置。";
    r.dashboard.note.switch_state.object="点摆件换一件；点完成保存布置。";
  }
  if(["computer-status","upcoming-todos"].includes(id)){
    for(const n of r.surface.elements)if(n.action){n.y=396;n.h=48;}
    r.surface.data_view.h=324;
    persist(r,["list_page"]);r.limits={list_page:[0,1000000000]};
    // Do not clamp negatives: data pager intentionally wraps previous at zero.
    delete r.limits;
  }
  if(id==="token-usage"){
    r.surface.elements.find(n=>n.bind==="note").h=44;
    r.dashboard.footer="统计来自当前 Agent 的本地可读记录，不代表账单。";
  }
  if(id==="stock-watchlist"){
    const ink=61343,muted=40313,accent=40478,bg=4325;
    r.surface={engine:"p4-app-surface-v1",graphics:"p4-app-graphics-v1",background:bg,
      data_view:{engine:"p4-data-surface-v1",layout:"metrics",x:28,y:66,w:584,h:324,colors:[ink,muted,accent,17484,6537]},
      elements:[
        {type:"icon",x:28,y:23,w:21,h:21,color:accent,icon:"desk"},
        {type:"text",x:60,y:18,w:320,h:30,color:ink,font_size:22,bind:"title",weight:600},
        button("list.previous","上一页",28,396,176,8683,ink),
        button("list.home","回首页",218,396,184,accent,4391),
        button("list.next","下一页",416,396,196,8683,ink),
        {type:"text",x:28,y:450,w:584,h:24,color:muted,font_size:14,text:"行情仅供参考 · PC 管理自选股 · 全局键退出"}
      ]};
    persist(r,["list_page"]);
  }
  if(id==="hachimi-notes")c.description="按键录制想法，转写后确认保存或转为待办；笔记只保存在本机。录音听感与转写实机验收待完成。";
  if(id==="wooden-fish")r.dashboard.footer="按主操作或轻点屏幕敲一下 · 全局键退出";
  if(id==="stock-watchlist")r.dashboard.note="PC 管理自选股；行情仅供参考，过期数据会标记。";
  const aliases={"block-combo":"土豆泥","flappy-bird":"纸飞机","snake-turn":"小豆包"};
  if(aliases[id])c.author=aliases[id];
  write(path.join(root,"component.json"),c);
  write(path.join(root,"runtime/widget.json"),r);
  write(path.join(root,"buttons.json"),b);
  const negative=read(path.join(root,"negative-screen.json")),initial=appDashboard(r,initialApp(r));
  for(const [key,value] of Object.entries(initial))if(key!=="progress")negative.dashboard[key]=value;
  write(path.join(root,"negative-screen.json"),negative);
  // Report only structure/size here. Successful generation is not validation.
  console.log(`${id} ${c.version} ${Buffer.byteLength(JSON.stringify(r))} bytes; ${checks.length} regression goals`);
}
