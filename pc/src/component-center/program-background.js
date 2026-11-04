// One UI owner, bounded single-flight IPC. No package execution or auto-start.
const idValid=s=>typeof s==='string'&&/^[A-Za-z0-9._-]{1,47}$/.test(s);
const widgetIdValid=s=>typeof s==='string'&&/^[a-z][a-z0-9_-]{0,46}$/.test(s);
const bootValid=s=>typeof s==='string'&&/^[0-9a-f]{16}$/.test(s);
const serialValid=s=>typeof s==='string'&&/^[1-9][0-9]{0,19}$/.test(s)&&(s.length<20||s<='18446744073709551615');
const integer=(n,max=4294967295)=>Number.isInteger(n)&&n>=0&&n<=max;
export const BACKGROUND_IDLE=Object.freeze({status:'idle',snapshot:null,pending:false,checking:false,code:''});
export function supportsBackgroundPrograms(caps={}){
  const known=s=>s==='p4-component-program-v1'||s==='p4-component-program-v2';
  // The primary negotiation is a known scalar. An array alone, or a future
  // primary plus a familiar array member, never grants execution support.
  if(!known(caps?.widgetProgram))return false;
  if(caps.widgetPrograms!==undefined&&(!Array.isArray(caps.widgetPrograms)||caps.widgetPrograms.length<1||caps.widgetPrograms.length>2
    ||!caps.widgetPrograms.every(known)||new Set(caps.widgetPrograms).size!==caps.widgetPrograms.length||!caps.widgetPrograms.includes(caps.widgetProgram)))return false;
  return caps?.widgetProgramBackground==='p4-program-background-v2'
    &&caps?.widgetProgramInspect==='p4-program-inspect-v1'&&integer(caps?.widgetProgramBackgroundMaxActive,4)&&caps.widgetProgramBackgroundMaxActive>0;
}
export function supportsBackgroundProgramEngine(caps,engine){
  return supportsBackgroundPrograms(caps)&&(engine==='p4-component-program-v1'||engine==='p4-component-program-v2')
    &&(caps.widgetPrograms===undefined?caps.widgetProgram===engine:caps.widgetPrograms.includes(engine));
}
export function backgroundSnapshot(value){
  if(!value||!bootValid(value.bootId)||!Array.isArray(value.items)||value.items.length>4)throw Error('invalid_background_snapshot');
  const ids=new Set(),serials=new Set();
  const items=value.items.map(row=>{
    if(!row||!widgetIdValid(row.widgetId)||!serialValid(row.registration)||ids.has(row.widgetId)||serials.has(row.registration)
      ||typeof row.live!=='boolean'||typeof row.stopped!=='boolean'||typeof row.checkpointPending!=='boolean'
      ||(row.live&&row.stopped)||!integer(row.revision)||!integer(row.phase,255)||!integer(row.lastStepUs)||!integer(row.maxStepUs)||row.lastStepUs>row.maxStepUs)throw Error('invalid_background_entry');
    ids.add(row.widgetId);serials.add(row.registration);
    return Object.freeze({widgetId:row.widgetId,registration:row.registration,live:row.live,stopped:row.stopped,
      checkpointPending:row.checkpointPending,revision:row.revision,phase:row.phase,lastStepUs:row.lastStepUs,maxStepUs:row.maxStepUs});
  });
  return Object.freeze({bootId:value.bootId,items:Object.freeze(items)});
}
export function createProgramBackgroundController({board,invoke,onChange=()=>{}}){
  if(!idValid(board)||typeof invoke!=='function')throw Error('invalid_background_owner');
  let alive=true,token={},reading=null,pending=false,state=BACKGROUND_IDLE;
  const publish=value=>{if(alive){state=Object.freeze(value);onChange(state);}return state;};
  const ready=()=>alive&&!pending&&!reading&&state.status==='ready'&&state.snapshot;
  const check=()=>{if(!alive)throw Error('background_owner_disposed');if(!ready())throw Error('background_state_not_ready');};
  async function refresh(){
    if(!alive||pending)return state;
    if(reading)return reading;
    const current=token;
    publish({...state,checking:true});
    reading=Promise.resolve().then(()=>invoke('usb_get_background_programs',{expectedBoardDeviceId:board})).then(value=>{
      const snapshot=backgroundSnapshot(value);
      if(alive&&current===token)publish({status:'ready',snapshot,pending:false,checking:false,code:''});
    }).catch(()=>{
      if(alive&&current===token)publish({...state,status:'error',checking:false,code:'query_unavailable'});
    }).finally(()=>{reading=null;});
    await reading;return state;
  }
  async function perform(action,id,observed){
    check();if(!widgetIdValid(id))throw Error('invalid_background_id');
    if(action==='stop'&&(!observed||!bootValid(observed.bootId)||!serialValid(observed.registration)
      ||state.snapshot.bootId!==observed.bootId||!state.snapshot.items.some(row=>row.widgetId===id&&row.registration===observed.registration)))throw Error('background_owner_changed');
    const current=token={};pending=true;publish({...state,pending:true,code:''});
    const payload={expectedBoardDeviceId:board,componentId:id,...(action==='stop'?{bootId:observed.bootId,registration:observed.registration}:{})};
    try{
      const result=await invoke(action==='start'?'usb_start_background_program':'usb_stop_background_program',payload);
      let snapshot;
      if(result?.confirmed===true){
        snapshot=backgroundSnapshot(result.snapshot);
        const row=snapshot.items.find(row=>row.widgetId===id);
        if(action==='start'?!row?.live||row.stopped:!row?.stopped||row.live||snapshot.bootId!==observed.bootId||row.registration!==observed.registration)throw Error('unconfirmed_background_owner');
      }
      if(alive&&current===token)publish(snapshot?{status:'ready',snapshot,pending:false,checking:false,code:action==='start'?'running':'stopped'}:
        {...state,status:'unknown',pending:false,checking:false,code:'result_unknown_no_replay'});
    }catch{
      if(alive&&current===token)publish({...state,status:'unknown',pending:false,checking:false,code:'result_unknown_no_replay'});
    }finally{pending=false;}
    return state;
  }
  return Object.freeze({getState:()=>state,refresh,start:id=>perform('start',id),stop:(id,observed)=>perform('stop',id,observed),
    dispose(){alive=false;token={};}});
}
