"""Exercise the production PCM parser and legacy/default classification on host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_default_status_cues_are_not_embedded_in_the_firmware():
    """The OTA slot is reserved for the Wi-Fi stack; the desktop keeps playing its own WAVs."""
    cmake = (ROOT / "main/CMakeLists.txt").read_text()
    ini = (ROOT / "platformio.ini").read_text()
    source = (ROOT / "main/pet_p4_system_cues.c").read_text()
    for text in (cmake, ini):
        assert "terrier-clips/done.wav" not in text and "terrier-clips/error.wav" not in text
        assert "foreach(cue done error)" not in text
    assert "_binary_" not in source


def test_shared_cues_parse_and_preserve_unknown_custom_audio():
    harness = r'''
#include "pet_p4_system_cues.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  assert(argc==4);
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
  /* No built-in sound: every family is silent unless the appearance ships custom audio. */
  const unsigned char *shared = (const unsigned char *) 1; size_t shared_size = 7;
  assert(!pet_p4_system_cue_pcm("done", &shared, &shared_size) && !shared && shared_size == 0);
  assert(!pet_p4_system_cue_pcm("error", &shared, &shared_size));
  assert(!pet_p4_system_cue_pcm("waiting_user", &shared, &shared_size));
  assert(pet_p4_system_cue_family("done"));
  assert(pet_p4_system_cue_family("error"));
  assert(pet_p4_system_cue_family("waiting_user"));
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
                        for n in ("done", "error", "waiting_user")]], check=True)


def test_playback_is_not_conditioned_on_matching_animation():
    source = (ROOT / "main/pet_p4_audio.c").read_text()
    process = source.split("void pet_p4_audio_process(", 1)[1].split("esp_err_t pet_p4_audio_set_enabled", 1)[0]
    assert "pet_p4_canonical_lifecycle" in process
    assert "pet_p4_system_cue_family(lifecycle) ? lifecycle" in process
    assert "if (index < 0 || index >= state->asset_catalog.count) return" not in process
    assert "pet_p4_audio_conversation_active()" in process
    assert "!request.audio_custom" in source
    assert "pet_p4_system_cue_legacy(request.family, checksum)" in source
