/* Independent of React/Vite chunks. Never logs text, URL queries or app data. */
(function () {
  'use strict';
  var first=null,firstCode=null,ready=false,reloading=false;
  function category(value){return value==='resource'?'resource':value==='timeout'?'timeout':value==='render'?'render':'javascript';}
  function show(kind,code){
    if(!first){first=category(kind);firstCode=['ReferenceError','TypeError','SyntaxError','RangeError'].indexOf(code)>=0?code:'unknown';}
    var panel=document.getElementById('startup-recovery');
    if(!panel){panel=document.createElement('section');panel.id='startup-recovery';document.body.appendChild(panel);}
    panel.setAttribute('role','alert');
    panel.style.cssText='position:fixed;inset:0;z-index:2147483647;background:#f6f7fb;color:#202637;padding:48px;font:16px system-ui;overflow:auto';
    panel.replaceChildren();
    var title=document.createElement('h1');title.textContent='界面暂时无法显示';panel.appendChild(title);
    var text=document.createElement('p');text.textContent='界面加载或显示发生异常（'+first+'/'+firstCode+'）。这不代表设备断连，也未确认任何正在进行操作的结果。';panel.appendChild(text);
    var note=document.createElement('p');note.textContent='重新加载只重载本页面，不重发安装、删除或其他操作。若刚有同步/删除，请加载后先刷新设备清单核对结果。';panel.appendChild(note);
    var button=document.createElement('button');button.textContent='仅重新加载界面';button.type='button';button.addEventListener('click',function(){
      if(reloading)return;
      if(!window.confirm('仅重新加载页面。未保存的界面编辑会丢失；不会重发操作。若设备操作尚未结束，请先等待，加载后核对设备清单。'))return;
      reloading=true;button.disabled=true;window.location.reload();
    });panel.appendChild(button);
    // Fixed category only: never emit exception.message, stack, form text or paths.
    console.error('[startup]',first,firstCode);
  }
  window.__PET_STARTUP__={fail:show,ready:function(){ready=true;var panel=document.getElementById('startup-loading');if(panel)panel.remove();}};
  window.addEventListener('error',function(event){
    var target=event.target;
    if(target&&target!==window){if(target.tagName==='SCRIPT'||target.tagName==='LINK'&&target.rel==='stylesheet')show('resource');return;}
    show('javascript',event.error&&event.error.name);
  },true);
  window.addEventListener('unhandledrejection',function(event){show('javascript',event.reason&&event.reason.name);});
  window.setTimeout(function(){if(!ready)show('timeout');},20000);
}());
