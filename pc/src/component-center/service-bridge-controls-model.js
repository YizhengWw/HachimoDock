// Presentation-only conversion. The native provider remains the authority.
export function bridgeEventArguments(command,draft={}){
 const data={};
 for(const argument of Object.values(command.arguments||{})){
  const key=argument.value?.event;if(!key)continue;
  const raw=draft[key];let value;
  if(argument.type==='int'){
   if(raw===undefined||raw===null||String(raw).trim()==='')throw Error('请输入范围内的整数');
   value=Number(raw);
   if(!Number.isSafeInteger(value)||value<(argument.min??-1e9)||value>(argument.max??1e9))throw Error('请输入范围内的整数');
  }else if(argument.type==='bool'){
   value=raw??false;if(typeof value!=='boolean')throw Error('请选择有效开关值');
  }else if(argument.type==='string'){
   value=raw??'';
   if(typeof value!=='string'||new TextEncoder().encode(value).length>63||/[\u0000-\u001f\u007f-\u009f]/.test(value))throw Error('文本不能超过63 UTF-8字节或包含控制字符');
  }else throw Error('服务未注册此参数类型');
  data[key]=value;
 }
 return data;
}
