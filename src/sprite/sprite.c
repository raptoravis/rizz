#include "rizz/sprite.h"

#include "sx/allocator.h"
#include "sx/array.h"
#include "sx/handle.h"
#include "sx/hash.h"
#include "sx/os.h"
#include "sx/pool.h"
#include "sx/string.h"

#include "rizz/app.h"
#include "rizz/asset.h"
#include "rizz/core.h"
#include "rizz/graphics.h"
#include "rizz/imgui-extra.h"
#include "rizz/imgui.h"
#include "rizz/json.h"
#include "rizz/plugin.h"
#include "rizz/reflect.h"

#include <float.h>
#include <limits.h>

#include <alloca.h>

#include rizz_shader_path(shaders_h, sprite.frag.h)
#include rizz_shader_path(shaders_h, sprite.vert.h)
#include rizz_shader_path(shaders_h, sprite_wire.vert.h)
#include rizz_shader_path(shaders_h, sprite_wire.frag.h)

#define MAX_VERTICES 2000
#define MAX_INDICES 6000
#define ANIMCTRL_PARAM_ID_END INT_MAX

RIZZ_STATE static rizz_api_core* the_core;
RIZZ_STATE static rizz_api_plugin* the_plugin;
RIZZ_STATE static rizz_api_asset* the_asset;
RIZZ_STATE static rizz_api_refl* the_refl;
RIZZ_STATE static rizz_api_gfx* the_gfx;
RIZZ_STATE static rizz_api_imgui* the_imgui;
RIZZ_STATE static rizz_api_imgui_extra* the_imguix;

typedef struct sprite__data {
    sx_str_t name;
    rizz_asset atlas;
    int atlas_sprite_id;
    rizz_asset texture;
    sx_vec2 size;    // size set by API (x or y can be <= 0)
    sx_vec2 origin;
    sx_color color;
    rizz_sprite_flip flip;
    rizz_sprite_animclip clip;
    rizz_sprite_animctrl ctrl;
    sx_rect draw_bounds;    // cropped
    sx_rect bounds;
} sprite__data;

typedef struct atlas__sprite {
    sx_vec2 base_size;
    sx_rect sprite_rect;
    sx_rect sheet_rect;
    int num_indices;
    int num_verts;
    int ib_index;
    int vb_index;
} atlas__sprite;

typedef struct atlas__data {
    rizz_atlas a;
    atlas__sprite* sprites;
    sx_hashtbl sprite_tbl;    // key: name, value:index-to-sprites
    rizz_sprite_vertex* vertices;
    uint16_t* indices;
} atlas__data;

typedef struct atlas__metadata {
    char img_filepath[RIZZ_MAX_PATH];
    int num_sprites;
    int num_vertices;
    int num_indices;
} atlas__metadata;

typedef struct sprite__animclip_frame {
    int16_t atlas_id;
    int16_t trigger;
    rizz_event e;
} sprite__animclip_frame;

typedef struct sprite__animclip {
    rizz_asset atlas;
    int num_frames;
    float tm;
    float fps;
    float len;
    int frame_id;
    rizz_sprite_flip flip;
    bool trigger_end_event;
    bool end_triggered;
    rizz_event_queue equeue;
    const sx_alloc* alloc;

#if RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES > 0
    sprite__animclip_frame frames[RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES];
#else
    sprite__animclip_frame* frames;
#endif
} sprite__animclip;

typedef struct sprite__animctrl_transition sprite__animctrl_transition;

typedef struct sprite__animctrl_state {
    char name[32];
    rizz_sprite_animclip clip;
    int num_transitions;
    sprite__animctrl_transition* transitions;
} sprite__animctrl_state;

typedef union {
    int i;
    float f;
    bool b;
} sprite__animctrl_value;

typedef struct sprite__animctrl_trigger {
    int param_id;
    rizz_sprite_animctrl_compare_func func;
    sprite__animctrl_value value;
} sprite__animctrl_trigger;

typedef struct sprite__animctrl_transition {
    sprite__animctrl_state* target;
    sprite__animctrl_trigger trigger;
    bool trigger_event;
    rizz_event event;
} sprite__animctrl_transition;

typedef struct sprite__animctrl_param {
    char name[32];
    uint32_t name_hash;
    rizz_sprite_animctrl_param_type type;
    sprite__animctrl_value value;
} sprite__animctrl_param;

typedef struct sprite__animctrl {
    const sx_alloc* alloc;
    sprite__animctrl_state* state;
    sprite__animctrl_state* start_state;
    sprite__animctrl_param params[RIZZ_SPRITE_ANIMCTRL_MAX_PARAMS];
    rizz_event_queue equeue;
    void* buff;
} sprite__animctrl;

typedef struct sprite__draw_context {
    sg_buffer vbuff[2];
    sg_buffer ibuff;
    sg_shader shader;
    sg_shader shader_wire;
    sg_pipeline pip;
    sg_pipeline pip_wire;
} sprite__draw_context;

typedef struct sprite__context {
    const sx_alloc* alloc;
    sx_strpool* name_pool;
    sx_handle_pool* sprite_handles;
    sprite__data* sprites;
    sprite__draw_context drawctx;
    sx_handle_pool* animclip_handles;
    sprite__animclip* animclips;
    sx_handle_pool* animctrl_handles;
    sprite__animctrl* animctrls;
} sprite__context;

typedef struct sprite__vertex_transform {
    sx_vec3 t1;
    sx_vec3 t2;
    sx_vec3 bc;
    uint32_t color;
} sprite__vertex_transform;

static rizz_vertex_layout k_sprite_vertex_layout = {
    .attrs[0] = { .semantic = "POSITION", .offset = offsetof(rizz_sprite_vertex, pos) },
    .attrs[1] = { .semantic = "TEXCOORD", .offset = offsetof(rizz_sprite_vertex, uv) },
    .attrs[2] = { .semantic = "COLOR",
                  .offset = offsetof(rizz_sprite_vertex, color),
                  .format = SG_VERTEXFORMAT_UBYTE4N },
    .attrs[3] = { .semantic = "TEXCOORD",
                  .semantic_idx = 1,
                  .offset = offsetof(sprite__vertex_transform, t1),
                  .buffer_index = 1 },
    .attrs[4] = { .semantic = "TEXCOORD",
                  .semantic_idx = 2,
                  .offset = offsetof(sprite__vertex_transform, t2),
                  .buffer_index = 1 },
    .attrs[5] = { .semantic = "COLOR",
                  .semantic_idx = 1,
                  .offset = offsetof(sprite__vertex_transform, color),
                  .buffer_index = 1,
                  .format = SG_VERTEXFORMAT_UBYTE4N }
};

static rizz_vertex_layout k_sprite_wire_vertex_layout = {
    .attrs[0] = { .semantic = "POSITION", .offset = offsetof(rizz_sprite_vertex, pos) },
    .attrs[1] = { .semantic = "COLOR",
                  .offset = offsetof(rizz_sprite_vertex, color),
                  .format = SG_VERTEXFORMAT_UBYTE4N },
    .attrs[2] = { .semantic = "TEXCOORD",
                  .semantic_idx = 1,
                  .offset = offsetof(sprite__vertex_transform, t1),
                  .buffer_index = 1 },
    .attrs[3] = { .semantic = "TEXCOORD",
                  .semantic_idx = 2,
                  .offset = offsetof(sprite__vertex_transform, t2),
                  .buffer_index = 1 },
    .attrs[4] = { .semantic = "TEXCOORD",
                  .semantic_idx = 3,
                  .offset = offsetof(sprite__vertex_transform, bc),
                  .buffer_index = 1 }
};

#define SORT_NAME sprite__sort
#define SORT_TYPE uint64_t
#define SORT_CMP(x, y) ((x) < (y) ? -1 : 1)
SX_PRAGMA_DIAGNOSTIC_PUSH()
SX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4267)
SX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4244)
SX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4146)
SX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wunused-function")
SX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG("-Wshorten-64-to-32")
#include "sort/sort.h"
SX_PRAGMA_DIAGNOSTIC_POP()

RIZZ_STATE static sprite__context g_spr;

////////////////////////////////////////////////////////////////////////////////////////////////////
// anim-clip
static void sprite__animclip_restart(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    clip->frame_id = 0;
    clip->tm = 0;
}

static rizz_sprite_animclip sprite__animclip_create(const rizz_sprite_animclip_desc* desc)
{
    sx_handle_t handle = sx_handle_new_and_grow(g_spr.animclip_handles, g_spr.alloc);
    sx_assert(handle);
    sx_assert(desc->num_frames > 0);

    const sx_alloc* alloc = desc->alloc ? desc->alloc : g_spr.alloc;

    sprite__animclip clip = { .atlas = desc->atlas,
                              .num_frames =
                                  RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES > 0
                                      ? sx_min(RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES, desc->num_frames)
                                      : desc->num_frames,
                              .fps = desc->fps,
                              .len = desc->length,
                              .trigger_end_event = desc->trigger_end_event,
                              .alloc = alloc };
    if (clip.num_frames < desc->num_frames) {
        rizz_log_warn(the_core, "num_frames exceeded maximum amount (%d) for sprite-animclip: 0x%x",
                      RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES, handle);
    }

    if (clip.fps > 0) {
        clip.len = (float)clip.num_frames / clip.fps;
    } else if (clip.len > 0) {
        clip.fps = (float)clip.num_frames / clip.len;
    } else {
        sx_assert(0 && "must define either 'fps' or 'length'");
    }

    the_asset->ref_add(desc->atlas);
    atlas__data* atlas = the_asset->obj(desc->atlas).ptr;

#if RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES > 0
    sprite__animclip_frame* frames = clip.frames;
#else
    sprite__animclip_frame* frames =
        sx_malloc(alloc, sizeof(sprite__animclip_frame) * clip.num_frames);
    if (!frames) {
        sx_out_of_memory();
        return (rizz_sprite_animclip){ 0 };
    }
    clip.frames = frames;
#endif

    for (int i = 0; i < clip.num_frames; i++) {
        const rizz_sprite_animclip_frame_desc* frame_desc = &desc->frames[i];
        sprite__animclip_frame* frame = &clip.frames[i];

        int sidx = sx_hashtbl_find(&atlas->sprite_tbl,
                                   sx_hash_fnv32(frame_desc->name, sx_strlen(frame_desc->name)));
        if (sidx != -1) {
            frame->atlas_id = sx_hashtbl_get(&atlas->sprite_tbl, sidx);
        } else {
            rizz_log_warn(the_core, "sprite not found: '%s' in '%s'", frame_desc->name,
                          the_asset->path(desc->atlas));
        }
    }

    sx_array_push_byindex(g_spr.alloc, g_spr.animclips, clip, sx_handle_index(handle));

    return (rizz_sprite_animclip){ handle };
}

static rizz_sprite_animclip sprite__animclip_clone(rizz_sprite_animclip src_handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, src_handle.id));
    sprite__animclip* src = &g_spr.animclips[sx_handle_index(src_handle.id)];

    sx_handle_t handle = sx_handle_new_and_grow(g_spr.animclip_handles, g_spr.alloc);
    sx_assert(handle);

    sx_assert(src->atlas.id);
    sprite__animclip clip = { .atlas = src->atlas,
                              .num_frames = src->num_frames,
                              .fps = src->fps,
                              .len = src->len,
                              .trigger_end_event = src->trigger_end_event,
                              .alloc = src->alloc };

#if RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES == 0
    sx_assert(clip.alloc);
    sprite__animclip_frame* frames =
        sx_malloc(clip.alloc, sizeof(sprite__animclip_frame) * clip.num_frames);
    if (!frames) {
        sx_out_of_memory();
        return (rizz_sprite_animclip){ 0 };
    }
    clip.frames = frames;
#endif

    sx_memcpy(clip.frames, src->frames, sizeof(sprite__animclip_frame) * clip.num_frames);
    the_asset->ref_add(clip.atlas);

    sx_array_push_byindex(g_spr.alloc, g_spr.animclips, clip, sx_handle_index(handle));

    return (rizz_sprite_animclip){ handle };
}

static void sprite__animclip_destroy(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];

    if (clip->atlas.id)
        the_asset->unload(clip->atlas);

#if RIZZ_SPRITE_ANIMCLIP_MAX_FRAMES == 0
    sx_assert(clip->alloc);
    sx_free(clip->alloc, clip->frames);
#endif

    sx_handle_del(g_spr.animclip_handles, handle.id);
}

static void sprite__animclip_update_batch(const rizz_sprite_animclip* handles, int num_clips,
                                          float dt)
{
    for (int i = 0; i < num_clips; i++) {
        sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handles[i].id));

        sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handles[i].id)];
        clip->end_triggered = false;
        float tadvance = clip->tm + dt;
        float t = sx_mod(tadvance, clip->len);    // progress time and wrap it onto time length

        // detect timeline end
        if (t < (tadvance - 0.0001f)) {
            if (clip->trigger_end_event) {
                rizz_event_push(&clip->equeue, RIZZ_SPRITE_ANIMCLIP_EVENT_END, NULL);
            }
            clip->end_triggered = true;
        }

        int frame_id = (int)(clip->fps * t);
        frame_id = sx_min(frame_id, clip->num_frames - 1);

        const sprite__animclip_frame* frame = &clip->frames[frame_id];
        if (frame->trigger && frame_id != clip->frame_id) {
            rizz_event_push(&clip->equeue, frame->e.e, frame->e.user);
        }

        clip->frame_id = frame_id;
        clip->tm = t;
    }
}

static void sprite__animclip_update(rizz_sprite_animclip clip, float dt)
{
    sprite__animclip_update_batch(&clip, 1, dt);
}

static float sprite__animclip_fps(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    return clip->fps;
}

static float sprite__animclip_len(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    return clip->len;
}

static rizz_sprite_flip sprite__animclip_flip(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    return clip->flip;
}

static rizz_event_queue* sprite__animclip_events(rizz_sprite_animclip handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    return &clip->equeue;
}

static void sprite__animclip_set_fps(rizz_sprite_animclip handle, float fps)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    sx_assert(clip->num_frames > 0);
    sx_assert(fps > 0);
    clip->len = (float)clip->num_frames / fps;
    clip->fps = fps;
}

static void sprite__animclip_set_len(rizz_sprite_animclip handle, float length)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    sx_assert(clip->num_frames > 0);
    sx_assert(length > 0);
    clip->fps = (float)clip->num_frames / length;
    clip->len = length;
}

void animclip_set_flip(rizz_sprite_animclip handle, rizz_sprite_flip flip)
{
    sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, handle.id));
    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(handle.id)];
    clip->flip = flip;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// anim-ctrl
static int sprite__animctrl_find_param_indesc(const char* name,
                                              const rizz_sprite_animctrl_param_desc* params)
{
    int index = 0;
    for (const rizz_sprite_animctrl_param_desc* p = &params[0]; p->name; ++p, ++index) {
        if (sx_strequal(name, p->name)) {
            return index;
        }
    }
    rizz_log_warn(the_core, "sprite animctrl param '%s' not found", name);
    sx_assert(0);
    return -1;
}

static sprite__animctrl_param* sprite__animctrl_find_param(const char* name, sprite__animctrl* ctrl)
{
    uint32_t name_hash = sx_hash_fnv32_str(name);
    for (sprite__animctrl_param* p = &ctrl->params[0]; p->name_hash; ++p) {
        if (p->name_hash == name_hash) {
            return p;
        }
    }
    rizz_log_warn(the_core, "sprite animctrl param '%s' not found", name);
    sx_assert(0);
    return NULL;
}

static void sprite__animctrl_set_paramb(rizz_sprite_animctrl handle, const char* name, bool b)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_BOOL || p->type == RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO);
    p->value.b = b;
}

static void sprite__animctrl_set_parami(rizz_sprite_animctrl handle, const char* name, int i)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_INT);
    p->value.i = i;
}

static void sprite__animctrl_set_paramf(rizz_sprite_animctrl handle, const char* name, float f)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_FLOAT);
    p->value.f = f;
}

static bool sprite__animctrl_param_valueb(rizz_sprite_animctrl handle, const char* name)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_BOOL || p->type == RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO);
    return p->value.b;
}

static float sprite__animctrl_param_valuef(rizz_sprite_animctrl handle, const char* name)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_FLOAT);
    return p->value.f;
}

static int sprite__animctrl_param_valuei(rizz_sprite_animctrl handle, const char* name)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sprite__animctrl_param* p = sprite__animctrl_find_param(name, ctrl);
    sx_assert(p->type == RIZZ_SPRITE_PARAMTYPE_INT);
    return p->value.i;
}

static rizz_sprite_animclip sprite__animctrl_clip(rizz_sprite_animctrl handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sx_assert(ctrl->state);
    return ctrl->state->clip;
}

static int sprite__animctrl_find_state(const char* name, const uint32_t* hashes, int num_states)
{
    uint32_t hash = sx_hash_fnv32_str(name);
    for (int i = 0; i < num_states; i++) {
        if (hash == hashes[i])
            return i;
    }
    rizz_log_warn(the_core, "sprite animctrl state '%s' not found", name);
    sx_assert(0);
    return 0;
}

static void sprite__animctrl_restart(rizz_sprite_animctrl handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    ctrl->state = ctrl->start_state;
    sprite__animclip_restart(ctrl->state->clip);
}

static rizz_event_queue* sprite__animctrl_events(rizz_sprite_animctrl handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    return &ctrl->equeue;
}

static rizz_sprite_animctrl sprite__animctrl_create(const rizz_sprite_animctrl_desc* desc)
{
    sx_assert(desc->num_states > 1);
    sx_assert(desc->num_transitions > 0);
    sx_assert(desc->start_state);

    const sx_alloc* alloc = desc->alloc ? desc->alloc : g_spr.alloc;
    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    sx_handle_t handle = sx_handle_new_and_grow(g_spr.animctrl_handles, g_spr.alloc);
    sx_assert(handle);
    int num_states = desc->num_states;
    int num_transitions = desc->num_transitions;

    uint32_t* hashes = sx_malloc(tmp_alloc, sizeof(uint32_t) * num_states);
    sx_assert(hashes);
    for (int i = 0; i < num_states; i++)
        hashes[i] = sx_hash_fnv32_str(desc->states[i].name);

    // each element is count of transitions for each state
    int* transition_counts = sx_malloc(tmp_alloc, sizeof(int) * num_states);
    sx_assert(transition_counts);
    sx_memset(transition_counts, 0x0, sizeof(int) * num_states);

    // each element is index to states array
    int* transition_map = sx_malloc(tmp_alloc, sizeof(int) * num_transitions);
    sx_assert(transition_map);

    int total_sz = sizeof(sprite__animctrl_state) * num_states +
                   sizeof(sprite__animctrl_transition) * num_transitions;
    uint8_t* buff = sx_malloc(alloc, total_sz);
    if (!buff) {
        the_core->tmp_alloc_pop();
        sx_out_of_memory();
        return (rizz_sprite_animctrl){ 0 };
    }
    void* _buff = buff;

    // populate remap and count arrays
    for (int i = 0; i < num_transitions; i++) {
        sx_assert(desc->transitions[i].state);
        sx_assert(desc->transitions[i].target_state);

        int state_id = sprite__animctrl_find_state(desc->transitions[i].state, hashes, num_states);
        transition_map[i] = state_id;
        ++transition_counts[state_id];
    }

    // allocate states
    sprite__animctrl_state** states =
        sx_malloc(tmp_alloc, sizeof(sprite__animctrl_state) * num_states);
    sx_assert(states);

    for (int i = 0; i < num_states; i++) {
        sprite__animctrl_state* state = (sprite__animctrl_state*)buff;
        states[i] = state;

        sx_strcpy(state->name, sizeof(state->name), desc->states[i].name);
        state->clip = desc->states[i].clip;
        sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, state->clip.id));
        state->num_transitions = transition_counts[i];

        buff += sizeof(sprite__animctrl_state);

        // transition array for each state
        sprite__animctrl_transition* transitions = (sprite__animctrl_transition*)buff;
        state->transitions = transitions;

        for (int t = 0, tidx = 0; t < num_transitions; t++) {
            if (transition_map[t] == i) {
                const char* param_name = desc->transitions[t].trigger.param_name;
                // save index instead of pointer, later we will resolve it to valid pointers
                transitions[tidx].target = (void*)(intptr_t)sprite__animctrl_find_state(
                    desc->transitions[t].target_state, hashes, num_states);
                transitions[tidx].trigger = (sprite__animctrl_trigger){
                    .param_id = param_name
                                    ? sprite__animctrl_find_param_indesc(param_name, desc->params)
                                    : ANIMCTRL_PARAM_ID_END,
                    .func = desc->transitions[t].trigger.func,
                    .value.i = desc->transitions[t].trigger.value.i
                };
                transitions[tidx].trigger_event = desc->transitions[t].trigger_event;
                transitions[tidx].event = desc->transitions[t].event;
                tidx++;
            }
        }
        buff += transition_counts[i] * sizeof(sprite__animctrl_transition);
    }

    // resolve pointers to states inside transitions
    for (int i = 0; i < num_states; i++) {
        sprite__animctrl_state* state = states[i];
        for (int t = 0; t < state->num_transitions; t++) {
            state->transitions[t].target = states[(intptr_t)(void*)state->transitions[t].target];
        }
    }

    //
    sprite__animctrl ctrl = (sprite__animctrl){
        .alloc = alloc,
        .start_state = states[sprite__animctrl_find_state(desc->start_state, hashes, num_states)],
        .buff = _buff
    };

    ctrl.state = ctrl.start_state;
    int param_idx = 0;
    for (const rizz_sprite_animctrl_param_desc* p = &desc->params[0]; p->name; ++p, ++param_idx) {
        sx_strcpy(ctrl.params[param_idx].name, sizeof(ctrl.params[param_idx].name), p->name);
        ctrl.params[param_idx].name_hash = sx_hash_fnv32_str(p->name);
        ctrl.params[param_idx].type = p->type;
        ctrl.params[param_idx].value.i = 0;
    }

    sx_array_push_byindex(g_spr.alloc, g_spr.animctrls, ctrl, sx_handle_index(handle));

    the_core->tmp_alloc_pop();
    return (rizz_sprite_animctrl){ handle };
}

static void sprite__animctrl_destroy(rizz_sprite_animctrl handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handle.id));
    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handle.id)];
    sx_assert(ctrl->alloc);

    sx_free(ctrl->alloc, ctrl->buff);
    sx_handle_del(g_spr.animctrl_handles, handle.id);
}

// callbacks for compare functions
static bool sprite__animctrl_cmp_none(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    sx_unused(v);
    sx_unused(p);
    return false;
}

static bool sprite__animctrl_cmp_less(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b != v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i < v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return p->value.f < v.f;
    default:
        return false;
    }
}

static bool sprite__animctrl_cmp_eq(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b == v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i == v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return sx_equal(p->value.f, v.f, 0.00001f);
    default:
        return false;
    }
}

static bool sprite__animctrl_cmp_gt(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b != v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i > v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return p->value.f > v.f;
    default:
        return false;
    }
}

static bool sprite__animctrl_cmp_neq(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b != v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i != v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return !sx_equal(p->value.f, v.f, 0.00001f);
    default:
        return false;
    }
}

static bool sprite__animctrl_cmp_gte(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b == v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i >= v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return p->value.f >= v.f;
    default:
        return false;
    }
}

static bool sprite__animctrl_cmp_lte(sprite__animctrl_value v, const sprite__animctrl_param* p)
{
    switch (p->type) {
    case RIZZ_SPRITE_PARAMTYPE_BOOL:
    case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
        return p->value.b == v.b;
    case RIZZ_SPRITE_PARAMTYPE_INT:
        return p->value.i <= v.i;
    case RIZZ_SPRITE_PARAMTYPE_FLOAT:
        return p->value.f <= v.f;
    default:
        return false;
    }
}

typedef bool(sprite__animctrl_cmp_fn)(sprite__animctrl_value, const sprite__animctrl_param*);
static sprite__animctrl_cmp_fn* k_compare_funcs[_RIZZ_SPRITE_COMPAREFUNC_COUNT] = {
    sprite__animctrl_cmp_none, sprite__animctrl_cmp_less, sprite__animctrl_cmp_eq,
    sprite__animctrl_cmp_gt,   sprite__animctrl_cmp_neq,  sprite__animctrl_cmp_gte,
    sprite__animctrl_cmp_lte
};

static void sprite__animctrl_trigger_transition(sprite__animctrl* ctrl, int transition_id)
{
    sprite__animctrl_state* state = ctrl->state;
    sprite__animctrl_transition* transition = &state->transitions[transition_id];

    ctrl->state = transition->target;
    if (transition->trigger_event) {
        rizz_event_push(&ctrl->equeue, transition->event.e, transition->event.user);
    }
}

static void sprite__animctrl_update_batch(const rizz_sprite_animctrl* handles, int num_ctrls,
                                          float dt)
{
    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();
    rizz_sprite_animclip* clips = sx_malloc(tmp_alloc, sizeof(rizz_sprite_animclip) * num_ctrls);
    sprite__animctrl** ctrls = sx_malloc(tmp_alloc, sizeof(sprite__animctrl*) * num_ctrls);
    sx_assert(clips && ctrls);

    for (int i = 0; i < num_ctrls; i++) {
        sx_assert_rel(sx_handle_valid(g_spr.animctrl_handles, handles[i].id));
        sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(handles[i].id)];

        ctrls[i] = ctrl;
        clips[i] = ctrl->state->clip;
    }

    // update clips
    sprite__animclip_update_batch(clips, num_ctrls, dt);

    // check on_end events on clips
    for (int i = 0; i < num_ctrls; i++) {
        sprite__animctrl* ctrl = ctrls[i];
        sprite__animctrl_state* state = ctrl->state;

        // check parameterized transitions
        for (int t = 0, c = state->num_transitions; t < c; t++) {
            sprite__animctrl_transition* transition = &state->transitions[t];
            if (transition->trigger.param_id != ANIMCTRL_PARAM_ID_END) {
                bool r = k_compare_funcs[transition->trigger.func](
                    (sprite__animctrl_value){ .i = transition->trigger.value.i },
                    &ctrl->params[transition->trigger.param_id]);
                if (r) {
                    sprite__animctrl_trigger_transition(ctrl, t);
                    break;
                }
            } else {
                sx_assert(sx_handle_valid(g_spr.animclip_handles, state->clip.id));
                sprite__animclip* clip = &g_spr.animclips[sx_handle_index(state->clip.id)];
                if (clip->end_triggered) {
                    sprite__animctrl_trigger_transition(ctrl, t);
                    break;
                }
            }
        }    // foreach: transition

        for (sprite__animctrl_param* p = &ctrl->params[0]; p->name_hash; ++p) {
            if (p->type == RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO && p->value.b)
                p->value.b = false;
        }
    }

    the_core->tmp_alloc_pop();
}

static void sprite__animctrl_update(rizz_sprite_animctrl handle, float dt)
{
    sprite__animctrl_update_batch(&handle, 1, dt);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// sprite
static void sprite__sync_with_animclip(sprite__data* spr)
{
    rizz_sprite_animclip clip_handle = spr->clip;
    if (sx_handle_valid(g_spr.animclip_handles, clip_handle.id)) {
        sprite__animclip* clip = &g_spr.animclips[sx_handle_index(clip_handle.id)];
        spr->atlas_sprite_id = (int)clip->frames[clip->frame_id].atlas_id;
        spr->flip = clip->flip;
    } else {
        rizz_log_warn(
            the_core, "sprite_animclip 'handle: 0x%x' binded to sprite '%s' has become invalid",
            clip_handle.id, spr->name ? sx_strpool_cstr(g_spr.name_pool, spr->name) : "[noname]");
        spr->clip = (rizz_sprite_animclip){ 0 };
    }
}

static sx_vec2 sprite__calc_size(const sx_vec2 size, const sx_vec2 base_size, rizz_sprite_flip flip)
{
    sx_assert(size.x > 0 || size.y > 0);
    sx_vec2 _size;
    // if any component of half-size is set to less than zero, then it will be evaluated by ratio
    if (size.y <= 0) {
        float ratio = base_size.y / base_size.x;
        _size = sx_vec2f(size.x, size.x * ratio);
    } else if (size.x <= 0) {
        float ratio = base_size.x / base_size.y;
        _size = sx_vec2f(size.x * ratio, size.y);
    } else {
        _size = size;
    }

    // flip
    if (flip & RIZZ_SPRITE_FLIP_X)
        _size.x = -_size.x;
    if (flip & RIZZ_SPRITE_FLIP_Y)
        _size.y = -_size.y;

    return _size;
}

static inline sx_vec2 sprite__normalize_pos(const sx_vec2 pos_px, const sx_vec2 base_size_rcp)
{
    sx_vec2 n = sx_vec2_mul(pos_px, base_size_rcp);
    return sx_vec2f(n.x - 0.5f, 0.5f - n.y);    // flip-y and transform to (0.5f, 0.5f) space
}

static inline void sprite__calc_coords(sx_vec2* points, int num_points, const sx_vec2* points_px,
                                       const sx_vec2 size, const sx_vec2 origin)
{
    const sx_vec2 size_rcp = sx_vec2f(1.0f / size.x, 1.0f / size.y);
    for (int i = 0; i < num_points; i++) {
        sx_vec2 n = sx_vec2_mul(points_px[i], size_rcp);    // normalize to 0..1
        n = sx_vec2f(n.x - 0.5f, 0.5f - n.y);    // flip-y and transform to (0.5f, 0.5f) space
        points[i] = sx_vec2_mul(sx_vec2_sub(n, origin), size);    // offset by origin and resize
    }
}

static void sprite__update_bounds(sprite__data* spr)
{
    if (spr->clip.id)
        sprite__sync_with_animclip(spr);

    sx_rect rect = sx_rectf(-0.5f, -0.5f, 0.5f, 0.5f);
    if (spr->atlas.id && spr->atlas_sprite_id >= 0) {
        const atlas__data* atlas = (atlas__data*)the_asset->obj_threadsafe(spr->atlas).ptr;
        const atlas__sprite* aspr = &atlas->sprites[spr->atlas_sprite_id];
        sx_vec2 size = sprite__calc_size(spr->size, aspr->base_size, spr->flip);
        sx_vec2 origin = spr->origin;
        sx_vec2 base_size_rcp = sx_vec2f(1.0f / aspr->base_size.x, 1.0f / aspr->base_size.y);
        sx_rect sprite_rect =
            sx_rectv(sprite__normalize_pos(aspr->sprite_rect.vmin, base_size_rcp),
                     sprite__normalize_pos(aspr->sprite_rect.vmax, base_size_rcp));

        spr->draw_bounds = sx_rectv(sx_vec2_mul(sx_vec2_sub(sprite_rect.vmin, origin), size),
                                    sx_vec2_mul(sx_vec2_sub(sprite_rect.vmax, origin), size));
        spr->bounds = sx_rectv(sx_vec2_mul(sx_vec2_sub(rect.vmin, origin), size),
                               sx_vec2_mul(sx_vec2_sub(rect.vmax, origin), size));
    } else {
        rizz_texture* tex = (rizz_texture*)the_asset->obj_threadsafe(spr->texture).ptr;
        sx_assert(tex);
        sx_vec2 base_size = sx_vec2f((float)tex->info.width, (float)tex->info.height);
        sx_vec2 size = sprite__calc_size(spr->size, base_size, spr->flip);
        sx_vec2 origin = spr->origin;
        spr->bounds = sx_rectv(sx_vec2_mul(sx_vec2_sub(rect.vmin, origin), size),
                               sx_vec2_mul(sx_vec2_sub(rect.vmax, origin), size));
        spr->draw_bounds = spr->bounds;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// atlas
static rizz_asset_load_data atlas__on_prepare(const rizz_asset_load_params* params,
                                              const void* metadata)
{
    const sx_alloc* alloc = params->alloc ? params->alloc : g_spr.alloc;
    const atlas__metadata* meta = metadata;
    sx_assert(meta);

    int hashtbl_cap = sx_hashtbl_valid_capacity(meta->num_sprites);
    int total_sz = sizeof(atlas__data) + meta->num_sprites * sizeof(atlas__sprite) +
                   meta->num_indices * sizeof(uint16_t) +
                   meta->num_vertices * sizeof(rizz_sprite_vertex) +
                   sx_hashtbl_fixed_size(meta->num_sprites);
    atlas__data* atlas = sx_malloc(alloc, total_sz);
    if (!atlas) {
        sx_out_of_memory();
        return (rizz_asset_load_data){ .obj = { 0 } };
    }
    sx_memset(atlas, 0x0, total_sz);

    uint8_t* buff = (uint8_t*)(atlas + 1);
    atlas->sprites = (atlas__sprite*)buff;
    buff += sizeof(atlas__sprite) * meta->num_sprites;
    uint32_t* keys = (uint32_t*)buff;
    buff += sizeof(uint32_t) * hashtbl_cap;
    int* values = (int*)buff;
    buff += sizeof(int) * hashtbl_cap;
    sx_hashtbl_init(&atlas->sprite_tbl, hashtbl_cap, keys, values);
    atlas->vertices = (rizz_sprite_vertex*)buff;
    buff += sizeof(rizz_sprite_vertex) * meta->num_vertices;
    atlas->indices = (uint16_t*)buff;
    buff += sizeof(uint16_t) * meta->num_indices;

    const rizz_atlas_load_params* aparams = params->params;
    rizz_texture_load_params tparams = { .min_filter = aparams->min_filter,
                                         .mag_filter = aparams->mag_filter,
                                         .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
                                         .wrap_v = SG_WRAP_CLAMP_TO_EDGE };
    atlas->a.texture = the_asset->load("texture", meta->img_filepath, &tparams, params->flags,
                                       alloc, params->tags);

    return (rizz_asset_load_data){ .obj = { .ptr = atlas }, .user = buff };
}

static bool atlas__on_load(rizz_asset_load_data* data, const rizz_asset_load_params* params,
                           const sx_mem_block* mem)
{
    atlas__data* atlas = data->obj.ptr;

    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    char* buff = sx_malloc(tmp_alloc, (size_t)mem->size + 1);
    if (!buff) {
        sx_out_of_memory();
        return false;
    }

    sx_memcpy(buff, mem->data, mem->size);
    buff[mem->size] = '\0';
    sjson_context* jctx = sjson_create_context(0, 0, (void*)tmp_alloc);
    sx_assert(jctx);

    sjson_node* jroot = sjson_decode(jctx, buff);
    if (!jroot) {
        rizz_log_warn(the_core, "loading atlas '%s' failed: not a valid json file", params->path);
        return false;
    }

    atlas->a.info.img_width = sjson_get_int(jroot, "image_width", 0);
    atlas->a.info.img_height = sjson_get_int(jroot, "image_height", 0);
    int sprite_idx = 0;
    sjson_node* jsprites = sjson_find_member(jroot, "sprites");
    sjson_node* jsprite;
    sx_assert(jsprites);
    sx_vec2 atlas_size = sx_vec2f((float)atlas->a.info.img_width, (float)atlas->a.info.img_height);
    sx_vec2 atlas_size_rcp = sx_vec2f(1.0f / atlas_size.x, 1.0f / atlas_size.y);
    int ib_index = 0;
    int vb_index = 0;
    sjson_foreach(jsprite, jsprites)
    {
        int tmp[4];
        atlas__sprite* aspr = &atlas->sprites[sprite_idx];
        const char* name = sjson_get_string(jsprite, "name", "");
        sx_hashtbl_add(&atlas->sprite_tbl, sx_hash_fnv32_str(name), sprite_idx);

        sjson_get_ints(tmp, 2, jsprite, "size");
        aspr->base_size = sx_vec2f((float)tmp[0], (float)tmp[1]);

        sjson_get_ints(tmp, 4, jsprite, "sprite_rect");
        aspr->sprite_rect = sx_rectf((float)tmp[0], (float)tmp[1], (float)tmp[2], (float)tmp[3]);

        sjson_get_ints(tmp, 4, jsprite, "sheet_rect");
        aspr->sheet_rect = sx_rectf((float)tmp[0], (float)tmp[1], (float)tmp[2], (float)tmp[3]);

        // load geometry
        sx_vec2 base_size_rcp = sx_vec2f(1.0f / aspr->base_size.x, 1.0f / aspr->base_size.y);
        sjson_node* jmesh = sjson_find_member(jsprite, "mesh");
        if (jmesh) {
            // sprite-mesh
            aspr->num_indices = sjson_get_int(jmesh, "num_tris", 0) * 3;
            aspr->num_verts = sjson_get_int(jmesh, "num_vertices", 0);
            rizz_sprite_vertex* verts = &atlas->vertices[vb_index];
            uint16_t* indices = &atlas->indices[ib_index];
            sjson_get_uint16s(indices, aspr->num_indices, jmesh, "indices");
            sjson_node* jposs = sjson_find_member(jmesh, "positions");
            if (jposs) {
                sjson_node* jpos;
                int v = 0;
                sjson_foreach(jpos, jposs)
                {
                    sx_vec2 pos;
                    sjson_get_floats(pos.f, 2, jpos, NULL);
                    sx_assert(v < aspr->num_verts);
                    verts[v].pos = sprite__normalize_pos(pos, base_size_rcp);
                    v++;
                }
            }
            sjson_node* juvs = sjson_find_member(jmesh, "uvs");
            if (juvs) {
                sjson_node* juv;
                int v = 0;
                sjson_foreach(juv, juvs)
                {
                    sx_vec2 uv;
                    sjson_get_floats(uv.f, 2, juv, NULL);
                    sx_assert(v < aspr->num_verts);
                    verts[v].uv = sx_vec2_mul(uv, atlas_size_rcp);
                    v++;
                }
            }

        } else {
            // sprite-quad
            aspr->num_indices = 6;
            aspr->num_verts = 4;
            rizz_sprite_vertex* verts = &atlas->vertices[vb_index];
            uint16_t* indices = &atlas->indices[ib_index];
            sx_rect uv_rect = sx_rectv(sx_vec2_mul(aspr->sheet_rect.vmin, atlas_size_rcp),
                                       sx_vec2_mul(aspr->sheet_rect.vmax, atlas_size_rcp));
            verts[0].pos =
                sprite__normalize_pos(sx_rect_corner(&aspr->sprite_rect, 0), base_size_rcp);
            verts[0].uv = sx_rect_corner(&uv_rect, 0);
            verts[1].pos =
                sprite__normalize_pos(sx_rect_corner(&aspr->sprite_rect, 1), base_size_rcp);
            verts[1].uv = sx_rect_corner(&uv_rect, 1);
            verts[2].pos =
                sprite__normalize_pos(sx_rect_corner(&aspr->sprite_rect, 2), base_size_rcp);
            verts[2].uv = sx_rect_corner(&uv_rect, 2);
            verts[3].pos =
                sprite__normalize_pos(sx_rect_corner(&aspr->sprite_rect, 3), base_size_rcp);
            verts[3].uv = sx_rect_corner(&uv_rect, 3);

            // clang-format off
            indices[0] = 0;         indices[1] = 1;     indices[2] = 2;
            indices[3] = 2;         indices[4] = 1;     indices[5] = 3;
            // clang-format on
        }

        aspr->ib_index = ib_index;
        aspr->vb_index = vb_index;
        ib_index += aspr->num_indices;
        vb_index += aspr->num_verts;
        ++sprite_idx;
    }
    atlas->a.info.num_sprites = sprite_idx;

    the_core->tmp_alloc_pop();

    return true;
}

static void atlas__on_finalize(rizz_asset_load_data* data, const rizz_asset_load_params* params,
                               const sx_mem_block* mem)
{
    sx_unused(data);
    sx_unused(params);
    sx_unused(mem);
}

static void atlas__on_reload(rizz_asset handle, rizz_asset_obj prev_obj, const sx_alloc* alloc)
{
    sx_unused(handle);
    sx_unused(prev_obj);
    sx_unused(alloc);
}

static void atlas__on_release(rizz_asset_obj obj, const sx_alloc* alloc)
{
    atlas__data* atlas = obj.ptr;
    sx_assert(atlas);

    if (!alloc)
        alloc = g_spr.alloc;

    if (atlas->a.texture.id) {
        the_asset->unload(atlas->a.texture);
    }

    sx_free(alloc, atlas);
}

static void atlas__on_read_metadata(void* metadata, const rizz_asset_load_params* params,
                                    const sx_mem_block* mem)
{
    atlas__metadata* meta = metadata;
    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    char* buff = sx_malloc(tmp_alloc, (size_t)mem->size + 1);
    if (!buff) {
        sx_out_of_memory();
        return;
    }
    sx_memcpy(buff, mem->data, mem->size);
    buff[mem->size] = '\0';
    sjson_context* jctx = sjson_create_context(0, 0, (void*)tmp_alloc);
    sx_assert(jctx);

    sjson_node* jroot = sjson_decode(jctx, buff);
    if (!jroot) {
        rizz_log_warn(the_core, "loading atlas '%s' failed: not a valid json file", params->path);
        return;
    }

    char dirname[RIZZ_MAX_PATH];
    sx_os_path_dirname(dirname, sizeof(dirname), params->path);
    sx_os_path_join(meta->img_filepath, sizeof(meta->img_filepath), dirname,
                    sjson_get_string(jroot, "image", ""));
    sx_os_path_unixpath(meta->img_filepath, sizeof(meta->img_filepath), meta->img_filepath);

    sjson_node* jsprites = sjson_find_member(jroot, "sprites");
    sjson_node* jsprite;
    int num_sprites = 0, num_indices = 0, num_vertices = 0;
    sjson_foreach(jsprite, jsprites)
    {
        sjson_node* jmesh = sjson_find_member(jsprite, "mesh");
        if (jmesh) {
            num_indices += 3 * sjson_get_int(jmesh, "num_tris", 0);
            num_vertices += sjson_get_int(jmesh, "num_vertices", 0);
        } else {
            num_indices += 6;
            num_vertices += 4;
        }
        ++num_sprites;
    }
    meta->num_sprites = num_sprites;
    meta->num_indices = num_indices;
    meta->num_vertices = num_vertices;

    the_core->tmp_alloc_pop();
}

static bool sprite__resize_draw_limits(int max_verts, int max_indices)
{
    sx_assert(max_verts < UINT16_MAX);

    // recreate vertex/index buffers
    sprite__draw_context* dc = &g_spr.drawctx;
    if (dc->vbuff[0].id)
        the_gfx->destroy_buffer(dc->vbuff[0]);
    if (dc->vbuff[1].id)
        the_gfx->destroy_buffer(dc->vbuff[1]);
    if (dc->ibuff.id)
        the_gfx->destroy_buffer(dc->ibuff);

    if (max_verts == 0 || max_indices == 0) {
        dc->vbuff[0] = dc->vbuff[1] = dc->ibuff = (sg_buffer){ 0 };
        return true;
    }

    dc->vbuff[0] =
        the_gfx->make_buffer(&(sg_buffer_desc){ .size = sizeof(rizz_sprite_vertex) * max_verts,
                                                .usage = SG_USAGE_STREAM,
                                                .type = SG_BUFFERTYPE_VERTEXBUFFER });
    dc->vbuff[1] = the_gfx->make_buffer(
        &(sg_buffer_desc){ .size = sizeof(sprite__vertex_transform) * max_verts,
                           .usage = SG_USAGE_STREAM,
                           .type = SG_BUFFERTYPE_VERTEXBUFFER });
    dc->ibuff = the_gfx->make_buffer(&(sg_buffer_desc){ .size = sizeof(uint16_t) * max_indices,
                                                        .usage = SG_USAGE_STREAM,
                                                        .type = SG_BUFFERTYPE_INDEXBUFFER });

    return dc->vbuff[0].id && dc->vbuff[1].id && dc->ibuff.id;
}

static bool sprite__init()
{
    g_spr.alloc = the_core->alloc(RIZZ_MEMID_GRAPHICS);
    g_spr.name_pool = sx_strpool_create(
        g_spr.alloc, &(sx_strpool_config){ .counter_bits = SX_CONFIG_HANDLE_GEN_BITS,
                                           .index_bits = 32 - SX_CONFIG_HANDLE_GEN_BITS,
                                           .entry_capacity = 4096,
                                           .block_capacity = 32,
                                           .block_sz_kb = 64,
                                           .min_str_len = 23 });

    if (!g_spr.name_pool) {
        sx_out_of_memory();
        return false;
    }

    g_spr.sprite_handles = sx_handle_create_pool(g_spr.alloc, 256);
    sx_assert(g_spr.sprite_handles);

    g_spr.animclip_handles = sx_handle_create_pool(g_spr.alloc, 256);
    sx_assert(g_spr.animclip_handles);

    g_spr.animctrl_handles = sx_handle_create_pool(g_spr.alloc, 128);
    sx_assert(g_spr.animctrl_handles);

    // register "atlas" asset type and metadata
    rizz_refl_field(the_refl, atlas__metadata, char[RIZZ_MAX_PATH], img_filepath, "img_filepath");
    rizz_refl_field(the_refl, atlas__metadata, int, num_sprites, "num_sprites");
    rizz_refl_field(the_refl, atlas__metadata, int, num_vertices, "num_vertices");
    rizz_refl_field(the_refl, atlas__metadata, int, num_indices, "num_indices");

    the_asset->register_asset_type(
        "atlas",
        (rizz_asset_callbacks){ .on_prepare = atlas__on_prepare,
                                .on_load = atlas__on_load,
                                .on_finalize = atlas__on_finalize,
                                .on_reload = atlas__on_reload,
                                .on_release = atlas__on_release,
                                .on_read_metadata = atlas__on_read_metadata },
        "rizz_atlas_load_params", sizeof(rizz_atlas_load_params), "atlas__metadata",
        sizeof(atlas__metadata), (rizz_asset_obj){ .ptr = NULL }, (rizz_asset_obj){ .ptr = NULL },
        0);

    // init draw context
    if (!sprite__resize_draw_limits(MAX_VERTICES, MAX_INDICES)) {
        return false;
    }

    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    rizz_shader shader = the_gfx->shader_make_with_data(
        tmp_alloc, k_sprite_vs_size, k_sprite_vs_data, k_sprite_vs_refl_size, k_sprite_vs_refl_data,
        k_sprite_fs_size, k_sprite_fs_data, k_sprite_fs_refl_size, k_sprite_fs_refl_data);
    g_spr.drawctx.shader = shader.shd;

    sg_pipeline_desc pip_desc =
        (sg_pipeline_desc){ .layout.buffers[0].stride = sizeof(rizz_sprite_vertex),
                            .layout.buffers[1].stride = sizeof(sprite__vertex_transform),
                            .index_type = SG_INDEXTYPE_UINT16,
                            .rasterizer = { .cull_mode = SG_CULLMODE_BACK },
                            .blend = { .enabled = true,
                                       .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                                       .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA } };

    g_spr.drawctx.pip = the_gfx->make_pipeline(
        the_gfx->shader_bindto_pipeline(&shader, &pip_desc, &k_sprite_vertex_layout));

    // wireframe pipeline
    rizz_shader shader_wire = the_gfx->shader_make_with_data(
        tmp_alloc, k_sprite_wire_vs_size, k_sprite_wire_vs_data, k_sprite_wire_vs_refl_size,
        k_sprite_wire_vs_refl_data, k_sprite_wire_fs_size, k_sprite_wire_fs_data,
        k_sprite_wire_fs_refl_size, k_sprite_wire_fs_refl_data);
    g_spr.drawctx.shader = shader_wire.shd;
    pip_desc.index_type = SG_INDEXTYPE_NONE;
    g_spr.drawctx.pip_wire = the_gfx->make_pipeline(
        the_gfx->shader_bindto_pipeline(&shader_wire, &pip_desc, &k_sprite_wire_vertex_layout));

    the_core->tmp_alloc_pop();
    return true;
}

static void sprite__release()
{
    if (!g_spr.alloc)
        return;

    // draw context
    {
        const sprite__draw_context* dc = &g_spr.drawctx;
        if (dc->vbuff[0].id)
            the_gfx->destroy_buffer(dc->vbuff[0]);
        if (dc->vbuff[1].id)
            the_gfx->destroy_buffer(dc->vbuff[1]);
        if (dc->ibuff.id)
            the_gfx->destroy_buffer(dc->ibuff);
        if (dc->shader.id)
            the_gfx->destroy_shader(dc->shader);
        if (dc->shader_wire.id)
            the_gfx->destroy_shader(dc->shader_wire);
        if (dc->pip.id)
            the_gfx->destroy_pipeline(dc->pip);
        if (dc->pip_wire.id)
            the_gfx->destroy_pipeline(dc->pip_wire);
    }

    if (g_spr.sprite_handles) {
        if (g_spr.sprite_handles->count > 0) {
            rizz_log_warn(the_core, "total %d sprites are not released",
                          g_spr.sprite_handles->count);
        }
        sx_handle_destroy_pool(g_spr.sprite_handles, g_spr.alloc);
    }

    if (g_spr.animclip_handles) {
        if (g_spr.animclip_handles->count > 0) {
            rizz_log_warn(the_core, "total %d sprite_animclips are not released",
                          g_spr.animclip_handles->count);
        }
        sx_handle_destroy_pool(g_spr.animclip_handles, g_spr.alloc);
    }

    if (g_spr.animctrl_handles) {
        if (g_spr.animctrl_handles->count > 0) {
            rizz_log_warn(the_core, "total %d sprite_animctrls are not released",
                          g_spr.animctrl_handles->count);
        }

        sx_handle_destroy_pool(g_spr.animctrl_handles, g_spr.alloc);
    }

    if (g_spr.name_pool)
        sx_strpool_destroy(g_spr.name_pool, g_spr.alloc);

    sx_array_free(g_spr.alloc, g_spr.sprites);
    sx_array_free(g_spr.alloc, g_spr.animctrls);
    sx_array_free(g_spr.alloc, g_spr.animclips);

    the_asset->unregister_asset_type("atlas");
}

static rizz_sprite sprite__create(const rizz_sprite_desc* desc)
{
    sx_assert(desc->texture.id || desc->clip.id || desc->ctrl.id);
    sx_assert(desc->size.x > 0 || desc->size.y > 0);

    sx_handle_t handle = sx_handle_new_and_grow(g_spr.sprite_handles, g_spr.alloc);
    sx_assert(handle);
    int name_len = desc->name ? sx_strlen(desc->name) : 0;
    sprite__data spr = { .name =
                             desc->name ? sx_strpool_add(g_spr.name_pool, desc->name, name_len) : 0,
                         .size = desc->size,
                         .origin = desc->origin,
                         .color = desc->color.n != 0 ? desc->color : sx_colorn(0xffffffff),
                         .flip = desc->flip,
                         .clip = desc->clip,
                         .ctrl = desc->ctrl };

    if (spr.ctrl.id) {
        spr.clip = sprite__animctrl_clip(spr.ctrl);
    }

    if (spr.clip.id) {
        sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, spr.clip.id));
        sprite__animclip* clip = &g_spr.animclips[sx_handle_index(spr.clip.id)];
        sx_assert(clip->num_frames > 0);
        spr.atlas = clip->atlas;
        spr.atlas_sprite_id = (int)clip->frames[clip->frame_id].atlas_id;
        atlas__data* atlas = the_asset->obj(clip->atlas).ptr;
        spr.texture = atlas->a.texture;
        the_asset->ref_add(spr.atlas);
    } else if (desc->texture.id) {
        const char* img_type = the_asset->type_name(desc->texture);
        if (sx_strequal(img_type, "texture")) {
            spr.texture = desc->texture;
            the_asset->ref_add(spr.texture);

            spr.atlas_sprite_id = -1;
        } else if (sx_strequal(img_type, "atlas")) {
            sx_assert(desc->name && "for atlases, desc->name should be set");
            spr.atlas = desc->atlas;
            the_asset->ref_add(desc->atlas);

            atlas__data* atlas = the_asset->obj(desc->atlas).ptr;
            spr.texture = atlas->a.texture;
            int sidx = sx_hashtbl_find(&atlas->sprite_tbl, sx_hash_fnv32(desc->name, name_len));
            if (sidx != -1) {
                spr.atlas_sprite_id = sx_hashtbl_get(&atlas->sprite_tbl, sidx);
            } else {
                rizz_log_warn(the_core, "sprite not found: '%s' in '%s'", desc->name,
                              the_asset->path(desc->atlas));
            }
        } else {
            sx_assert(0 && "desc->atlas != atlas or desc->texture != texture");
            return (rizz_sprite){ 0 };
        }
    }
    sprite__update_bounds(&spr);

    sx_array_push_byindex(g_spr.alloc, g_spr.sprites, spr, sx_handle_index(handle));

    return (rizz_sprite){ handle };
}

rizz_sprite sprite__clone(rizz_sprite src_handle, rizz_sprite_animclip clip_handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, src_handle.id));
    sprite__data* src = &g_spr.sprites[sx_handle_index(src_handle.id)];

    sx_handle_t handle = sx_handle_new_and_grow(g_spr.sprite_handles, g_spr.alloc);
    sx_assert(handle);

    const char* name = src->name ? sx_strpool_cstr(g_spr.name_pool, src->name) : NULL;
    int name_len = name ? sx_strlen(name) : 0;
    sprite__data spr = { .name = name ? sx_strpool_add(g_spr.name_pool, name, name_len) : 0,
                         .size = src->size,
                         .origin = src->origin,
                         .color = src->color,
                         .flip = src->flip,
                         .clip = src->clip,
                         .atlas = src->atlas,
                         .atlas_sprite_id = src->atlas_sprite_id,
                         .texture = src->texture,
                         .draw_bounds = src->draw_bounds,
                         .bounds = src->bounds };

    // if new clip is set, override the previous one
    if (clip_handle.id) {
        sx_assert_rel(sx_handle_valid(g_spr.animclip_handles, clip_handle.id));
        sprite__animclip* clip = &g_spr.animclips[sx_handle_index(clip_handle.id)];
        sx_assert(clip->num_frames > 0);
        spr.atlas = clip->atlas;
        spr.atlas_sprite_id = (int)clip->frames[clip->frame_id].atlas_id;
        atlas__data* atlas = the_asset->obj(clip->atlas).ptr;
        spr.texture = atlas->a.texture;
    }

    if (spr.atlas.id) {
        the_asset->ref_add(spr.atlas);
    } else {
        sx_assert(spr.texture.id);
        the_asset->ref_add(spr.texture);
    }

    sx_array_push_byindex(g_spr.alloc, g_spr.sprites, spr, sx_handle_index(handle));

    return (rizz_sprite){ handle };
}

static void sprite__destroy(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));

    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    if (spr->atlas.id) {
        the_asset->unload(spr->atlas);
    } else if (spr->texture.id) {
        the_asset->unload(spr->texture);
    }

    if (spr->name) {
        sx_strpool_del(g_spr.name_pool, spr->name);
    }

    sx_handle_del(g_spr.sprite_handles, handle.id);
}

static sx_vec2 sprite__size(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->size;
}

static sx_vec2 sprite__origin(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->origin;
}

static sx_color sprite__color(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->color;
}

static const char* sprite__name(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return sx_strpool_cstr(g_spr.name_pool, spr->name);
}

static sx_rect sprite__bounds(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->bounds;
}

static sx_rect sprite__draw_bounds(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->draw_bounds;
}

static rizz_sprite_flip sprite__flip(rizz_sprite handle)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    return spr->flip;
}

static void sprite__set_size(rizz_sprite handle, const sx_vec2 size)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    spr->size = size;
    sprite__update_bounds(spr);
}

static void sprite__set_origin(rizz_sprite handle, const sx_vec2 origin)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    spr->origin = origin;
    sprite__update_bounds(spr);
}

static void sprite__set_color(rizz_sprite handle, const sx_color color)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    spr->color = color;
}

static void sprite__set_flip(rizz_sprite handle, rizz_sprite_flip flip)
{
    sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, handle.id));
    sprite__data* spr = &g_spr.sprites[sx_handle_index(handle.id)];
    spr->flip = flip;
    sprite__update_bounds(spr);
}

// draw-data
static rizz_sprite_drawdata* sprite__drawdata_make_batch(const rizz_sprite* sprs, int num_sprites,
                                                         const sx_alloc* alloc)
{
    sx_assert(num_sprites > 0);
    sx_assert(sprs);

    // count final vertices and indices,
    int num_verts = 0;
    int num_indices = 0;
    for (int i = 0; i < num_sprites; i++) {
        sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, sprs[i].id));

        int index = sx_handle_index(sprs[i].id);
        sprite__data* spr = &g_spr.sprites[index];

        // check clip/controller handle validity and fetch rendering frame
        if (spr->ctrl.id) {
            spr->clip = sprite__animctrl_clip(spr->ctrl);
        }

        if (spr->clip.id) {
            sprite__sync_with_animclip(spr);
        }

        if (spr->atlas.id && spr->atlas_sprite_id >= 0) {
            const atlas__data* atlas = (atlas__data*)the_asset->obj_threadsafe(spr->atlas).ptr;
            sx_assert(spr->atlas_sprite_id < atlas->a.info.num_sprites);

            const atlas__sprite* aspr = &atlas->sprites[spr->atlas_sprite_id];
            num_verts += aspr->num_verts;
            num_indices += aspr->num_indices;
        } else {
            num_verts += 4;
            num_indices += 6;
        }
    }

    // assume that every sprite is a batch, so we can pre-allocate loosely
    int total_sz = sizeof(rizz_sprite_drawdata) + num_verts * sizeof(rizz_sprite_vertex) +
                   num_indices * sizeof(uint16_t) +
                   (sizeof(rizz_sprite_drawbatch) + sizeof(rizz_sprite_drawsprite)) * num_sprites;
    rizz_sprite_drawdata* dd = sx_malloc(alloc, total_sz);
    if (!dd) {
        sx_out_of_memory();
        return NULL;
    }

    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    uint64_t* keys = sx_malloc(tmp_alloc, sizeof(uint64_t) * num_sprites);
    sx_assert(keys);

    for (int i = 0; i < num_sprites; i++) {
        sx_assert_rel(sx_handle_valid(g_spr.sprite_handles, sprs[i].id));

        int index = sx_handle_index(sprs[i].id);
        sprite__data* spr = &g_spr.sprites[index];

        keys[i] = ((uint64_t)spr->texture.id << 32) | (uint64_t)index;
    }

    // sort sprites:
    //      high-bits (32): texture handle. main batching
    //      low-bits  (32): sprite index. cache coherence
    if (num_sprites > 1)
        sprite__sort_tim_sort(keys, num_sprites);

    memset(dd, 0x0, sizeof(rizz_sprite_drawdata));
    uint8_t* buff = (uint8_t*)(dd + 1);
    dd->sprites = (rizz_sprite_drawsprite*)buff;
    buff += sizeof(rizz_sprite_drawsprite) * num_sprites;
    dd->batches = (rizz_sprite_drawbatch*)buff;
    buff += sizeof(rizz_sprite_drawbatch) * num_sprites;
    dd->verts = (rizz_sprite_vertex*)buff;
    buff += sizeof(rizz_sprite_vertex) * num_verts;
    dd->indices = (uint16_t*)buff;
    dd->num_batches = 0;
    dd->num_verts = num_verts;
    dd->num_indices = num_indices;

    // fill buffers and batch
    rizz_sprite_vertex* verts = dd->verts;
    uint16_t* indices = dd->indices;
    int index_idx = 0;
    int vertex_idx = 0;
    uint32_t last_batch_key = 0;
    int num_batches = 0;

    for (int i = 0; i < num_sprites; i++) {
        int index = (int)(keys[i] & 0xffffffff);
        const sprite__data* spr = &g_spr.sprites[index];
        sx_color color = spr->color;
        int index_start = index_idx;
        int vertex_start = vertex_idx;

        // there are two types of sprites :
        //  - atlas sprites
        //  - single texture sprites
        if (spr->atlas.id && spr->atlas_sprite_id >= 0) {
            // extract sprite rectangle and uv from atlas
            const atlas__data* atlas = (atlas__data*)the_asset->obj_threadsafe(spr->atlas).ptr;
            const atlas__sprite* aspr = &atlas->sprites[spr->atlas_sprite_id];
            sx_vec2 size = sprite__calc_size(spr->size, aspr->base_size, spr->flip);
            sx_vec2 origin = spr->origin;

            const rizz_sprite_vertex* src_verts = &atlas->vertices[aspr->vb_index];
            const uint16_t* src_indices = &atlas->indices[aspr->ib_index];
            rizz_sprite_vertex* dst_verts = &verts[vertex_idx];
            uint16_t* dst_indices = &indices[index_idx];

            for (int ii = 0, c = aspr->num_verts; ii < c; ii++) {
                dst_verts[ii].pos = sx_vec2_mul(sx_vec2_sub(src_verts[ii].pos, origin), size);
                dst_verts[ii].uv = src_verts[ii].uv;
                dst_verts[ii].color = color;
            }

            for (int ii = 0, c = aspr->num_indices; ii < c; ii += 3) {
                dst_indices[ii] = src_indices[ii] + vertex_start;
                dst_indices[ii + 1] = src_indices[ii + 1] + vertex_start;
                dst_indices[ii + 2] = src_indices[ii + 2] + vertex_start;
            }

            vertex_idx += aspr->num_verts;
            index_idx += aspr->num_indices;
        } else {
            // normal texture sprite: there is no atalas. sprite takes the whole texture
            rizz_texture* tex = (rizz_texture*)the_asset->obj_threadsafe(spr->texture).ptr;
            sx_assert(tex);
            sx_vec2 base_size = sx_vec2f((float)tex->info.width, (float)tex->info.height);
            sx_vec2 size = sprite__calc_size(spr->size, base_size, spr->flip);
            sx_vec2 origin = spr->origin;
            sx_rect rect = sx_rectf(-0.5f, -0.5f, 0.5f, 0.5f);

            verts[0].pos = sx_vec2_mul(sx_vec2_sub(sx_rect_corner(&rect, 0), origin), size);
            verts[0].uv = sx_vec2f(0.0f, 1.0f);
            verts[0].color = color;
            verts[1].pos = sx_vec2_mul(sx_vec2_sub(sx_rect_corner(&rect, 1), origin), size);
            verts[1].uv = sx_vec2f(1.0f, 1.0f);
            verts[2].color = color;
            verts[2].pos = sx_vec2_mul(sx_vec2_sub(sx_rect_corner(&rect, 2), origin), size);
            verts[2].uv = sx_vec2f(0.0f, 0.0f);
            verts[2].color = color;
            verts[3].pos = sx_vec2_mul(sx_vec2_sub(sx_rect_corner(&rect, 3), origin), size);
            verts[3].uv = sx_vec2f(1.0f, 0.0f);
            verts[3].color = color;

            // clang-format off
            int v = vertex_start;
            indices[3] = v;         indices[4] = v + 2;     indices[5] = v + 1;
            indices[0] = v + 1;     indices[1] = v + 2;     indices[2] = v + 3;
            // clang-format on            

            vertex_idx += 4;
            index_idx += 6;
        }

        // batch by texture
        uint32_t key = spr->texture.id;
        if (last_batch_key != key) {
            rizz_sprite_drawbatch* batch = &dd->batches[num_batches++];
            batch->texture = spr->texture;
            batch->index_start = index_start;
            batch->index_count = index_idx - index_start;
            last_batch_key = key;
        } else {
            sx_assert(num_batches > 0);
            rizz_sprite_drawbatch* batch = &dd->batches[num_batches-1];
            batch->index_count += (index_idx - index_start);
        }

        dd->sprites[index] = (rizz_sprite_drawsprite) {
            .index = index,
            .start_vertex = vertex_start,
            .start_index = index_start,
            .num_verts = vertex_idx - vertex_start,
            .num_indices = index_idx - index_start
        };        
    }

    dd->num_indices = num_indices;
    dd->num_verts = num_verts;
    dd->num_batches = num_batches;
    dd->num_sprites = num_sprites;

    the_core->tmp_alloc_pop();
    return dd;
}

static rizz_sprite_drawdata* sprite__drawdata_make(rizz_sprite spr, const sx_alloc* alloc) {
    return sprite__drawdata_make_batch(&spr, 1, alloc);
}

static void sprite__drawdata_free(rizz_sprite_drawdata* data, const sx_alloc* alloc) {
    sx_free(alloc, data);
}

static void sprite__draw_batch(const rizz_sprite* sprs, int num_sprites, const sx_mat4* vp, 
                               const sx_mat3* mats, sx_color* tints) {
    sx_unused(tints);   // TODO

    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    rizz_sprite_drawdata* dd =
        sprite__drawdata_make_batch(sprs, num_sprites, tmp_alloc);
    if (!dd) {
        sx_assert(0 && "out of memory");
        return;
    }
    
    if (!tints) {
        tints = sx_malloc(tmp_alloc, sizeof(sx_color)*num_sprites);
        if (!tints) {
            sx_assert(0 && "out of memory");
            return;
        }
        for (int i = 0; i < num_sprites; i++) 
            tints[i] = sx_colorn(0xffffffff);
    }

    const sprite__draw_context* dc = &g_spr.drawctx;

    // append drawdata to buffers
    int ib_offset = the_gfx->staged.append_buffer(dc->ibuff, dd->indices, 
                                                  sizeof(uint16_t)*dd->num_indices);
    int vb_offset1 = the_gfx->staged.append_buffer(dc->vbuff[0], dd->verts, 
                                                   sizeof(rizz_sprite_vertex)*dd->num_verts);

    sprite__vertex_transform* tverts = sx_malloc(tmp_alloc, 
                                                 sizeof(sprite__vertex_transform)*dd->num_verts);
    sx_assert(tverts);

    // put transforms into another vbuff
    for (int i = 0; i < dd->num_sprites; i++) {
        rizz_sprite_drawsprite* dspr = &dd->sprites[i];
        const sx_mat3* m = &mats[i];

        sx_vec3 t1 = sx_vec3f(m->m11, m->m12, m->m21);
        sx_vec3 t2 = sx_vec3f(m->m22, m->m13, m->m23);
        int end_vertex = dspr->start_vertex + dspr->num_verts;
        for (int v = dspr->start_vertex; v < end_vertex; v++) {
            tverts[v].t1 = t1;
            tverts[v].t2 = t2;
            tverts[v].color = tints[i].n;
        }
    }
    int vb_offset2 = the_gfx->staged.append_buffer(dc->vbuff[1], tverts, 
                                                   sizeof(sprite__vertex_transform)*dd->num_verts);

    sg_bindings bindings = {
        .index_buffer = g_spr.drawctx.ibuff,
        .vertex_buffers[0] = g_spr.drawctx.vbuff[0],
        .vertex_buffers[1] = g_spr.drawctx.vbuff[1],
        .vertex_buffer_offsets[0] = vb_offset1,
        .vertex_buffer_offsets[1] = vb_offset2,
        .index_buffer_offset = ib_offset,
    };

    the_gfx->staged.apply_pipeline(dc->pip);
    the_gfx->staged.apply_uniforms(SG_SHADERSTAGE_VS, 0, vp, sizeof(*vp));

    // draw with batching
    for (int i = 0; i < dd->num_batches; i++)  {
        rizz_sprite_drawbatch* batch = &dd->batches[i];
        bindings.fs_images[0] = ((rizz_texture*)the_asset->obj_threadsafe(batch->texture).ptr)->img;
        the_gfx->staged.apply_bindings(&bindings);
        the_gfx->staged.draw(batch->index_start, batch->index_count, 1);
    }    

    the_core->tmp_alloc_pop();
}

static void sprite__draw(rizz_sprite spr, const sx_mat4* vp, const sx_mat3* mat, sx_color tint) {
    sprite__draw_batch(&spr, 1, vp, mat, &tint);
}

static void sprite__draw_wireframe_batch(const rizz_sprite* sprs, int num_sprites, const sx_mat4* vp, 
                                         const sx_mat3* mats) {
    const sx_alloc* tmp_alloc = the_core->tmp_alloc_push();

    rizz_sprite_drawdata* dd =
        sprite__drawdata_make_batch(sprs, num_sprites, tmp_alloc);
    if (!dd) {
        sx_assert(0);
        return;
    }

    const sprite__draw_context* dc = &g_spr.drawctx;

    rizz_sprite_vertex* verts = sx_malloc(tmp_alloc, sizeof(rizz_sprite_vertex)*dd->num_indices);
    sx_assert(verts);
    sprite__vertex_transform* tverts = sx_malloc(tmp_alloc, 
                                                 sizeof(sprite__vertex_transform)*dd->num_indices);
    sx_assert(tverts);
    const sx_vec3 bcs[] = { {{ 1.0f, 0, 0 }}, {{ 0, 1.0f, 0 }}, {{ 0, 0, 1.0f }} };

    // put transforms into another vbuff
    int v = 0;
    for (int i = 0; i < dd->num_sprites; i++) {
        rizz_sprite_drawsprite* dspr = &dd->sprites[i];
        const sx_mat3* m = &mats[i];

        sx_vec3 t1 = sx_vec3f(m->m11, m->m12, m->m21);
        sx_vec3 t2 = sx_vec3f(m->m22, m->m13, m->m23);
        int end_index = dspr->start_index + dspr->num_indices;
        for (int ii = dspr->start_index; ii < end_index; ii++) {
            verts[v] = dd->verts[dd->indices[ii]];

            tverts[v].t1 = t1;
            tverts[v].t2 = t2;
            tverts[v].bc = bcs[v % 3];
            v++;
        }
    }
    
    int vb_offset1 = the_gfx->staged.append_buffer(dc->vbuff[0], verts, 
                                                   sizeof(rizz_sprite_vertex)*dd->num_indices);
    int vb_offset2 = the_gfx->staged.append_buffer(dc->vbuff[1], tverts, 
                                                   sizeof(sprite__vertex_transform)*dd->num_indices);

    sg_bindings bindings = {
        .vertex_buffers[0] = g_spr.drawctx.vbuff[0],
        .vertex_buffers[1] = g_spr.drawctx.vbuff[1],
        .vertex_buffer_offsets[0] = vb_offset1,
        .vertex_buffer_offsets[1] = vb_offset2,
    };

    the_gfx->staged.apply_pipeline(dc->pip_wire);
    the_gfx->staged.apply_uniforms(SG_SHADERSTAGE_VS, 0, vp, sizeof(*vp));

    // draw with batching
    for (int i = 0; i < dd->num_batches; i++)  {
        rizz_sprite_drawbatch* batch = &dd->batches[i];
        bindings.fs_images[0] = ((rizz_texture*)the_asset->obj_threadsafe(batch->texture).ptr)->img;
        the_gfx->staged.apply_bindings(&bindings);
        the_gfx->staged.draw(batch->index_start, batch->index_count, 1);
    }    

    the_core->tmp_alloc_pop();
}

static void sprite__draw_wireframe(rizz_sprite spr, const sx_mat4* vp, const sx_mat3* mat)
{
    sprite__draw_wireframe_batch(&spr, 1, vp, mat);
}

static void sprite__show_sprite_preview(sprite__data* spr) {
    the_imgui->BeginChild("sprite_preview", SX_VEC2_ZERO, false, 0);
    {
        ImDrawList* draw_list = the_imgui->GetWindowDrawList();
        sx_vec2     wsize;
        sx_vec2     wpos;
        the_imgui->GetWindowSize_nonUDT(&wsize);
        the_imgui->GetWindowPos_nonUDT(&wpos);
        sx_vec2 padded_wsize = sx_vec2_mulf(wsize, 0.9f);
        sx_rect sprite_rect;
        sx_vec2 uv1, uv2;

        if (spr->atlas.id) {
            const atlas__data* atlas = (atlas__data*)the_asset->obj(spr->atlas).ptr;
            sx_assert(spr->atlas_sprite_id < atlas->a.info.num_sprites);
            const atlas__sprite* aspr = &atlas->sprites[spr->atlas_sprite_id];

            sx_vec2 base_size = aspr->base_size;
            sx_vec2 atlas_size_rcp =
                sx_vec2f(1.0f / atlas->a.info.img_width, 1.0f / atlas->a.info.img_height);
            uv1 = sx_vec2_mul(aspr->sheet_rect.vmin, atlas_size_rcp);
            uv2 = sx_vec2_mul(aspr->sheet_rect.vmax, atlas_size_rcp);

            // normalize sprite_rect
            sx_vec2 base_size_rcp = sx_vec2f(1.0f / base_size.x, 1.0f / base_size.y);
            sprite_rect = sx_rectv(sx_vec2_mul(aspr->sprite_rect.vmin, base_size_rcp),
                                   sx_vec2_mul(aspr->sprite_rect.vmax, base_size_rcp));

            // fit into padded_size
            sx_vec2 s1 = sprite__calc_size(sx_vec2f(padded_wsize.x, 0), base_size, 0);
            sx_vec2 s2 = sprite__calc_size(sx_vec2f(0, padded_wsize.y), base_size, 0);
            if (s1.y <= padded_wsize.y)
                padded_wsize = s1;
            else if (s2.x <= padded_wsize.x)
                padded_wsize = s2;
        } else {
            uv1 = sx_vec2f(0.0f, 0.0f);
            uv2 = sx_vec2f(1.0f, 1.0f);
            rizz_texture* tex = (rizz_texture*)the_asset->obj(spr->texture).ptr;
            sx_vec2 base_size = sx_vec2f((float)tex->info.width, (float)tex->info.height);
            sprite_rect = sx_rectf(0, 0, base_size.x, base_size.y);

            // fit into padded_size
            sx_vec2 s1 = sprite__calc_size(sx_vec2f(padded_wsize.x, 0), base_size, 0);
            sx_vec2 s2 = sprite__calc_size(sx_vec2f(0, padded_wsize.y), base_size, 0);
            if (s1.y <= padded_wsize.y)
                padded_wsize = s1;
            else if (s2.x <= padded_wsize.x)
                padded_wsize = s2;
        }

        wpos = sx_vec2_add(wpos, sx_vec2_mulf(sx_vec2_sub(wsize, padded_wsize), 0.5f));
        wsize = padded_wsize;

        sx_vec2     vmin = sx_vec2_add(wpos, sx_vec2_mul(sprite_rect.vmin, wsize));
        sx_vec2     vmax = sx_vec2_add(wpos, sx_vec2_mul(sprite_rect.vmax, wsize));
        ImTextureID tex_id =
            (ImTextureID)(uintptr_t)((rizz_texture*)the_asset->obj(spr->texture).ptr)->img.id;
        the_imgui->ImDrawList_AddImage(draw_list, tex_id, vmin, vmax, uv1, uv2, 0xffffffff);

        // base frame
        the_imgui->ImDrawList_AddRect(draw_list, wpos, sx_vec2_add(wpos, wsize),
                                      sx_color4u(255, 255, 0, 255).n, 0, 0, 1.0f);

        // sprite frame
        the_imgui->ImDrawList_AddRect(draw_list, vmin, vmax, sx_color4u(255, 0, 0, 255).n, 0, 0,
                                      1.0f);

        // origin
        sx_vec2 origin = sx_vec2_add(
            sx_vec2_mul(sx_vec2f(spr->origin.x + 0.5f, 0.5f - spr->origin.y), wsize), wpos);
        the_imgui->ImDrawList_AddCircleFilled(draw_list, origin, 5.0f, sx_color4u(0, 255, 0, 255).n,
                                              6);
    }

    the_imgui->EndChild();
    
}

static void sprite__show_sprite_tab_contents(sprite__data* spr) {
    sx_vec2 base_size;
    if (spr->atlas.id) {
        const atlas__data* atlas = (atlas__data*)the_asset->obj(spr->atlas).ptr;
        sx_assert(spr->atlas_sprite_id < atlas->a.info.num_sprites);
        const atlas__sprite* aspr = &atlas->sprites[spr->atlas_sprite_id];
        base_size = aspr->base_size;
    } else {
        rizz_texture* tex = (rizz_texture*)the_asset->obj(spr->texture).ptr;
        base_size = sx_vec2f((float)tex->info.width, (float)tex->info.height);
    }

    // 
    the_imgui->Columns(2, "sprite_cols", true);
    the_imgui->BeginChild("sprite_info", SX_VEC2_ZERO, false, 0);

    the_imgui->Columns(2, "sprite_info_cols", false);
    the_imgui->SetColumnWidth(0, 70.0f);
    
    // clang-format off
    the_imgui->Text("atlas");   the_imgui->NextColumn();
    the_imgui->Text("0x%x", spr->atlas.id); the_imgui->NextColumn();    
    the_imgui->Text("atlas_id"); the_imgui->NextColumn();
    the_imgui->Text("%d", spr->atlas_sprite_id); the_imgui->NextColumn();    
    the_imgui->Text("texture"); the_imgui->NextColumn();
    the_imgui->Text("0x%x", spr->texture.id); the_imgui->NextColumn();
    the_imgui->Text("size"); the_imgui->NextColumn();
    the_imgui->Text("(%.2f, %.2f)", spr->size.x, spr->size.y); the_imgui->NextColumn();    
    the_imgui->Text("color"); the_imgui->NextColumn();
    sx_vec4 color = sx_color_vec4(spr->color);
    the_imgui->ColorButton("sprite_color", color, 0, SX_VEC2_ZERO); the_imgui->NextColumn();
    the_imgui->Text("origin"); the_imgui->NextColumn();
    the_imgui->DragFloat2("", spr->origin.f, 0.01f, -0.5f, 0.5f, "%.2f", 1.0);
    the_imgui->NextColumn();
    the_imgui->Text("dbounds"); the_imgui->NextColumn();
    the_imgui->Text("(%.2f, %.2f, %.2f, %.2f)", spr->draw_bounds.xmin, spr->draw_bounds.ymin, 
        spr->draw_bounds.xmax, spr->draw_bounds.ymax);  the_imgui->NextColumn();
    the_imgui->Text("bounds"); the_imgui->NextColumn();
    the_imgui->Text("(%.2f, %.2f, %.2f, %.2f)", spr->bounds.xmin, spr->bounds.ymin, 
        spr->bounds.xmax, spr->bounds.ymax);  the_imgui->NextColumn();
    the_imgui->Text("base_size"); the_imgui->NextColumn();
    the_imgui->Text("(%.0f, %.0f)", base_size.x, base_size.y);  the_imgui->NextColumn();
    // clang-format on
    the_imgui->EndChild();
    the_imgui->NextColumn();

    sprite__show_sprite_preview(spr);
    the_imgui->NextColumn();
    the_imgui->Columns(1, NULL, false);
}

static void sprite__show_animclip_tab_contents(sprite__data* spr)
{
    sx_assert(spr->clip.id);
    sx_assert(sx_handle_valid(g_spr.animclip_handles, spr->clip.id));

    sprite__animclip* clip = &g_spr.animclips[sx_handle_index(spr->clip.id)];

    the_imgui->Columns(2, "animclip_cols", true);

    the_imgui->BeginChild("animclip_info", SX_VEC2_ZERO, false, 0);
    the_imgui->Columns(2, "animclip_info_cols", false);
    the_imgui->SetColumnWidth(0, 80.0f);

    the_imgui->Text("num_frames");
    the_imgui->NextColumn();
    the_imgui->Text("%d", clip->num_frames);
    the_imgui->NextColumn();
    the_imgui->Text("time");
    the_imgui->NextColumn();
    the_imgui->Text("%.3f", clip->tm);
    the_imgui->NextColumn();
    the_imgui->Text("frame");
    the_imgui->NextColumn();
    the_imgui->Text("%d", clip->frame_id);
    the_imgui->NextColumn();
    the_imgui->Text("duration");
    the_imgui->NextColumn();
    the_imgui->Text("%.2f", clip->len);
    the_imgui->NextColumn();
    the_imgui->Text("fps");
    the_imgui->NextColumn();
    if (the_imgui->DragFloat("", &clip->fps, 0.1f, 0.1f, 200.0f, "%.1f", 1.0f)) {
        clip->len = (float)clip->num_frames / clip->fps;
    }
    the_imgui->NextColumn();

    the_imgui->EndChild();
    the_imgui->NextColumn();

    sprite__show_sprite_preview(spr);
    the_imgui->NextColumn();
    the_imgui->Columns(1, NULL, false);
}

static void sprite__show_animctrl_tab_contents(sprite__data* spr)
{
    sx_assert(spr->ctrl.id);
    sx_assert(sx_handle_valid(g_spr.animclip_handles, spr->ctrl.id));

    sprite__animctrl* ctrl = &g_spr.animctrls[sx_handle_index(spr->ctrl.id)];

    the_imgui->Columns(2, "animctrl_cols", true);

    the_imgui->BeginChild("animctrl_info", SX_VEC2_ZERO, false, 0);
    the_imgui->Columns(2, "animctrl_info_cols", false);
    the_imgui->SetColumnWidth(0, 80.0f);

    the_imgui->Text("state");
    the_imgui->NextColumn();
    the_imgui->Text(ctrl->state->name);
    the_imgui->NextColumn();
    the_imgui->Columns(1, NULL, false);

    if (the_imgui->CollapsingHeader("Params", ImGuiTreeNodeFlags_DefaultOpen)) {
        the_imgui->BeginChild("params", SX_VEC2_ZERO, false, 0);
        the_imgui->Columns(2, "params_cols", false);
        the_imgui->SetColumnWidth(0, 80.0f);
        for (sprite__animctrl_param* p = &ctrl->params[0]; p->name_hash; p++) {
            the_imgui->Text(p->name);
            the_imgui->NextColumn();

            // check type
            char id[32];
            sx_snprintf(id, sizeof(id), "param_%s", p->name);
            the_imgui->PushIDStr(id);
            switch (p->type) {
            case RIZZ_SPRITE_PARAMTYPE_BOOL:
                the_imgui->Checkbox("", &p->value.b);
                break;
            case RIZZ_SPRITE_PARAMTYPE_BOOL_AUTO:
                the_imgui->Checkbox("(auto)", &p->value.b);
                break;
            case RIZZ_SPRITE_PARAMTYPE_FLOAT:
                the_imgui->InputFloat("", &p->value.f, 1.0f, 10.0f, "%.2f", 0);
                break;
            case RIZZ_SPRITE_PARAMTYPE_INT:
                the_imgui->InputInt("", &p->value.i, 1, 10, 0);
                break;
            }
            the_imgui->PopID();
            the_imgui->NextColumn();
        }
        the_imgui->EndChild();
    }

    the_imgui->EndChild();
    the_imgui->NextColumn();

    sprite__show_sprite_preview(spr);
    the_imgui->NextColumn();
    the_imgui->Columns(1, NULL, false);
}

static void sprite__show_debugger(bool* p_open)
{
    if (!the_imgui || g_spr.sprite_handles->count == 0) {
        return;
    }

    static int selected_sprite = -1;

    int num_items = g_spr.sprite_handles->count;
    the_imgui->SetNextWindowSizeConstraints(sx_vec2f(350.0f, 500.0f), sx_vec2f(FLT_MAX, FLT_MAX),
                                            NULL, NULL);
    if (the_imgui->Begin("Sprite Debugger", p_open, 0)) {
        the_imgui->Columns(3, NULL, false);
        the_imgui->SetColumnWidth(0, 70.0f);
        the_imgui->Text("Handle");
        the_imgui->NextColumn();
        the_imgui->SetColumnWidth(1, 200.0f);
        the_imgui->Text("Name");
        the_imgui->NextColumn();
        the_imgui->Text("Image");
        the_imgui->NextColumn();
        the_imgui->Separator();

        the_imgui->Columns(1, NULL, false);
        the_imgui->BeginChild("sprite_list",
                              sx_vec2f(the_imgui->GetWindowContentRegionWidth(), 100.0f), false, 0);
        the_imgui->Columns(3, NULL, false);

        ImGuiListClipper clipper;
        the_imgui->ImGuiListClipper_Begin(&clipper, num_items, -1.0f);
        char handle_str[32];

        while (the_imgui->ImGuiListClipper_Step(&clipper)) {
            int start = num_items - clipper.DisplayStart - 1;
            int end = num_items - clipper.DisplayEnd;
            for (int i = start; i >= end; i--) {
                sx_handle_t handle = sx_handle_at(g_spr.sprite_handles, i);
                sprite__data* spr = &g_spr.sprites[sx_handle_index(handle)];
                sx_snprintf(handle_str, sizeof(handle_str), "0x%x", handle);
                the_imgui->SetColumnWidth(0, 70.0f);
                if (the_imgui->Selectable(handle_str, selected_sprite == i,
                                          ImGuiSelectableFlags_SpanAllColumns, SX_VEC2_ZERO)) {
                    selected_sprite = i;
                }
                the_imgui->NextColumn();

                the_imgui->SetColumnWidth(1, 200.0f);
                the_imgui->Text(spr->name ? sx_strpool_cstr(g_spr.name_pool, spr->name)
                                          : "[noname]");
                the_imgui->NextColumn();

                if (spr->texture.id) {
                    the_imgui->Text(the_asset->path(spr->texture));
                } else {
                    the_imgui->Text("");
                }

                the_imgui->NextColumn();
            }
        }
        the_imgui->ImGuiListClipper_End(&clipper);
        the_imgui->EndChild();

        if (selected_sprite != -1 && the_imgui->BeginTabBar("sprite_tab", 0)) {
            sx_handle_t handle = sx_handle_at(g_spr.sprite_handles, selected_sprite);
            sprite__data* spr = &g_spr.sprites[sx_handle_index(handle)];

            if (the_imgui->BeginTabItem("Sprite", NULL, 0)) {
                sprite__show_sprite_tab_contents(spr);
                the_imgui->EndTabItem();
            }

            if (spr->clip.id && the_imgui->BeginTabItem("AnimClip", NULL, 0)) {
                sprite__show_animclip_tab_contents(spr);
                the_imgui->EndTabItem();
            }

            if (spr->ctrl.id && the_imgui->BeginTabItem("AnimCtrl", NULL, 0)) {
                sprite__show_animctrl_tab_contents(spr);
                the_imgui->EndTabItem();
            }

            the_imgui->EndTabBar();
        }
    }
    the_imgui->End();
}

static rizz_api_sprite the__sprite = { .create = sprite__create,
                                       .destroy = sprite__destroy,
                                       .clone = sprite__clone,
                                       .size = sprite__size,
                                       .origin = sprite__origin,
                                       .bounds = sprite__bounds,
                                       .flip = sprite__flip,
                                       .set_size = sprite__set_size,
                                       .set_origin = sprite__set_origin,
                                       .set_color = sprite__set_color,
                                       .set_flip = sprite__set_flip,
                                       .make_drawdata = sprite__drawdata_make,
                                       .make_drawdata_batch = sprite__drawdata_make_batch,
                                       .free_drawdata = sprite__drawdata_free,
                                       .draw = sprite__draw,
                                       .draw_batch = sprite__draw_batch,
                                       .draw_wireframe_batch = sprite__draw_wireframe_batch,
                                       .resize_draw_limits = sprite__resize_draw_limits,
                                       .animclip_create = sprite__animclip_create,
                                       .animclip_destroy = sprite__animclip_destroy,
                                       .animclip_clone = sprite__animclip_clone,
                                       .animclip_update = sprite__animclip_update,
                                       .animclip_update_batch = sprite__animclip_update_batch,
                                       .animclip_fps = sprite__animclip_fps,
                                       .animclip_len = sprite__animclip_len,
                                       .animclip_events = sprite__animclip_events,
                                       .animclip_set_fps = sprite__animclip_set_fps,
                                       .animclip_set_len = sprite__animclip_set_len,
                                       .animclip_restart = sprite__animclip_restart,
                                       .animctrl_create = sprite__animctrl_create,
                                       .animctrl_destroy = sprite__animctrl_destroy,
                                       .animctrl_update = sprite__animctrl_update,
                                       .animctrl_update_batch = sprite__animctrl_update_batch,
                                       .animctrl_set_paramb = sprite__animctrl_set_paramb,
                                       .animctrl_set_parami = sprite__animctrl_set_parami,
                                       .animctrl_set_paramf = sprite__animctrl_set_paramf,
                                       .animctrl_param_valueb = sprite__animctrl_param_valueb,
                                       .animctrl_param_valuei = sprite__animctrl_param_valuei,
                                       .animctrl_param_valuef = sprite__animctrl_param_valuef,
                                       .animctrl_restart = sprite__animctrl_restart,
                                       .show_debugger = sprite__show_debugger };

rizz_plugin_decl_main(sprite, plugin, e)
{
    switch (e) {
    case RIZZ_PLUGIN_EVENT_STEP:
        break;

    case RIZZ_PLUGIN_EVENT_INIT:
        the_plugin = plugin->api;
        the_core = the_plugin->get_api(RIZZ_API_CORE, 0);
        the_asset = the_plugin->get_api(RIZZ_API_ASSET, 0);
        the_refl = the_plugin->get_api(RIZZ_API_REFLECT, 0);
        the_gfx = the_plugin->get_api(RIZZ_API_GFX, 0);
        the_imgui = the_plugin->get_api_byname("imgui", 0);
        the_imguix = the_plugin->get_api_byname("imgui_extra", 0);
        if (!sprite__init()) {
            return -1;
        }
        the_plugin->inject_api("sprite", 0, &the__sprite);
        break;

    case RIZZ_PLUGIN_EVENT_LOAD:
        the_plugin->inject_api("sprite", 0, &the__sprite);
        break;

    case RIZZ_PLUGIN_EVENT_UNLOAD:
        break;

    case RIZZ_PLUGIN_EVENT_SHUTDOWN:
        the_plugin->remove_api("sprite", 0);
        sprite__release();
        break;
    }

    return 0;
}

rizz_plugin_decl_event_handler(sprite, e)
{
    if (e->type == RIZZ_APP_EVENTTYPE_UPDATE_APIS) {
        the_imgui = the_plugin->get_api_byname("imgui", 0);
        the_imguix = the_plugin->get_api_byname("imgui_extra", 0);
    }
}

static const char* sprite__deps[] = { "imgui" };
rizz_plugin_implement_info(sprite, 1000, "sprite plugin", sprite__deps, 1);
