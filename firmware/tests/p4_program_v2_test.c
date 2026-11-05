/* p4-component-program-v2 host harness: real descriptor parser, real pure runtime, real cells core.
 * Modes: parse <json> -> {"ok":..,"v2":..}; run <json> reads {"runtime":..,"events":[..]} and
 * executes the program through the product runtime, mirroring the grid like the device does. */
#include "pet_p4_component_program.h"
#include "pet_p4_program_runtime.h"
#include "pet_p4_cells.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool always(void *c,uint64_t g){(void)c;return g==1;}
static int var_index(const cJSON *vars,const char *name){int i=0;const cJSON *v;cJSON_ArrayForEach(v,vars){if(!strcmp(v->string,name))return i;i++;}return -1;}
int main(int argc,char **argv){
  if(argc!=2)return 2;
  static char input[262144];size_t length=fread(input,1,sizeof(input)-1,stdin);input[length]=0;
  cJSON *root=cJSON_Parse(input);if(!root)return 3;
  const cJSON *runtime=!strcmp(argv[1],"run")?cJSON_GetObjectItemCaseSensitive(root,"runtime"):root;
  static pet_component_program_config_t config;
  bool ok=pet_component_program_parse(runtime,&config);
  if(strcmp(argv[1],"run")){printf("{\"ok\":%s,\"enabled\":%s,\"v2\":%s}\n",ok?"true":"false",config.enabled?"true":"false",config.v2?"true":"false");cJSON_Delete(root);return 0;}
  if(!ok||!config.enabled){printf("{\"ok\":false}\n");cJSON_Delete(root);return 0;}
  /* Device seed: outputs from the variable inits, VM cells [0,cols*rows) from the grid init. */
  const cJSON *vars=cJSON_GetObjectItemCaseSensitive(runtime,"vars"),*grid=cJSON_GetObjectItemCaseSensitive(runtime,"cells");
  int32_t values[8]={0};int count=0;const cJSON *v;
  cJSON_ArrayForEach(v,vars){const cJSON *init=cJSON_GetObjectItemCaseSensitive(v,"init");values[count++]=cJSON_IsNumber(init)?init->valueint:0;}
  static pet_cells_t cells;
  if(grid){
    if(!pet_cells_init(&cells,cJSON_GetObjectItemCaseSensitive(grid,"cols")->valueint,cJSON_GetObjectItemCaseSensitive(grid,"rows")->valueint,cJSON_GetObjectItemCaseSensitive(grid,"bits")->valueint)){cJSON_Delete(root);return 5;}
    const cJSON *init=cJSON_GetObjectItemCaseSensitive(grid,"init");int y=0;
    cJSON_ArrayForEach(v,init){if(!pet_cells_init_row(&cells,y++,v->valuestring)){cJSON_Delete(root);return 6;}}
  }
  static pet_program_snapshot_t seed;seed.schema=config.image.schema;seed.cells=config.image.cells;
  for(unsigned i=0;i<8;i++){unsigned at=config.projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND)seed.outputs[i]=values[at];}
  if(grid)pet_cells_export(&cells,seed.values,PET_PROGRAM_CELLS);
  if(!pet_component_program_seed_valid(&config,&seed)){cJSON_Delete(root);return 7;}
  static pet_program_runtime_t rt;pet_program_runtime_init(&rt);
  pet_program_output_contract_t contract;
  for(unsigned i=0;i<8;i++){unsigned at=config.projection.target[i];contract.minimum[i]=at==PET_PROGRAM_UI_UNBOUND?INT32_MIN:config.projection.minimum[at];contract.maximum[i]=at==PET_PROGRAM_UI_UNBOUND?INT32_MAX:config.projection.maximum[at];}
  if(pet_program_runtime_start_with_contract(&rt,&config.image,1,always,NULL,PET_PROGRAM_QUANTUM,&seed,&contract)!=PET_RUNTIME_OK){cJSON_Delete(root);return 8;}
  uint64_t sequence=0;const cJSON *event;
  cJSON_ArrayForEach(event,cJSON_GetObjectItemCaseSensitive(root,"events")){
    const char *action=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event,"action"));
    pet_program_event_t words;
    if(!pet_component_program_event(&config,action,values,&words)){
      /* Not a program action: apply the ordinary transition inc/set on the cursor variables. */
      const cJSON *t;cJSON_ArrayForEach(t,cJSON_GetObjectItemCaseSensitive(runtime,"transitions")){
        if(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t,"on")),action))continue;
        const cJSON *inc=cJSON_GetObjectItemCaseSensitive(t,"inc"),*e;
        cJSON_ArrayForEach(e,inc){int i=var_index(vars,e->string);if(i<0){cJSON_Delete(root);return 9;}values[i]+=e->valueint;
          const cJSON *lim=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(runtime,"limits"),e->string);
          if(lim){int lo=cJSON_GetArrayItem(lim,0)->valueint,hi=cJSON_GetArrayItem(lim,1)->valueint;if(values[i]<lo)values[i]=lo;if(values[i]>hi)values[i]=hi;}}
      }
      continue;
    }
    if(pet_program_runtime_enqueue(&rt,1,++sequence,&words)!=PET_RUNTIME_OK){cJSON_Delete(root);return 10;}
    for(unsigned turn=0;turn<4096;turn++){
      pet_program_runtime_result_t r=pet_program_runtime_pump(&rt,1);
      if(r==PET_RUNTIME_COMPLETE){pet_program_report_t report;if(pet_program_runtime_take_report(&rt,1,&report)!=PET_RUNTIME_OK){cJSON_Delete(root);return 11;}
        if(report.result!=PET_PROGRAM_COMMITTED){printf("{\"ok\":false,\"result\":%d}\n",(int)report.result);cJSON_Delete(root);return 0;}break;}
      if(r!=PET_RUNTIME_OK&&r!=PET_RUNTIME_YIELDED&&r!=PET_RUNTIME_IDLE){cJSON_Delete(root);return 12;}
      if(turn==4095){cJSON_Delete(root);return 13;}
    }
    static pet_program_snapshot_t snap;if(pet_program_runtime_snapshot(&rt,1,&snap)!=PET_RUNTIME_OK){cJSON_Delete(root);return 14;}
    for(unsigned i=0;i<8;i++){unsigned at=config.projection.target[i];if(at!=PET_PROGRAM_UI_UNBOUND)values[at]=snap.outputs[i];}
    if(grid)pet_cells_import(&cells,snap.values,(unsigned)cells.cols*cells.rows,NULL);
  }
  static pet_program_snapshot_t final;if(pet_program_runtime_snapshot(&rt,1,&final)!=PET_RUNTIME_OK){cJSON_Delete(root);return 15;}
  printf("{\"ok\":true,\"outputs\":{");int i=0;bool first=true;
  cJSON_ArrayForEach(v,vars){if(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(v,"type")),"int")){printf("%s\"%s\":%ld",first?"":",",v->string,(long)values[i]);first=false;}i++;}
  printf("},\"cells\":[");for(unsigned k=0;k<final.cells;k++)printf("%s%ld",k?",":"",(long)final.values[k]);
  printf("],\"grid\":[");
  if(grid)for(unsigned y=0;y<cells.rows;y++){printf("%s\"",y?",":"");for(unsigned x=0;x<cells.cols;x++)printf("%x",cells.values[y*cells.cols+x]);printf("\"");}
  printf("],\"fuel\":%u}\n",(unsigned)PET_PROGRAM_FUEL);
  cJSON_Delete(root);return 0;
}
