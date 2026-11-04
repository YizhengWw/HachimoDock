// Bind image budgets to an actual generated ESP-IDF table, including its MD5.
import {createHash} from 'node:crypto';
const common={nvs:[1,2,0x9000,0x6000],phy_init:[1,1,0xf000,0x1000]};
const layouts=[
 ['p4-legacy-appearance20m-v0',{...common,ota_0:[0,16,0x10000,0x280000],ota_1:[0,17,0x290000,0x280000],otadata:[1,0,0x510000,0x2000],storage:[1,130,0x520000,0x6e0000],appearance0:[1,64,0xc00000,0xa00000],appearance1:[1,65,0x1600000,0xa00000]}],
 ['p4-storage-plus2m-appearance18m-v1',{...common,ota_0:[0,16,0x10000,0x280000],ota_1:[0,17,0x290000,0x280000],otadata:[1,0,0x510000,0x2000],storage:[1,130,0x520000,0x8e0000],appearance0:[1,64,0xe00000,0x900000],appearance1:[1,65,0x1700000,0x900000]}],
 ['p4-storage-plus2m-ota3m-appearance17m-v2',{...common,ota_0:[0,16,0x10000,0x300000],ota_1:[0,17,0x310000,0x300000],otadata:[1,0,0x610000,0x2000],storage:[1,130,0x620000,0x8e0000],appearance0:[1,64,0xf00000,0x880000],appearance1:[1,65,0x1780000,0x880000]}],
];
export function partitionArtifact(bytes) {
 const fail=()=>{throw Error('invalid_or_unreviewed_partition_table');};
 if(!Buffer.isBuffer(bytes)||![3072,4096].includes(bytes.length))fail();
 const rows={},ranges=[];let end=0;
 for(;end+32<=bytes.length;end+=32){
  if(bytes.readUInt16LE(end)!==0x50aa)break;
  const nameBytes=bytes.subarray(end+12,end+28),nul=nameBytes.indexOf(0);
  if(nul<1||nameBytes.subarray(nul).some(b=>b!==0))fail();
  const name=nameBytes.subarray(0,nul).toString('ascii');
  if(Object.hasOwn(rows,name)||Object.keys(rows).length>=8||bytes.readUInt32LE(end+28)!==0)fail();
  const row=[bytes[end+2],bytes[end+3],bytes.readUInt32LE(end+4),bytes.readUInt32LE(end+8)];
  if(row[2]%4096||!row[3]||row[3]%4096||row[2]+row[3]>0x2000000)fail();
  rows[name]=row;ranges.push([row[2],row[2]+row[3]]);
 }
 if(Object.keys(rows).length!==8||end+32>bytes.length||bytes.readUInt16LE(end)!==0xebeb||bytes.subarray(end+2,end+16).some(b=>b!==255)
   ||!createHash('md5').update(bytes.subarray(0,end)).digest().equals(bytes.subarray(end+16,end+32))||bytes.subarray(end+32).some(b=>b!==255))fail();
 ranges.sort((a,b)=>a[0]-b[0]);if(ranges.some((r,i)=>i&&r[0]<ranges[i-1][1]))fail();
 const layout=layouts.find(([,expected])=>Object.keys(expected).every(k=>JSON.stringify(rows[k])===JSON.stringify(expected[k])));
 if(!layout)fail();
 return {id:layout[0],sha256:createHash('sha256').update(bytes).digest('hex'),flashBytes:0x2000000,otaBytes:rows.ota_0[3],storageBytes:rows.storage[3],appearanceBytes:rows.appearance0[3]+rows.appearance1[3],md5Verified:true};
}
