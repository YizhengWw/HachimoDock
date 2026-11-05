/* Real view-model regression: agent identity is not the compact pet status. */
#include <assert.h>
#include <string.h>
#include "pet_p4_view.h"
static pet_p4_runtime_state_t state;
static void prepare(const char *agent,const char *life,const char *body) {
  memset(&state,0,sizeof(state));strcpy(state.screen_page,"main");
  strcpy(state.active_agent,agent);strcpy(state.current_state,life);
  strcpy(state.current_status,life);strcpy(state.current_speech,body);
  strcpy(state.current_status_text,"arbitrary Agent-name/statusText");
}
int main(void) {
  const char *agents[]={"deepseek-harness","codex","workbuddy","claude-code","future-agent"};
  const char *life[]={"idle","working","tool_running","waiting_user","done","error","stopped","speaking"};
  const char *label[]={"休息中","工作中","工作中","等待操作","已完成","出错了","已停止","工作中"};
  pet_p4_view_model_t v;
  for(unsigned a=0;a<sizeof(agents)/sizeof(*agents);a++)
    for(unsigned n=0;n<sizeof(life)/sizeof(*life);n++) {
      prepare(agents[a],life[n],"");pet_p4_runtime_state_t before=state;
      pet_p4_build_view_model(&state,&v);
      assert(v.compact_bubble&&!v.realtime_conversation);
      assert(!v.title[0]);
      if(!strcmp(life[n],"done"))assert(!v.show_bubble&&!v.body[0]);
      else assert(v.show_bubble&&!strcmp(v.body,label[n]));
      assert(!strcmp(v.agent,agents[a])&&!memcmp(&state,&before,sizeof(state)));
      /* A title without content is metadata; a real nonempty body remains
       * real text even if its value happens to equal a provider name. */
      strcpy(state.current_title,"ProviderIdentity");
      pet_p4_build_view_model(&state,&v);assert(v.compact_bubble);
      if(!strcmp(life[n],"done"))assert(!v.show_bubble&&!v.title[0]&&!v.body[0]);
      else assert(!v.title[0]&&!strcmp(v.body,label[n]));
      strcpy(state.current_speech,"DeepSeek Harness");
      pet_p4_build_view_model(&state,&v);
      assert(!strcmp(v.body,"DeepSeek Harness"));
    }
  /* Full session/conversation text remains content, not a status label. */
  prepare("future-agent","working","Long task content exceeding the compact bubble byte limit and retained as body");
  strcpy(state.current_title,"Actual task title");
  pet_p4_build_view_model(&state,&v);assert(!v.compact_bubble);
  assert(!strcmp(v.title,"Actual task title")&&!strcmp(v.body,state.current_speech));
  prepare("deepseek-harness","working","");state.session_voice_active=true;
  pet_p4_build_view_model(&state,&v);assert(v.show_voice_overlay&&!v.show_bubble);
  prepare("deepseek-harness","working","");strcpy(state.conversation_state,"listening");
  strcpy(state.conversation_name,"Pet");strcpy(state.conversation_text,"Actual local conversation caption");
  pet_p4_build_view_model(&state,&v);assert(v.realtime_conversation&&!v.compact_bubble);
  assert(!strcmp(v.body,"Actual local conversation caption"));
  pet_p4_build_view_model(NULL,&v);assert(!v.show_bubble&&!v.show_voice_overlay);
  return 0;
}
