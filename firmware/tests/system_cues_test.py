"""Exercise the production PCM parser and legacy/default classification on host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_identical_default_cues_are_embedded_only_once():
    clips = ROOT.parent / "pc/public/terrier-clips"
    assert (clips / "error.wav").read_bytes() == (clips / "waiting_user.wav").read_bytes()
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    assert "PET_ERROR_CUE_SHA STREQUAL PET_WAIT_CUE_SHA" in cmake
    assert "foreach(cue done error)" not in cmake
    assert '"pet_p4_cue_packed.c"' in cmake
    assert 'compress_system_cues.py" "--check"' in cmake
    assert "foreach(cue permission_request" not in cmake
    assert "../pc/public/terrier-clips/waiting_user.wav" not in (ROOT / "platformio.ini").read_text()
    source = (ROOT / "main/pet_p4_system_cues.c").read_text()
    assert "_binary_waiting_user_wav" not in source


def test_shared_cues_parse_and_preserve_unknown_custom_audio():
    harness = r'''
#include "pet_p4_system_cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Host test exercises the original WAV parser on canonical resource bytes. */
/* This test isolates the existing WAV parser; the packed decoder and complete
 * seven-family lookup are exercised with real zlib by p4_cue_packed_test. */
bool pet_p4_cue_packed_init(void) {return false;}
bool pet_p4_cue_packed_pcm(unsigned index,const uint8_t **pcm,size_t *size) {(void)index;(void)pcm;(void)size;return false;}
int main(int argc, char **argv) {
  assert(argc==9);
  for (int i=1;i<argc;i++) {
    FILE *f=fopen(argv[i],"rb"); assert(f);
    fseek(f,0,SEEK_END); size_t size=(size_t)ftell(f); rewind(f);
    unsigned char *wav=malloc(size); assert(wav); assert(fread(wav,1,size,f)==size); fclose(f);
    const unsigned char *pcm=NULL; size_t count=0;
    assert(pet_p4_cue_parse_wav(wav,size,&pcm,&count));
    assert(count>0 && !(count&1) && pcm>=wav && pcm+count<=wav+size);
    assert(!pet_p4_cue_parse_wav(wav,20,&pcm,&count));
    wav[0]=0; assert(!pet_p4_cue_parse_wav(wav,size,&pcm,&count)); free(wav);
  }
  assert(pet_p4_system_cue_family("done"));
  assert(pet_p4_system_cue_family("error"));
  assert(pet_p4_system_cue_family("waiting_user"));
  assert(pet_p4_system_cue_family("permission_request"));
  assert(pet_p4_system_cue_family("permission_approved"));
  assert(pet_p4_system_cue_family("permission_denied"));
  assert(pet_p4_system_cue_family("permission_expired"));
  assert(pet_p4_system_cue_family("session_terminated"));
  assert(!pet_p4_system_cue_family("working"));
  assert(pet_p4_system_cue_legacy("done",UINT64_C(0x392e208e77b6a2a4)));
  assert(pet_p4_system_cue_legacy("done",UINT64_C(0x3800fd80e9e2461f)));
  assert(!pet_p4_system_cue_legacy("done",UINT64_C(12345)));
  assert(!pet_p4_system_cue_legacy("custom",UINT64_C(0x3800fd80e9e2461f)));
  return 0;
}'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        source = path / "test.c"
        source.write_text(harness)
        binary = path / "test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                        str(ROOT / "main/pet_p4_system_cues.c"), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), *[str(ROOT.parent / "pc/public/terrier-clips" / (n + ".wav"))
                        for n in ("done", "error", "waiting_user", "permission_request", "permission_approved", "permission_denied", "permission_expired", "session_terminated")]], check=True)


def test_playback_is_not_conditioned_on_matching_animation():
    source = (ROOT / "main/pet_p4_audio.c").read_text()
    process = source.split("void pet_p4_audio_process(", 1)[1].split("esp_err_t pet_p4_audio_set_enabled", 1)[0]
    assert "pet_p4_canonical_lifecycle" in process
    assert "pet_p4_system_cue_family(lifecycle)" in process
    assert "pet_audio_patch_global_enabled(lifecycle)" in process
    assert 'if (!family[0]) return;' in process
    assert "if ((!entry || !entry->audio_path[0])" not in process
    assert "if (index < 0 || index >= state->asset_catalog.count) return" not in process
    assert "pet_p4_audio_conversation_active()" in process
    assert "!request.audio_custom" in source
    assert "pet_p4_system_cue_legacy(request.family, checksum)" in source


def test_permission_cues_are_event_driven_on_approval():
    protocol = (ROOT / "main/pet_p4_protocol.c").read_text()
    inp = (ROOT / "main/pet_p4_input.c").read_text()
    audio = (ROOT / "main/pet_p4_audio.c").read_text()
    assert 'pet_p4_audio_play_event_cue("permission_request")' in protocol
    assert 'pet_p4_audio_play_event_cue("permission_approved")' in protocol
    assert 'pet_p4_audio_play_event_cue("permission_denied")' in protocol
    assert 'pet_p4_audio_play_event_cue("permission_expired")' in protocol
    assert 'pet_p4_audio_play_event_cue(\n    allow ? "permission_approved" : "permission_denied"\n  )' in inp
    assert "pet_p4_audio_play_event_cue(const char *family)" in audio
