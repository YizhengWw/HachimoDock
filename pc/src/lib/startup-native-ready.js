/** One document captures its native generation before loading App; one ready IPC.
 * No timers, reloads, service actions or retries. request is injected for tests. */
export function createStartupHandshake(request,isNative) {
  let captured=null,confirmation=null;
  function capture(){
    if(!isNative())return Promise.resolve(null);
    if(!captured)captured=Promise.resolve().then(()=>request('frontend_startup_generation')).then(value=>{
      if(!Number.isSafeInteger(value?.generation)||value.generation<1||typeof value.token!=='string'||value.token.length>64||!value.token.length)
        throw new TypeError('invalid startup challenge');
      return {generation:value.generation,token:value.token};
    });
    return captured;
  }
  return {
    capture,
    confirm(){
      if(!confirmation)confirmation=capture().then(value=>value===null?true:request('frontend_startup_ready',value));
      return confirmation;
    },
  };
}
