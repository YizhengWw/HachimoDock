"""Extract the installed official IDF SPIFFS/VFS directory error path unchanged."""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function


def test_sdk_directory_errors_and_shared_error_interleaving(tmp_path):
    sdk = Path.home() / ".platformio/packages/framework-espidf/components/spiffs"
    if not (sdk / "esp_spiffs.c").exists():
        pytest.skip("local official ESP-IDF SPIFFS sources unavailable")
    vfs = (sdk / "esp_spiffs.c").read_text()
    core = (sdk / "spiffs/src/spiffs_hydrogen.c").read_text()
    public = (sdk / "spiffs/src/spiffs.h").read_text()
    nucleus = (sdk / "spiffs/src/spiffs_nucleus.h").read_text()
    constant_names = {"SPIFFS_OK", "SPIFFS_VIS_END", "SPIFFS_VIS_NO_WRAP", "SPIFFS_RDONLY",
                      "SPIFFS_O_RDONLY", "SPIFFS_UNDEFINED_LEN", "SPIFFS_OBJ_ID_IX_FLAG"}
    constants = "\n".join(line for line in (public + "\n" + nucleus).splitlines()
                          if line.startswith("#define ") and
                          (line.split()[1] in constant_names or line.split()[1].startswith("SPIFFS_ERR_")))
    macro_lines = nucleus.splitlines()
    macro_start = next(i for i, line in enumerate(macro_lines)
                       if line.startswith("#define SPIFFS_API_CHECK_RES_UNLOCK("))
    macro_end = macro_start
    while macro_lines[macro_end].endswith("\\"):
        macro_end += 1
    checked_error_macro = "\n".join(macro_lines[macro_start:macro_end + 1])
    definitions = "\n".join(function(core, name) for name in
        ("SPIFFS_errno", "SPIFFS_clearerr", "spiffs_hydro_read", "SPIFFS_read",
         "SPIFFS_opendir", "SPIFFS_readdir", "SPIFFS_closedir"))
    definitions += "\n" + "\n".join(function(vfs, name) for name in
        ("spiffs_res_to_errno", "vfs_spiffs_readdir_r", "vfs_spiffs_readdir", "vfs_spiffs_closedir"))
    # The installed official SDK changes when root switches the two build
    # environments. 5.5.1 has no special EOF mapping; 5.5.4 does. Exercise the
    # actual installed implementation without replacing its behavior in tests.
    eof_rescan = "case SPIFFS_ERR_END_OF_OBJECT" not in function(vfs, "vfs_spiffs_readdir_r")
    source = f"#define EXPECT_EOF_RESCAN {int(eof_rescan)}\n" + constants + "\n" + checked_error_macro + r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef int32_t s32_t;typedef uint8_t u8_t;typedef uint32_t u32_t;typedef uint16_t spiffs_obj_id;
typedef unsigned spiffs_block_ix;typedef int spiffs_file;
enum{SPIFFS_OBJ_NAME_LEN=32};
typedef struct{int err_code;bool mounted;}spiffs;
typedef struct{int flags;uint32_t size,fdoffset;}spiffs_fd;
typedef struct{spiffs*fs;unsigned block;int entry;}spiffs_DIR;
struct spiffs_dirent{unsigned obj_id;uint8_t name[32];unsigned type;};
typedef struct{spiffs*fs;}esp_spiffs_t;
typedef struct{spiffs_DIR d;struct dirent e;char path[32];unsigned offset;}vfs_spiffs_dir_t;
static int visitor_result;
static bool concurrent_clear;
static bool enumerate,persistent_read_error;
static unsigned depth;
void SPIFFS_clearerr(spiffs*);
#define SPIFFS_API_DBG(...) ((void)0)
#define SPIFFS_CHECK_MOUNT(fs) ((fs)->mounted)
#define SPIFFS_CHECK_CFG(fs) ((void)(fs),true)
#define SPIFFS_FH_UNOFFS(fs,fh) ((void)(fs),(fh))
#define SPIFFS_API_CHECK_CFG(fs) ((void)(fs))
#define SPIFFS_API_CHECK_MOUNT(fs) if(!(fs)->mounted){(fs)->err_code=SPIFFS_ERR_NOT_MOUNTED;return SPIFFS_ERR_NOT_MOUNTED;}
#define SPIFFS_LOCK(fs) do{(void)(fs);assert(!depth);depth++;}while(0)
/* Another reader may clear the shared fs errno after SPIFFS unlock, before
 * the VFS wrapper inspects it. A scan eligibility epoch must detect overlap. */
#define SPIFFS_UNLOCK(fs) do{assert(depth==1);depth--;if(concurrent_clear)SPIFFS_clearerr(fs);}while(0)
#define SPIFFS_CACHE_WR 0
static spiffs_fd file_descriptor={.flags=SPIFFS_O_RDONLY,.size=4,.fdoffset=4};
static int spiffs_fd_get(spiffs*fs,spiffs_file fh,spiffs_fd**out){(void)fs;(void)fh;*out=&file_descriptor;return SPIFFS_OK;}
static int spiffs_object_read(spiffs_fd*fd,int offset,int len,u8_t*buf){(void)fd;(void)offset;(void)len;(void)buf;return SPIFFS_OK;}
#define spiffs_read_dir_v NULL
static int spiffs_obj_lu_find_entry_visitor(spiffs*fs,unsigned b,int e,int flags,int id,void*visitor,void*constant,void*out,unsigned*bix,int*entry){
 (void)fs;(void)b;(void)flags;(void)id;(void)visitor;(void)constant;
 if(enumerate){
  if(e)return persistent_read_error?-12345:SPIFFS_VIS_END;
  struct spiffs_dirent*d=out;strcpy((char*)d->name,"/target");d->type=1;d->obj_id=1;*bix=0;*entry=0;return SPIFFS_OK;
 }
 if(visitor_result==SPIFFS_OK){struct spiffs_dirent*d=out;strcpy((char*)d->name,"/target");d->type=1;d->obj_id=1;*bix=0;*entry=1;}
 return visitor_result;
}
''' + definitions + r'''
static spiffs *scan_fs;
static esp_spiffs_t scan_context;
static unsigned scan_opens,scan_closes,scan_removes;
static DIR *sdk_open(const char*path){
 scan_opens++;vfs_spiffs_dir_t*d=calloc(1,sizeof(*d));assert(d);
 assert(SPIFFS_opendir(scan_fs,path,&d->d));strcpy(d->path,"/");return(DIR*)d;
}
static struct dirent *sdk_read(DIR*d){return vfs_spiffs_readdir(&scan_context,d);}
static int sdk_close(DIR*d){scan_closes++;return vfs_spiffs_closedir(&scan_context,d);}
static bool current(void*ctx){(void)ctx;return true;}
static bool idle(void*ctx,uint64_t*out){(void)ctx;*out=7;return true;}
static bool checked_remove(const char*path){assert(!strcmp(path,"store/target")||!strcmp(path,"store/absent"));scan_removes++;return true;}
#define opendir sdk_open
#define readdir sdk_read
#define closedir sdk_close
#include "pet_p4_file_cleanup.c"
#undef opendir
#undef readdir
#undef closedir
int main(void){
 spiffs fs={.mounted=true};esp_spiffs_t context={.fs=&fs};
 vfs_spiffs_dir_t*dir=calloc(1,sizeof(*dir));assert(dir);dir->d.fs=&fs;strcpy(dir->path,"/");
 visitor_result=SPIFFS_OK;errno=0;struct dirent*entry=vfs_spiffs_readdir(&context,(DIR*)dir);
 assert(entry&&!strcmp(entry->d_name,"target")&&!errno);
 visitor_result=-12345;errno=0;assert(!vfs_spiffs_readdir(&context,(DIR*)dir)&&errno==EIO);
 visitor_result=SPIFFS_VIS_END;errno=0;assert(!vfs_spiffs_readdir(&context,(DIR*)dir)&&!errno);
 visitor_result=-12345;concurrent_clear=true;errno=0;
 assert(!vfs_spiffs_readdir(&context,(DIR*)dir)&&!errno); // demonstrates why non-overlap is required
 concurrent_clear=false;fs.mounted=false;errno=0;
 assert(!vfs_spiffs_readdir(&context,(DIR*)dir)&&errno==ENODEV);
 assert(vfs_spiffs_closedir(&context,(DIR*)dir)==-1&&errno==ENODEV);
 fs.mounted=true;dir=calloc(1,sizeof(*dir));assert(dir);dir->d.fs=&fs;
 assert(!vfs_spiffs_closedir(&context,(DIR*)dir));assert(!depth);
 /* Real successful file EOF sets the SPIFFS error internally. In official
  * 5.5.1, a normal directory VIS_END subsequently maps that stale value to
  * EIO, then clears it. No genuine directory read error is injected here. */
 uint8_t byte=0;fs.err_code=SPIFFS_OK;
 assert(SPIFFS_read(&fs,1,&byte,1)==0&&fs.err_code==SPIFFS_ERR_END_OF_OBJECT);
 scan_fs=&fs;scan_context.fs=&fs;enumerate=true;
 pet_file_cleanup_target_t targets[]={{"store/target"},{"store/absent"}};
 pet_file_cleanup_guard_t guard={.owner_current=current,.idle_epoch=idle,.remove_path=checked_remove};
 pet_file_cleanup_plan_t plan;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,2,guard)&&plan.snapshot_used&&plan.present==1);
 assert(plan.diagnostic.attempts==(EXPECT_EOF_RESCAN?2:1));
 assert(scan_opens==plan.diagnostic.attempts&&scan_closes==scan_opens&&!scan_removes);
 assert(plan.diagnostic.epoch_before==7&&plan.diagnostic.epoch_after==7&&!plan.diagnostic.activity);
 if(EXPECT_EOF_RESCAN) {
  assert(plan.diagnostic.first_reason==PET_CLEANUP_READ_FAILED&&plan.diagnostic.first_read_errno==EIO);
  assert(plan.diagnostic.first_entries==1);
 } else assert(plan.diagnostic.first_reason==PET_CLEANUP_SNAPSHOT);
 assert(!plan.diagnostic.read_errno&&fs.err_code==SPIFFS_OK);
 assert(pet_file_cleanup_remove_at(&plan,0)&&pet_file_cleanup_remove_at(&plan,1)&&scan_removes==1);
 /* A genuine recurring lookup read failure does not become a false EOF.
  * Both independent scans fail; original per-target removal is retained. */
 persistent_read_error=true;scan_opens=scan_closes=scan_removes=0;
 assert(pet_file_cleanup_prepare(&plan,"store",targets,2,guard)&&!plan.snapshot_used&&plan.present==3);
 assert(plan.diagnostic.attempts==2&&scan_opens==2&&scan_closes==2&&!scan_removes);
 assert(plan.diagnostic.first_read_errno==EIO&&plan.diagnostic.read_errno==EIO);
 assert(pet_file_cleanup_remove_at(&plan,0)&&pet_file_cleanup_remove_at(&plan,1)&&scan_removes==2);
 assert(!depth);return 0;
}
'''
    path=tmp_path/"sdk_dir.c";path.write_text(source);binary=tmp_path/"sdk_dir"
    subprocess.run(["cc","-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-I",str(Path(__file__).resolve().parents[1]/"main"),
                    str(path),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)
