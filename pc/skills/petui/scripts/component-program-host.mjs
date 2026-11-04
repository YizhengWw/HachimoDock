// Explicit synthetic stdin only. Error output never echoes user data.
import {smokeComponentProgram} from './component-program-smoke.mjs';
try{
 const chunks=[];let size=0;for await(const chunk of process.stdin){size+=chunk.length;if(size>65536)throw Error();chunks.push(chunk);}
 const data=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(Buffer.concat(chunks)));
 if(!data||Object.keys(data).length!==2||!Object.hasOwn(data,'runtime')||!Array.isArray(data.buttons))throw Error();
 process.stdout.write(JSON.stringify(smokeComponentProgram(data.runtime,data.buttons))+'\n');
}catch{process.stdout.write(JSON.stringify({ok:false,errors:['Component program verification input or execution invalid'],deviceVerified:false,hostServicesVerified:false})+'\n');process.exitCode=1;}
