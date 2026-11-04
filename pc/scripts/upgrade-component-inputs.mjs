// Upgrade copies only. Publication, installation, firmware writes are separate.
import fs from "node:fs";
import path from "node:path";
import {initialApp,appDashboard} from "../src/component-center/app-runtime.js";
const [source,out]=process.argv.slice(2);
if(!source||!out||path.resolve(source)===path.resolve(out))throw Error("Independent source/output required");
const read=p=>JSON.parse(fs.readFileSync(p,"utf8"));
const write=(p,v)=>fs.writeFileSync(p,JSON.stringify(v,null,2)+"\n");
const binding=(action,event,label)=>({action,event,label,control:event.startsWith("button.sw1")?"SW1":event.startsWith("button.sw2")?"SW2":"前方摇杆"});
const button=(action,text,x,y,w=100,h=48)=>({type:"button",x,y,w,h,color:63454,foreground:16970,font_size:16,radius:14,padding:10,text,action});
const report=[];
for(const id of fs.readdirSync(source).sort()){
  if(!fs.existsSync(path.join(source,id,"component.json")))continue;
  if(id==="music-player")throw Error("Disabled component must not enter this batch");
  const root=path.join(out,id);if(fs.existsSync(root))throw Error(`Refusing to overwrite ${root}`);
  fs.cpSync(path.join(source,id),root,{recursive:true});
  const r=read(path.join(root,"runtime/widget.json")),c=read(path.join(root,"component.json"));
  let b=read(path.join(root,"buttons.json"));
  const v=c.version.split(".").map(Number);v[2]++;c.version=v.join(".");
  r.input={engine:"p4-input-v1"};
  const nodes=r.surface?.elements||[];
  const add=(from,on,extra={})=>r.transitions.push({from,on,...extra});
  const map=(a,e,l)=>{const old=b.find(x=>x.action===a);if(old)Object.assign(old,binding(a,e,l));else b.push(binding(a,e,l));};
  const swipe=(left,right)=>{r.input.swipe={left,right};};
  const notes=["组件内滑动不触发全局退出","未声明的摇杆动作不连发","状态不匹配的按钮禁用"];
  if(["block-combo","snake-turn","flappy-bird","two-key-pong"].includes(id)){
    const badge=nodes.find(n=>n.bind==="badge"),caption=nodes.find(n=>n.text==="历史最高 · 离开自动暂停");
    if(badge)Object.assign(badge,{x:130,y:52,w:300,h:28,font_size:18});
    if(caption)Object.assign(caption,{x:28,y:54,w:96,h:24,text:"历史最高",font_size:15});
    const pause=nodes.find(n=>n.action==="session.pause");
    if(pause)Object.assign(pause,{x:492,y:16,w:122,h:44,font_size:15,text:"暂停/继续"});
    if(id!=="flappy-bird"){
      const start=r.game?.actions.start||"start";
      add("paused",start,{to:"playing"});
      if(r.dashboard.metricUnit?.switch_state)r.dashboard.metricUnit.switch_state.paused="继续游戏";
      notes.push("暂停后的主按钮继续原局，不重新开始");
    }
  }
  if(id==="block-combo"){
    r.game.actions.soft_drop="blocks.soft_drop";
    add("playing","blocks.soft_drop");
    map("blocks.soft_drop","joystick.down","缓降（可长按）");
    map("blocks.drop","joystick.up","直接落下（单次）");
    r.input.repeat=["blocks.left","blocks.right","blocks.soft_drop"];
    const drop=nodes.find(n=>n.action==="blocks.drop");drop.text="直落";
    const rotate=nodes.find(n=>n.action==="blocks.rotate");rotate.text="旋转 SW2";
    nodes.push(button("blocks.soft_drop","缓降",378,263,236,48));
    notes.push("下摇软降，上摇单次直落，SW2 旋转");
    c.description="移动、旋转方块，填满横行消除得分；堆到顶部结束。缓降与一次直落分开操作，支持暂停续玩和最高分。";
    for(const n of nodes)if(n.type==="text"&&n.y>=436&&n.text?.includes("摇杆")){n.text="左右移动 · 下缓降 · 上直落";n.font_size=12;}
  }
  if(id==="snake-turn"){
    r.game.direction_mode="absolute";
    r.game.actions.up="snake.up";r.game.actions.down="snake.down";
    add("playing","snake.up");add("playing","snake.down");
    map("snake.up","joystick.up","向上");map("snake.down","joystick.down","向下");
    const left=nodes.find(n=>n.action===r.game.actions.left),right=nodes.find(n=>n.action===r.game.actions.right);
    Object.assign(left,{text:"左",x:490,y:326,w:58,h:44});
    Object.assign(right,{text:"右",x:556,y:326,w:58,h:44});
    nodes.push(button("snake.up","上",490,263,124,48),button("snake.down","下",490,378,124,44));
    for(const n of nodes)if(n.text?.includes("相对蛇头"))n.text="摇杆四向移动";
    b.filter(x=>x.action===r.game.actions.left||x.action===r.game.actions.right).forEach(x=>x.label=x.action===r.game.actions.left?"向左":"向右");
    c.description="摇杆四方向控制蛇头；禁止反向和一帧内连续折返。支持触屏方向键、暂停和最高分。";
    notes.push("绝对方向，一步只接收一次有效转向，不允许掉头");
  }
  if(id==="flappy-bird"){
    r.input.press=["flappy.flap"];
    const v=r.surface.game_view;
    nodes.push({type:"panel",x:v.x,y:v.y,w:v.w,h:v.h,color:0,opacity:0,action:"flappy.flap"});
    notes.push("点游戏区或按下 SW1 即拍翅，释放不重复，保持半速");
  }
  if(id==="wooden-fish"){r.input.press=["instrument.strike"];notes.push("触屏按下与 SW1 即时敲击，一次按下仅一次");}
  if(id==="two-key-pong"){r.input.repeat=["shift_left","shift_right"];r.input.drag={entity:"bat",axis:"x"};notes.push("摇杆长按连续移动；触屏横向拖动球拍");}
  if(id==="hachimi-learn")swipe("host.next","host.previous");
  if(id==="hachimi-travel"){
    swipe("host.next","host.previous");
    for(const n of nodes){if(n.text==="上一站")n.text="上一项";if(n.text==="下一站")n.text="下一项";}
  }
  if(id==="hachimi-weather"){
    swipe("host.next","host.previous");
    for(const n of nodes)if(["host.previous","host.next"].includes(n.action))n.host_mask=8;
    notes.push("普通天气页隐藏选城切换键；仅在选城时显示");
  }
  if(id==="hachimi-sounds"){
    r.input.repeat=["host.up","host.down"];
    const profile=read(path.join(root,"pc/application.json"));
    ["rain","sea","fire","insects"].forEach((op,index)=>{
      const action=`host.${op}`;profile.actions[action]=op;add("*",action);
      const chip=button(action,["窗边细雨","海边潮汐","暖炉微火","安静虫鸣"][index],26+index*147,188,139,48);
      Object.assign(chip,{color:12811,foreground:61341,font_size:16});nodes.push(chip);
    });
    write(path.join(root,"pc/application.json"),profile);
    Object.assign(nodes.find(n=>n.bind==="headline"),{y:126,h:48,font_size:32});
    nodes.push({...button("host.down","−",442,366,48),color:12811,foreground:61341});
    nodes.push({type:"text",x:492,y:378,w:68,h:28,color:61341,font_size:18,bind:"metricValue",align:"center"});
    nodes.push({...button("host.up","+",564,366,48),color:12811,foreground:61341});
    r.dashboard.note="点选声景；左右切换，上下调音量。";
    r.dashboard.metricValue="35%";
    notes.push("四个声景直接点选，独立音量加减与百分比；点当前声景不重启定时");
  }
  if(id==="hachimi-notes"){
    // Directional gestures navigate only; destructive actions require deliberate buttons.
    b=b.filter(x=>!["host.todo","host.delete"].includes(x.action));
    nodes.push(button("host.todo","转为待办",348,366,124),button("host.delete","删除",488,366,124));
    r.input.confirm={"host.delete":"删除当前笔记？此操作不能撤销。","host.todo":"将当前笔记添加到近期待办？"};
    for(const n of nodes)if(n.action)n.host_mask=({"host.primary":163,"host.secondary":16,"host.todo":48,"host.delete":32})[n.action]||255;
    swipe("host.next","host.previous");
    notes.push("移除摇杆转待办/删除快捷操作，改为屏幕确认");
  }
  if(id==="tomato-clock"){
    c.description="开始前选择 5–60 分钟专注时长，结束后休息 5 分钟；可暂停、跳过或确认重计时。只有完整专注结束才增加完成轮次。";
    r.states.push("ready");r.initial_state="ready";
    add("ready","tomato.start_pause",{to:"focus"});
    for(const value of Object.values(r.dashboard))if(value?.switch_state)value.switch_state.ready=value.switch_state.focus_paused??value.switch_state.focus;
    r.dashboard.metricUnit.switch_state.ready="开始专注";
    if(r.persist)r.persist.states.ready="ready";
    r.vars.focus_s={type:"int",init:1500};
    r.limits.focus_s=[300,3600];r.limits.remaining_s=[0,3600];r.limits.phase_total_s=[300,3600];
    r.persist.vars.push("focus_s");
    for(const t of [...r.transitions,...r.tick.map(t=>t.then)])if(t?.set?.remaining_s===1500){
      t.set.remaining_s={var:"focus_s"};t.set.phase_total_s={var:"focus_s"};
    }
    add("ready","tomato.shorter",{inc:{focus_s:-300}});
    add("ready","tomato.longer",{inc:{focus_s:300}});
    // Countdown needs zero; keep the editable duration separate until starting.
    r.dashboard.metricValue={switch_state:Object.fromEntries(r.states.map(s=>[s,{fmt_mmss:s==="ready"?"focus_s":"remaining_s"}]))};
    r.transitions.find(t=>t.from==="ready"&&t.on==="tomato.start_pause").set={remaining_s:{var:"focus_s"},phase_total_s:{var:"focus_s"}};
    nodes.push(button("tomato.shorter","−5 分钟",198,378,156),button("tomato.longer","+5 分钟",370,378,156));
    map("tomato.shorter","joystick.down","开始前减少 5 分钟");map("tomato.longer","joystick.up","开始前增加 5 分钟");
    r.dashboard.eyebrow.switch_state.ready="准备开始";
    r.dashboard.headline.switch_state.ready="选好时长，再开始。";
    r.dashboard.visualSprite="timer";
    r.dashboard.note="开始前可调 5–60 分钟 · 休息 5 分钟";
    r.dashboard.footer="主键开始/暂停  副键跳过  全局键退出";
    r.surface.elements=nodes.filter(n=>n.bind!=="metricLabel"&&!(n.type==="panel"&&n.y===436&&n.h===1));
    for(const n of nodes){
      if(n.text==="暂停/继续"){delete n.text;n.bind="metricUnit";}
      if(n.text==="跳过"||n.key_for==="tomato.skip_phase")n.state_mask=15;
    }
    for(const n of nodes)if(n.text==="下重计时"){n.text="开始前上下调时长";n.font_size=12;}
    b=b.filter(x=>x.action!=="tomato.reset_phase");
    r.input.confirm={"tomato.reset_phase":"重置本轮计时？已完成的轮数会保留。","tomato.skip_phase":"跳过本轮，进入下一阶段？"};
    notes.push("首次进入不自动计时；开始前可调 5–60 分钟；跳过和重计时必须确认；取消摇杆重置");
  }
  if(id==="drink-reminder"){
    c.description="每隔 15–120 分钟提醒喝水，默认 60 分钟，可暂停或恢复；新间隔在下一次确认喝水后生效。累计实际确认次数，不推算饮水量。";
    map("reminder.switch_previous","knob.rotate_ccw","切换倒计时/记录");
    add("*","reminder.switch_previous",{page:"prev"});
    swipe("reminder.switch_view","reminder.switch_previous");
    r.limits.interval_min=[15,120];r.limits.remaining_min=[0,120];
    add("*","reminder.less",{inc:{interval_min:-15}});
    add("*","reminder.more",{inc:{interval_min:15}});
    map("reminder.less","joystick.down","提醒间隔减少 15 分钟");
    map("reminder.more","joystick.up","提醒间隔增加 15 分钟");
    nodes.push(button("reminder.less","间隔 −",392,378,104),button("reminder.more","间隔 +",506,378,106));
    r.dashboard.note={switch_page:{countdown:"间隔调整在下次确认喝水后生效。",records:"累计确认次数，不会按自然日清零。"}};
    notes.push("左右切换对称；支持 15–120 分钟提醒间隔");
  }
  if(id==="token-usage"){
    b=b.filter(x=>x.event!=="knob.rotate_cw");
    add("*","stats.previous",{page:"prev"});add("*","stats.next",{page:"next"});
    map("stats.previous","knob.rotate_ccw","上一统计页");map("stats.next","knob.rotate_cw","下一统计页");
    swipe("stats.next","stats.previous");
    notes.push("左右顺序切换总量/输入/输出，不再单向跳输出");
  }
  if(id==="hachimi-home"){
    // Separate selecting an object from cycling its variants. Preserve original saved vars.
    r.transitions=r.transitions.filter(t=>!["previous","next"].includes(t.on));
    add("plant","previous",{inc:{plant:-1},set:{editing:1}});
    add("plant","next",{inc:{plant:1},set:{editing:1}});
    add("object","previous",{inc:{object:-1},set:{editing:1}});
    add("object","next",{inc:{object:1},set:{editing:1}});
    add("place","previous",{page:"prev"});add("place","next",{page:"next"});
    add("*","select.plant",{to:"plant",set:{editing:1}});
    add("*","select.object",{to:"object",set:{editing:1}});
    for(const n of nodes)if(n.type==="image"&&n.action)n.action=n.tile?.var==="plant"?"select.plant":"select.object";
    nodes.push(button("previous","上一个",480,366,64),button("next","下一个",552,366,64));
    for(const n of nodes.slice(-2)){n.font_size=12;n.padding=4;n.show={var:"editing",min:1,max:1};}
    const finish=nodes.find(n=>n.action==="finish");finish.text="完成 · 自动保存";finish.font_size=14;
    r.dashboard.note="点选物件，左右换样式；布置自动保存。";
    // Modulo atlas selection supports negative indices; do not clamp at zero.
    if(r.limits){r.limits.plant=[-1000000000,1000000000];r.limits.object=[-1000000000,1000000000];}
    notes.push("点物件选择，左右对称换样式，完成明确自动保存");
  }
  if(id==="bloomfrog_companion"){
    c.description="移动蛙蛙、抚摸和喂食可看到即时回应；抚摸间隔 2 秒、喂食间隔 30 秒，逐步增加亲密度。互动记录保存，无饥饿惩罚。";
    // Movement belongs on the joystick; touch the scene to interact with the frog.
    r.surface.elements=nodes.filter(n=>!["move_up","move_down","move_left","move_right"].includes(n.action));
    r.surface.elements.push({type:"panel",x:32,y:76,w:384,h:240,color:0,opacity:0,action:"pet_pet"});
    r.transitions.find(t=>t.on==="feed_pet").inc.bond=2;
    r.input.cooldown={pet_pet:2000,feed_pet:30000};
    r.dashboard.note="抚摸间隔 2 秒，喂食间隔 30 秒；不减少亲密度。";
    r.surface.elements.push({type:"panel",x:26,y:334,w:588,h:32,color:65502,opacity:225,radius:10});
    r.surface.elements.push({type:"text",x:36,y:340,w:568,h:24,font_size:14,color:10856,bind:"note"});
    notes.push("移除重复方向按钮，仅点击场景抚摸；2 秒抚摸/30 秒喂食冷却，实体与触屏共用");
  }
  if(r.data){
    const previous=b.find(x=>x.event==="knob.rotate_ccw").action,next=b.find(x=>x.event==="knob.rotate_cw").action;
    swipe(next,previous);
  }
  if(id==="hachimi-break")swipe("next","previous");
  if(id==="city-radio")r.input.repeat=["media.up","media.down"];
  if(id==="tomato-clock")Object.assign(r.surface.elements.find(n=>n.bind==="metricValue"),{y:232,h:78,font_size:48});
  for(const n of r.surface?.elements||[]){
    if(!n.action||!r.surface.graphics)continue;
    const mask=r.states.reduce((mask,state,index)=>mask|(r.transitions.some(t=>t.on===n.action&&(t.from==="*"||t.from===state))?(1<<index):0),0);
    if(mask&&mask!==(1<<r.states.length)-1)n.state_mask=mask;
  }
  if(r.surface?.elements.length>32)throw Error(`${id}: surface budget`);
  if(r.transitions.length>24||b.length>8)throw Error(`${id}: input budget`);
  write(path.join(root,"runtime/widget.json"),r);write(path.join(root,"buttons.json"),b);write(path.join(root,"component.json"),c);
  const share=read(path.join(root,"share.json"));share.summary=c.description;write(path.join(root,"share.json"),share);
  const n=read(path.join(root,"negative-screen.json")),dashboard=appDashboard(r,initialApp(r));
  for(const [k,v] of Object.entries(dashboard))if(k!=="progress")n.dashboard[k]=v;
  write(path.join(root,"negative-screen.json"),n);
  report.push({id,version:c.version,notes,status:"generated-not-validated"});
}
write(path.join(path.dirname(out),"input-resource-report.json"),report);
console.log(`Generated ${report.length} upgrades; not installed or published.`);
