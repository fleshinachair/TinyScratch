#ifndef SCHWUNG_PLUGIN_H
#define SCHWUNG_PLUGIN_H

/* Glue between Schwung's plugin_api_v2 and the scratcher engine.
 *
 * The structs below mirror src/host/plugin_api_v1.h from charlesvestal/schwung
 * (member order and signatures as published there). They are ts_-prefixed so
 * this module builds without the Schwung tree; the host only ever sees the
 * pointer returned by move_plugin_init_v2, so layout is what matters. */

#include <stddef.h>
#include <stdint.h>

#define TS_PLUGIN_API_V2 2
#define TS_CLOCK_RUNNING 2

typedef struct ts_host_api {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    void *mod_emit_value;      /* move_mod_emit_value_fn   */
    void *mod_clear_source;    /* move_mod_clear_source_fn */
    void *mod_host_ctx;
    float (*get_bpm)(void);
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    double (*get_beat_position)(void);
    void *reserved[8];
} ts_host_api_t;

typedef struct ts_plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} ts_plugin_api_v2_t;

#if defined(_WIN32)
#define TS_EXPORT __declspec(dllexport)
#else
#define TS_EXPORT __attribute__((visibility("default")))
#endif

TS_EXPORT const ts_plugin_api_v2_t *move_plugin_init_v2(const ts_host_api_t *host);

/* Table-driven JSON; module.json is generated from these (tools/gen_manifest.c). */
int ts_plugin_json_chain_params(char *buf, int cap);
int ts_plugin_json_ui_hierarchy(char *buf, int cap);

#endif
