/* Prints module.json. chain_params / ui_hierarchy come from the plugin's own
 * parameter table so the manifest and the code cannot disagree. The layout
 * follows the modules already running on a Move (tinysid, linein, scratch):
 * ui_hierarchy lives under capabilities and its params are keys into chain_params.
 * Usage: gen_manifest > module.json   (scripts/gen_manifest.sh) */
#include "../src/schwung_plugin.h"

#include <stdio.h>

int main(void)
{
    static char cp[8192], ui[4096];
    if (ts_plugin_json_chain_params(cp, sizeof cp) < 0 ||
        ts_plugin_json_ui_hierarchy(ui, sizeof ui) < 0) {
        fprintf(stderr, "gen_manifest: JSON buffer too small\n");
        return 1;
    }
    printf("{\n"
           "\"id\":\"tinyscratch\",\n"
           "\"name\":\"TinyScratch\",\n"
           "\"abbrev\":\"TSCR\",\n"
           "\"version\":\"0.9.3\",\n"
           "\"description\":\"A generative auto-scratcher for the Ableton Move: press a pad and it writes a new, human-sounding scratch for your sample, plays it in time with your song, and lets you keep the ones you like. MIDI notes at C3 and above cut the sound in like a crossfader.\",\n"
           "\"author\":\"TinyScratch\",\n"
           "\"dsp\":\"dsp.so\",\n"
           "\"api_version\":2,\n"
           "\"component_type\":\"sound_generator\",\n"
           "\"capabilities\":{\n"
           "\"audio_in\":false,\n"
           "\"audio_out\":true,\n"
           "\"midi_in\":true,\n"
           "\"midi_out\":false,\n"
           "\"chainable\":true,\n"
           "\"claims_ccs\":[62,63],\n"
           "\"component_type\":\"sound_generator\",\n"
           "\"requires_continuous_processing\":true,\n"
           "\"chain_params\":%s,\n"
           "\"ui_hierarchy\":%s\n"
           "}\n"
           "}\n",
           cp, ui);
    return 0;
}
