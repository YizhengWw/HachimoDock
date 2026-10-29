// Server-owned UI. No native APIs, credentials, file paths or arbitrary bridge commands.
const root=document.querySelector('#app'), embedded=window.parent!==window;
const categories=[['','全部'],['hachimi','哈基米专属'],['game','小游戏'],['productivity','效率'],['tool','工具'],['relax','放松'],['life','生活']];
let hostOrigin='',channel='',context=null,serial=0,epoch=0,dirty=false;
const pending=new Map(), state={view:'discover',category:'',query:'',order:'featured',offset:0,metric:'downloads',window:'7d'};
const memory=new Map();
function read(key,fallback){try{return JSON.parse(localStorage.getItem(key))??fallback;}catch{return memory.get(key)??fallback;}}
function save(key,value){localStorage.setItem(key,JSON.stringify(value));memory.set(key,value);}
function nickname(value){const name=String(value??'').trim();if(!name||[...name].length>40||/[\p{Cc}\u202a-\u202e\u2066-\u2069]/u.test(name))throw Error('请输入 1–40 个字符的昵称，不包含控制字符');return name;}
function localContext(){
  let identity=read('community-profile',null);
  if(!identity){const code=`用户-${crypto.randomUUID().replaceAll('-','').slice(0,12)}`;identity={code,name:'',displayName:code};try{save('community-profile',identity);}catch{/* usable for this session */}}
  return {identity,favorites:read('community-favorites',[])};
}
function node(tag,props={},...children){const el=document.createElement(tag);for(const [key,value] of Object.entries(props)){if(value==null)continue;if(key==='class')el.className=value;else if(key.startsWith('on'))el.addEventListener(key.slice(2).toLowerCase(),value);else if(key==='text')el.textContent=value;else if(key in el&&!key.startsWith('aria-'))el[key]=value;else el.setAttribute(key,String(value));}for(const child of children.flat(Infinity))if(child!=null)el.append(child instanceof Node?child:document.createTextNode(String(child)));return el;}
const button=(text,fn,cls='')=>node('button',{type:'button',class:cls,onClick:fn},text);
function toast(message){const el=document.querySelector('#toast');el.textContent=message;clearTimeout(toast.timer);toast.timer=setTimeout(()=>el.textContent='',6500);}
function setDirty(value){dirty=value;if(embedded&&channel)parent.postMessage({type:'pet-community-dirty',v:1,channel,value},hostOrigin);}
function bridge(action,fields={}){return new Promise((resolve,reject)=>{if(!channel)return reject(Error('客户端连接尚未就绪，请重试'));const requestId=`web-${++serial}`;const timer=setTimeout(()=>{pending.delete(requestId);reject(Error('操作回执超时，请核对结果后重试'));},180000);pending.set(requestId,{resolve,reject,timer});parent.postMessage({type:'pet-community-request',v:1,channel,requestId,action,fields},hostOrigin);});}
window.addEventListener('message',event=>{
  if(!embedded||event.source!==parent)return;
  const data=event.data;
  if(!data||data.v!==1)return;
  if(data.type==='pet-community-init'&&!channel){
    if(!['tauri://localhost','http://tauri.localhost','https://tauri.localhost',location.origin].includes(event.origin))return;
    if(typeof data.channel!=='string'||!data.context?.identity)return;
    channel=data.channel;hostOrigin=event.origin;context=data.context;state.view=data.view==='favorites'?'favorites':'discover';
    renderRoute();return;
  }
  if(event.origin!==hostOrigin||data.channel!==channel)return;
  if(data.type==='pet-community-response'){
    const call=pending.get(data.requestId);if(!call)return;pending.delete(data.requestId);clearTimeout(call.timer);
    data.error?call.reject(Error(data.error)):call.resolve(data.result);
  }
});
let clientToken=read('community-browser-token','');
if(!/^[a-f0-9]{64}$/.test(clientToken)){clientToken=Array.from(crypto.getRandomValues(new Uint8Array(32)),v=>v.toString(16).padStart(2,'0')).join('');try{save('community-browser-token',clientToken);}catch{/* session only */}}
async function api(action,fields={}){
  if(embedded)return bridge(action,fields);
  if(action==='profile'){const name=nickname(fields.name);const identity={...context.identity,name,displayName:name};save('community-profile',identity);context.identity=identity;return identity;}
  if(action==='favorite'){
    const work=await api('detail',{id:fields.id});const values=context.favorites.some(x=>x.id===work.id)?context.favorites.filter(x=>x.id!==work.id):[{id:work.id,name:work.name},...context.favorites].slice(0,200);
    save('community-favorites',values);context.favorites=values;return values;
  }
  const params=new URLSearchParams();let path='',body;
  if(action==='browse'){for(const [a,b] of [['query','q'],['category','category'],['order','order'],['offset','offset']])if(fields[a]!=null)params.set(b,fields[a]);path='/works';}
  else if(action==='rankings'){for(const key of ['metric','window'])if(fields[key])params.set(key,fields[key]);path='/rankings';}
  else if(['detail','comments','like','comment'].includes(action)){
    if(!/^[a-z][a-z0-9_-]{0,43}$/.test(fields.id))throw Error('组件标识无效');
    path=`/works/${fields.id}`;
    if(action==='comments'){path+='/comments';params.set('offset',fields.offset||0);}
    if(action==='like'){path+='/like';body={active:fields.active};}
    if(action==='comment'){path+='/comments';body={name:context.identity.displayName,text:fields.text,requestId:fields.requestId};}
  }else throw Error('请在 Pet Manager 中下载、配置和同步组件');
  // Read-only requests may retry once. Writes are never automatically replayed.
  for(let attempt=0;attempt<(body?1:2);attempt++){
    try{
      const response=await fetch(`/api/v1${path}?${params}`,{method:body?'POST':'GET',headers:{'X-Community-Client':clientToken,...(body?{'Content-Type':'application/json'}:{})},body:body?JSON.stringify(body):undefined,credentials:'omit',signal:AbortSignal.timeout(20000)});
      const value=await response.json();if(!response.ok){const error=Error(value.error||'社区请求失败');error.status=response.status;error.retry=response.status>=500;throw error;}return value;
    }catch(error){if(body||attempt||error.retry===false||(!error.retry&&!['TypeError','TimeoutError'].includes(error.name)))throw error;await new Promise(r=>setTimeout(r,450));}
  }
}
function image(hash,name){return /^[a-f0-9]{64}$/.test(hash||'')?node('img',{class:'cover',src:`/api/v1/images/${hash}`,alt:name,loading:'lazy',onError:e=>{e.target.removeAttribute('src');e.target.alt='封面暂不可用';}}):node('div',{class:'cover'});}
function isSaved(id){return context.favorites.some(x=>x.id===id);}
async function toggleFavorite(id,control){control.disabled=true;try{context.favorites=await api('favorite',{id});control.textContent=isSaved(id)?'已收藏':'收藏';control.setAttribute('aria-pressed',String(isSaved(id)));}catch(e){toast(e.message);}finally{control.disabled=false;}}
function card(work){if(!work||work.available===false)return null;const bookmark=button(isSaved(work.id)?'已收藏':'收藏',()=>toggleFavorite(work.id,bookmark),'bookmark');bookmark.setAttribute('aria-pressed',String(isSaved(work.id)));bookmark.setAttribute('aria-label',`收藏 ${work.name}`);
  return node('article',{class:'card'},node('button',{class:'card-open',onClick:()=>navigate(work.id)},image(work.covers?.[0],work.name),node('div',{class:'card-copy'},node('h3',{},work.name),node('p',{},work.summary||'这个玩法暂不可用'),node('span',{class:'muted'},`by ${work.author||'社区创作者'}`))),node('div',{class:'card-foot'},node('span',{},`↓ ${work.downloads||0}　♡ ${work.likes||0}`),bookmark));}
function errorBox(error,retry){return node('div',{class:'error',role:'alert'},node('p',{},error.message||'暂时无法连接社区，本机组件仍可使用。'),button('重试',retry));}
function confirmLeave(proceed){
  if(!dirty){proceed();return;}
  const dialog=node('dialog',{'aria-label':'离开未保存内容'},node('h3',{},'离开当前页面？'),node('p',{},'未发送的评论和未保存的昵称不会保留。'));
  dialog.append(node('div',{class:'actions'},button('继续编辑',()=>{dialog.close();dialog.remove();}),button('放弃并离开',()=>{dialog.close();dialog.remove();setDirty(false);proceed();},'primary')));document.body.append(dialog);dialog.showModal();
}
function navigate(id=''){confirmLeave(()=>{setDirty(false);location.hash=id?`work=${encodeURIComponent(id)}`:state.view;});}
function heading(title,subtitle){return node('header',{class:'heading'},node('div',{},node('span',{class:'eyebrow'},'SMALL WIDGETS, BIG DELIGHT'),node('h1',{},title,node('span',{class:'accent'},'。')),node('p',{class:'muted'},subtitle)),node('div',{class:'mark','aria-hidden':'true'},'✧'));}
function shell(){const wrap=node('div',{class:'wrap'});root.replaceChildren(wrap);if(!embedded){const nav=node('nav',{class:'standalone-nav'},button('社区广场',()=>{state.view='discover';navigate();}),button('我的收藏',()=>{state.view='favorites';navigate();}));wrap.append(nav);}return wrap;}
async function loadFavoritePage(favorites,offset){
  let failedCount=0;
  const items=await Promise.all(favorites.slice(offset,offset+18).map(async item=>{
    try{const work=await api('detail',{id:item.id});return work?.available===false?null:work;}
    catch(error){
      // Older native bridges preserve the server's error text, but not HTTP status.
      // Never rebuild an entry point from stale saved metadata, even when offline.
      if(error.status!==404&&error.message!=='组件不存在或已下架')failedCount++;
      return null;
    }
  }));
  return {items:items.filter(Boolean),failedCount,nextOffset:offset+18<favorites.length?offset+18:null};
}
async function renderList(){const turn=++epoch,wrap=shell(),favorites=state.view==='favorites';wrap.append(heading(favorites?'留住喜欢的玩法':'给桌面，添点新玩法',favorites?'收藏保存在当前客户端或浏览器，不会自动安装。':'发现好玩的灵感，把喜欢的带回你的哈基米。'));
  const content=node('div'),grid=node('div',{class:'grid'}),rank=node('aside',{class:'panel'}),layout=node('div',{class:'layout'},content,rank);
  if(!favorites){const cats=node('div',{class:'categories'},categories.map(([id,label])=>{const b=button(label,()=>{state.category=id;state.offset=0;renderList();},state.category===id?'active':'');b.setAttribute('aria-pressed',String(state.category===id));return b;}));const search=node('input',{'aria-label':'搜索社区组件',placeholder:'搜索玩法或创作者',value:state.query,maxLength:80});const form=node('form',{class:'search',onSubmit:e=>{e.preventDefault();state.query=search.value.trim();state.offset=0;renderList();}},search,node('button',{type:'submit'},'搜索'));wrap.append(node('div',{class:'toolbar'},cats,form));}
  const select=node('select',{'aria-label':'作品排序',onChange:e=>{state.order=e.target.value;state.offset=0;renderList();}},node('option',{value:'featured'},'精选优先'),node('option',{value:'latest'},'最近更新'));select.value=state.order;
  content.append(node('div',{class:'title-row'},node('h3',{},favorites?'我的收藏':'社区新鲜玩法'),!favorites?select:null),grid);wrap.append(layout);grid.append(node('p',{class:'loading'},'正在加载…'));loadRank(rank,turn);
  try{
    const result=favorites?await loadFavoritePage(context.favorites,state.offset):await api('browse',state);
    if(turn!==epoch)return;grid.replaceChildren(...result.items.map(card).filter(Boolean));
    if(result.failedCount)grid.append(errorBox(Error('部分收藏暂时无法加载，请稍后重试。收藏记录未更改。'),renderList));
    if(!result.items.length&&!result.failedCount)grid.append(node('p',{class:'empty favorite-empty'},favorites?(context.favorites.length?'本页没有可展示的收藏；已下架作品不再显示，收藏记录未更改。':'还没有收藏，去广场发现喜欢的玩法吧。'):'没有找到匹配的玩法，试试其他分类或关键词。'));
    content.append(node('nav',{class:'pagination','aria-label':'组件分页'},state.offset>0?button('上一页',()=>{state.offset=Math.max(0,state.offset-18);renderList();}):null,result.nextOffset!=null?button('下一页',()=>{state.offset=result.nextOffset;renderList();}):null));
  }catch(e){if(turn===epoch)grid.replaceChildren(errorBox(e,renderList));}
}
async function loadRank(container,turn){container.replaceChildren(node('h3',{},'人气榜单'));
  const metric=node('select',{'aria-label':'榜单类型',onChange:e=>{state.metric=e.target.value;loadRank(container,turn);}},node('option',{value:'downloads'},'下载榜'),node('option',{value:'likes'},'点赞榜'));metric.value=state.metric;
  const period=node('select',{'aria-label':'榜单时间',onChange:e=>{state.window=e.target.value;loadRank(container,turn);}},node('option',{value:'7d'},'近 7 天'),node('option',{value:'24h'},'近 24 小时'),node('option',{value:'all'},'全部'));period.value=state.window;container.append(node('div',{class:'actions'},metric,period));const rows=node('div');container.append(rows);const key=`${state.metric}:${state.window}`;
  try{const result=await api('rankings',state);if(turn!==epoch||key!==`${state.metric}:${state.window}`)return;rows.replaceChildren(...result.items.map((work,i)=>node('button',{class:'rank',onClick:()=>navigate(work.id)},node('b',{},String(i+1).padStart(2,'0')),node('span',{},work.name),node('small',{},work.rankValue))));if(!result.items.length)rows.append(node('p',{class:'empty'},'等待每一份真实热爱'));}catch(e){if(turn===epoch)rows.replaceChildren(errorBox(e,()=>loadRank(container,turn)));}
  container.append(node('p',{class:'muted'},'下载校验完成后计数；同一设备与版本 24 小时内去重。点赞可取消。'));
}
async function renderDetail(id){const turn=++epoch,wrap=shell();wrap.append(button('← 返回广场',()=>navigate(),'back'));const area=node('div');wrap.append(area);area.append(node('p',{class:'loading'},'正在加载玩法…'));
  try{const work=await api('detail',{id});if(turn!==epoch)return;const main=node('article'),side=node('aside',{class:'panel detail-side'});area.replaceChildren(node('div',{class:'detail'},main,side));main.append(node('div',{class:'shots'},(work.covers||[]).map(hash=>image(hash,work.name))),node('p',{class:'muted'},'作者提供的界面示意图，实际状态以设备为准。'),node('h2',{},work.name),node('p',{class:'muted'},`by ${work.author} · ${categories.find(x=>x[0]===work.category)?.[1]||''}`),node('p',{},work.summary));
    for(const [title,body] of [['怎么玩',work.description],[`本次更新 · ${work.version}`,work.releaseNotes],['授权说明',work.license]])main.append(node('h3',{},title),node('p',{class:'pre'},body||'—'));
    // No external navigation from the embedded surface. The source remains copyable text.
    if(work.sourceUrl)main.append(node('p',{class:'source'},`作者来源：${work.sourceUrl}`));
    side.append(node('span',{class:'eyebrow'},'PETUI 组件资源包'),node('h3',{},'把玩法带到桌面'),node('p',{},`社区版本 ${work.version}`),node('p',{class:'muted'},`${Math.max(1,Math.round((work.packageBytes||0)/1024))} KB · 下载仅加入本机，不自动修改设备。`));
    const status=node('div',{class:'requirements muted'});side.append(status);
    const download=button(embedded?'下载到我的组件':'请在 Pet Manager 中下载',async()=>{download.disabled=true;try{await api('download',{id});toast('已校验并加入我的组件，可配置与同步到设备。');await localState();}catch(e){toast(e.message);}finally{download.disabled=false;await localState();}},'primary');download.disabled=!embedded;side.append(download);
    const configure=button('配置与同步到设备',async()=>{try{await api('configure',{id});}catch(e){toast(e.message);}});configure.hidden=true;side.append(configure);
    async function localState(){if(!embedded)return;try{const value=await api('status',{id});if(turn!==epoch)return;status.replaceChildren(node('p',{},value.localLabel),node('p',{},value.deviceLabel));configure.hidden=!value.hasLocal;download.disabled=value.blocked;download.textContent=value.downloadLabel;}catch{status.textContent='本机状态暂不可用，下载前将重新核对。';}}
    await localState();if(turn!==epoch)return;
    const like=button(`♡ ${work.likes}`,async()=>{like.disabled=true;try{await api('like',{id,active:!work.liked});const fresh=await api('detail',{id});Object.assign(work,fresh);like.textContent=`${work.liked?'♥':'♡'} ${work.likes}`;like.setAttribute('aria-pressed',String(!!work.liked));}catch(e){toast(e.message);}finally{like.disabled=false;}});like.setAttribute('aria-pressed',String(!!work.liked));const favorite=button(isSaved(id)?'已收藏':'收藏',()=>toggleFavorite(id,favorite));favorite.setAttribute('aria-pressed',String(isSaved(id)));side.append(node('div',{class:'social'},like,favorite),node('p',{class:'muted'},`↓ ${work.downloads} 次下载`));
    const count=node('span',{class:'muted'},`${work.commentCount||0} 条评论`),comments=node('div',{class:'comments'});main.append(node('div',{class:'title-row comments-heading'},node('h3',{},'评论'),count),comments);
    let next=null,loaded=0;const more=button('加载更多评论',()=>loadComments(false),'small');more.hidden=true;main.append(more);
    async function loadComments(reset=true){more.disabled=true;try{const result=await api('comments',{id,offset:reset?0:next});if(turn!==epoch)return;if(reset){comments.replaceChildren();loaded=0;}for(const comment of result.items){if([...comments.children].some(x=>x.dataset.id===comment.id))continue;const row=node('article',{},node('strong',{},comment.name),node('time',{},new Date(comment.at*1000).toLocaleDateString('zh-CN')),node('p',{},comment.text));row.dataset.id=comment.id;comments.append(row);loaded++;}next=result.nextOffset;more.hidden=next==null;}catch(e){toast(e.message);}finally{more.disabled=false;}}
    loadComments();main.append(commentForm(id,async()=>{await loadComments();const fresh=await api('detail',{id});count.textContent=`${fresh.commentCount} 条评论`;}));
  }catch(e){if(turn===epoch)area.replaceChildren(errorBox(e,()=>renderDetail(id)));}
}
function commentForm(id,onPosted){let busy=false,editing=false,attempt=null;const name=node('strong',{'aria-label':'评论身份'},context.identity.displayName),editor=node('div',{class:'editor',hidden:true}),error=node('p',{role:'alert'}),body=node('textarea',{'aria-label':'评论内容',placeholder:'聊聊玩法、使用建议或遇到的问题',maxLength:500,rows:3}),counter=node('span',{class:'muted'},'0/500');
  const form=node('form',{class:'comment-form',onSubmit:async event=>{event.preventDefault();if(busy||editing||!body.value.trim())return;busy=true;update();error.textContent='';const text=body.value.trim(),fingerprint=JSON.stringify([id,context.identity.displayName,text]);if(attempt?.fingerprint!==fingerprint)attempt={fingerprint,requestId:crypto.randomUUID()};try{await api('comment',{id,text,requestId:attempt.requestId});body.value='';counter.textContent='0/500';attempt=null;setDirty(false);await onPosted();}catch(e){error.textContent=e.message;}finally{busy=false;update();}}});
  const submit=node('button',{type:'submit',class:'primary',disabled:true},'发表评论');
  const edit=button('编辑昵称',()=>{if(busy)return;editing=true;editor.hidden=false;input.value=context.identity.name;error.textContent='';update();input.focus();});
  const input=node('input',{'aria-label':'昵称',maxLength:40,placeholder:'输入昵称',onKeydown:event=>{if(event.isComposing)return;if(event.key==='Enter'){event.preventDefault();saveName();}if(event.key==='Escape'){event.preventDefault();finish();}}});
  function update(){submit.disabled=busy||editing||!body.value.trim();edit.disabled=busy||editing;body.disabled=busy;setDirty(Boolean(body.value.trim())||editing||busy);}
  function finish(){editing=false;editor.hidden=true;error.textContent='';update();edit.focus();}
  async function saveName(){if(busy)return;try{const value=nickname(input.value);busy=true;update();saveButton.disabled=true;cancel.disabled=true;context.identity=await api('profile',{name:value});name.textContent=context.identity.displayName;busy=false;finish();}catch(e){error.textContent=e.message;}finally{busy=false;saveButton.disabled=false;cancel.disabled=false;update();}}
  const saveButton=button('保存',saveName,'small'),cancel=button('取消',finish,'small');editor.append(input,saveButton,cancel);
  body.addEventListener('input',()=>{counter.textContent=`${body.value.length}/500`;update();});form.append(node('div',{class:'identity'},node('span',{class:'avatar','aria-hidden':'true'},'☺'),name,edit),editor,body,error,node('div',{class:'comment-actions'},counter,submit));return form;
}
function renderRoute(){if(!context)return;setDirty(false);const match=/^#work=([a-z][a-z0-9_-]{0,43})$/.exec(location.hash);if(match)renderDetail(match[1]);else{if(location.hash==='#favorites')state.view='favorites';else if(location.hash==='#discover')state.view='discover';renderList();}}
let previousHash=location.hash;
window.addEventListener('hashchange',()=>{const target=location.hash;if(dirty){history.replaceState(null,'',previousHash||location.pathname);confirmLeave(()=>{location.hash=target;});return;}previousHash=target;renderRoute();});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue='';}});
if(embedded){parent.postMessage({type:'pet-community-ready',v:1},'*');setTimeout(()=>{if(!channel)root.replaceChildren(errorBox(Error('客户端连接未就绪，请返回后重新打开社区。'),()=>location.reload()));},12000);}else{context=localContext();renderRoute();}
