#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_surface.h"
static void emit(cJSON *root){char *s=cJSON_PrintUnformatted(root);puts(s?s:"{}");cJSON_free(s);cJSON_Delete(root);}
int main(int argc,char **argv){
  if(argc!=2)return 2;
  char *raw=calloc(65537,1);if(!raw)return 3;
  size_t n=fread(raw,1,65536,stdin);if(n==65536){free(raw);return 4;}
  cJSON *root=cJSON_Parse(raw),*out=cJSON_CreateObject();free(raw);
  if(!root||!out){cJSON_Delete(root);cJSON_Delete(out);return 5;}
  bool ok=false;
  if(!strcmp(argv[1],"surface")){
    pet_surface_t *surface=calloc(1,sizeof(*surface));if(!surface)return 6;
    ok=pet_surface_parse(cJSON_GetObjectItemCaseSensitive(root,"surface"),surface);
#if PET_HOST_COLLECTION_ENABLED
    pet_host_collection_snapshot_t *snapshot=calloc(1,sizeof(*snapshot));if(!snapshot)return 7;
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(root,"snapshot");
    bool valid=value&&pet_host_collection_parse_snapshot(value,snapshot);
    if(ok)pet_surface_resolve_collection(surface,valid?snapshot:NULL,valid);
    cJSON_AddBoolToObject(out,"modelValid",valid);
    cJSON *nodes=cJSON_AddArrayToObject(out,"nodes");
    if(ok)for(unsigned i=0;i<surface->count;i++){
      const pet_surface_node_t *n=&surface->nodes[i];cJSON *v=cJSON_CreateObject();cJSON_AddItemToArray(nodes,v);
      cJSON_AddNumberToObject(v,"type",n->type);cJSON_AddNumberToObject(v,"x",n->x);cJSON_AddNumberToObject(v,"y",n->y);
      cJSON_AddNumberToObject(v,"w",n->w);cJSON_AddNumberToObject(v,"h",n->h);cJSON_AddNumberToObject(v,"page",n->page);
      cJSON_AddNumberToObject(v,"slot",n->collection_slot);cJSON_AddNumberToObject(v,"field",n->collection_field);
      cJSON_AddNumberToObject(v,"color",n->color);cJSON_AddNumberToObject(v,"stroke",n->stroke);
      cJSON_AddBoolToObject(v,"hidden",n->hidden);cJSON_AddBoolToObject(v,"disabled",n->disabled);
      cJSON_AddStringToObject(v,"text",n->text);cJSON_AddStringToObject(v,"action",n->hidden||n->disabled?"":n->action);
    }
    free(snapshot);
#endif
    free(surface);
  }
#if PET_HOST_COLLECTION_ENABLED
  else if(!strcmp(argv[1],"config")){
    pet_host_collection_config_t config={0};ok=pet_host_collection_parse_config(root,&config);
  }else if(!strcmp(argv[1],"snapshot")){
    pet_host_collection_snapshot_t *snapshot=calloc(1,sizeof(*snapshot));if(!snapshot)return 7;
    ok=pet_host_collection_parse_snapshot(root,snapshot);free(snapshot);
  }
#endif
  cJSON_AddBoolToObject(out,"ok",ok);emit(out);cJSON_Delete(root);return 0;
}
