#include "sx/allocator.h"
#include "sx/math.h"
#include "sx/os.h"
#include "sx/string.h"
#include "sx/timer.h"

#include "rizz/app.h"
#include "rizz/asset.h"
#include "rizz/camera.h"
#include "rizz/core.h"
#include "rizz/entry.h"
#include "rizz/graphics.h"
#include "rizz/imgui-extra.h"
#include "rizz/imgui.h"
#include "rizz/plugin.h"
#include "rizz/vfs.h"

#include "../common.h"
#include "./game.h"

RIZZ_STATE static rizz_api_core* the_core;
RIZZ_STATE static rizz_api_gfx* the_gfx;
RIZZ_STATE static rizz_api_app* the_app;
RIZZ_STATE static rizz_api_imgui* the_imgui;
RIZZ_STATE static rizz_api_asset* the_asset;
RIZZ_STATE static rizz_api_imgui_extra* the_imguix;
RIZZ_STATE static rizz_api_camera* the_camera;
RIZZ_STATE static rizz_api_vfs* the_vfs;

extern const char *vs_src, *fs_src;

const int SAMPLE_COUNT = 4;

typedef struct {
    rizz_gfx_stage stage;
    sg_bindings bindings;
    sg_pipeline pip;
    rizz_asset img;
    rizz_asset shader;
    sg_buffer vbuff;
    sg_buffer ibuff;
    rizz_camera_fps cam;
} quad_state;

RIZZ_STATE static quad_state g_quad;

static sprite_data_t* sprite_data;
static uint64_t time;

typedef struct {
    float aspect;
} vs_params_t;

void init(void) {

	#if SX_PLATFORM_ANDROID
    the_vfs->mount_android_assets("/assets");
#else
    // mount `/asset` directory
    char asset_dir[RIZZ_MAX_PATH];
    sx_os_path_join(asset_dir, sizeof(asset_dir), EXAMPLES_ROOT, "assets");    // "/examples/assets"
    the_vfs->mount(asset_dir, "/assets");
    the_vfs->watch_mounts();
#endif

	 // load assets metadata cache to speedup asset loading
    // always do this after you have mounted all virtual directories
    the_asset->load_meta_cache();

    // register main graphics stage.
    // at least one stage should be registered if you want to draw anything
    g_quad.stage = the_gfx->stage_register("main", (rizz_gfx_stage){ .id = 0 });
    sx_assert(g_quad.stage.id);

	    /* create an index buffer for a ecsminigame */
    uint16_t indices[] = {
        0, 1, 2, 2, 1, 3,
    };

	 // buffers
    g_quad.vbuff = the_gfx->imm.make_buffer(&(sg_buffer_desc){ .usage = SG_USAGE_STREAM,
                                                               .type = SG_BUFFERTYPE_VERTEXBUFFER,
															   .size = sizeof(sprite_data_t) * kMaxSpriteCount});

    g_quad.ibuff = the_gfx->imm.make_buffer(&(sg_buffer_desc){ .usage = SG_USAGE_IMMUTABLE,
                                                               .type = SG_BUFFERTYPE_INDEXBUFFER,
                                                               .size = sizeof(indices),
                                                               .content = indices });
    /* create shader */
    sg_shader shd = the_gfx->imm.make_shader(&(sg_shader_desc){
        .attrs = { 
			[0] = { .name = "posScale", .sem_name = "POSSCALE", .sem_index =  0},
            [1] = { .name ="colorIndex", .sem_name = "COLORSPRITE", .sem_index = 0}
		}, 
        .vs.uniform_blocks[0] = {
            .size = sizeof(vs_params_t)
        },
        .fs.images[0].type = SG_IMAGETYPE_2D,
        .vs.source = vs_src,
        .fs.source = fs_src,
    });
    
	g_quad.img = the_asset->load("texture", "/assets/textures/sprites.png",
                                 &(rizz_texture_load_params){ 0 }, 0, NULL, 0);

    /* create pipeline object */
    g_quad.pip = the_gfx->imm.make_pipeline(&(sg_pipeline_desc){
        .layout = {
                .buffers[0] = { .stride = sizeof(sprite_data_t),
                                .step_func = SG_VERTEXSTEP_PER_INSTANCE },
            .attrs = {
                [0] = { .offset =0, .format=SG_VERTEXFORMAT_FLOAT3 }, // instance pos + scale
                [1] = { .offset =12, .format=SG_VERTEXFORMAT_FLOAT4 }, // instance color
            },
        },
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .depth_stencil = {
            .depth_compare_func = SG_COMPAREFUNC_LESS_EQUAL,
            .depth_write_enabled = true,
        },
        .rasterizer.cull_mode = SG_CULLMODE_NONE,
        .rasterizer.sample_count = SAMPLE_COUNT,
        .blend = {
            .enabled = true,
            .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            .src_factor_alpha = SG_BLENDFACTOR_SRC_ALPHA,
            .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        },
    });

    g_quad.bindings =
        (sg_bindings){ .vertex_buffers[0] = g_quad.vbuff, .index_buffer = g_quad.ibuff };

	const sx_alloc* tmp_alloc = sx_alloc_malloc();
    sprite_data =
        (sprite_data_t*)sx_malloc(tmp_alloc, kMaxSpriteCount * sizeof(sprite_data_t));

    game_initialize();
}

static void update(float dt) {
}

void frame(float dt) {
    vs_params_t vs_params;
    const float w = (float)the_app->width();
    const float h = (float)the_app->height();
    vs_params.aspect = w / h;

    double time = sx_tm_sec(the_core->elapsed_tick());
    //float dt = (float)sx_tm_sec(the_core->delta_tick());

	rizz_profile_begin(the_core, updateMovement, 0);
    int sprite_count = game_update(sprite_data, time, dt);
    rizz_profile_end(the_core);


    sg_pass_action pass_action = {
        .colors[0] = { .action = SG_ACTION_CLEAR, .val = { 0.1f, 0.1f, 0.1f, 1.0f } }
    };

	the_gfx->staged.begin(g_quad.stage);
    the_gfx->staged.begin_default_pass(&pass_action, the_app->width(), the_app->height());

    assert(sprite_count >= 0 && sprite_count <= kMaxSpriteCount);
    the_gfx->staged.update_buffer(g_quad.vbuff, sprite_data, sprite_count * sizeof(sprite_data[0]));

	g_quad.bindings.fs_images[0] = ((rizz_texture*)the_asset->obj(g_quad.img).ptr)->img;
    the_gfx->staged.apply_pipeline(g_quad.pip);
    the_gfx->staged.apply_bindings(&g_quad.bindings);

    the_gfx->staged.apply_uniforms(SG_SHADERSTAGE_VS, 0, &vs_params, sizeof(vs_params));
    if (sprite_count > 0) {
        the_gfx->staged.draw(0, 6, sprite_count);
	}

	the_gfx->staged.end_pass();
    the_gfx->staged.end();
}

void shutdown(void) {
    game_destroy();

    if (g_quad.vbuff.id)
        the_gfx->imm.destroy_buffer(g_quad.vbuff);
    if (g_quad.ibuff.id)
        the_gfx->imm.destroy_buffer(g_quad.ibuff);
    if (g_quad.img.id)
        the_asset->unload(g_quad.img);
    if (g_quad.shader.id)
        the_asset->unload(g_quad.shader);
    if (g_quad.pip.id)
        the_gfx->imm.destroy_pipeline(g_quad.pip);

	if (sprite_data) {
        const sx_alloc* tmp_alloc = sx_alloc_malloc();
        sx_free(tmp_alloc, sprite_data);
    }
}


const char* vs_src =
    "cbuffer params : register(b0) {\n"
    "  float aspect;\n"
    "};\n"
    "struct vs_in {\n"
    "  float4 posScale : POSSCALE;\n"
    "  float4 colorIndex : COLORSPRITE;\n"
    "  uint vid : SV_VertexID;\n"
    "};\n"
    "struct v2f {\n"
    "  float3 color : COLOR0;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 pos : SV_Position;\n"
    "};\n"
    "v2f main(vs_in inp) {\n"
    "  v2f outp;\n"
    "  float x = inp.vid / 2;\n"
    "  float y = inp.vid & 1;\n"
    "  outp.pos.x = inp.posScale.x + (x-0.5f) * inp.posScale.z;\n"
    "  outp.pos.y = inp.posScale.y + (y-0.5f) * inp.posScale.z * aspect;\n"
    "  outp.pos.z = 0.0f;\n"
    "  outp.pos.w = 1.0f;\n"
    "  outp.uv = float2((x + inp.colorIndex.w)/8,1-y);\n"
    "  outp.color = inp.colorIndex.rgb;\n"
    "  return outp;\n"
    "};\n";
const char* fs_src =
    "struct v2f {\n"
    "  float3 color: COLOR0;\n"
    "  float2 uv: TEXCOORD0;\n"
    "  float4 pos: SV_Position;\n"
    "};\n"
    "Texture2D tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "float4 main(v2f inp) : SV_Target0 {\n"
    "  float4 diffuse = tex0.Sample(smp0, inp.uv);"
    "  float lum = dot(diffuse.rgb, 0.333);\n"
    "  diffuse.rgb = lerp(diffuse.rgb, lum.xxx, 0.8);\n"
    "  diffuse.rgb *= inp.color.rgb;\n"
    "  return diffuse;\n"
    "}\n";

rizz_plugin_decl_main(ecsminigame, plugin, e)
{
    switch (e) {
    case RIZZ_PLUGIN_EVENT_STEP:
        frame((float)sx_tm_sec(the_core->delta_tick()));

        break;

    case RIZZ_PLUGIN_EVENT_INIT:
        // runs only once for application. Retreive needed APIs
        the_core = (rizz_api_core*)plugin->api->get_api(RIZZ_API_CORE, 0);
        the_gfx = (rizz_api_gfx*)plugin->api->get_api(RIZZ_API_GFX, 0);
        the_app = (rizz_api_app*)plugin->api->get_api(RIZZ_API_APP, 0);
        the_vfs = (rizz_api_vfs*)plugin->api->get_api(RIZZ_API_VFS, 0);
        the_asset = (rizz_api_asset*)plugin->api->get_api(RIZZ_API_ASSET, 0);
        the_camera = (rizz_api_camera*)plugin->api->get_api(RIZZ_API_CAMERA, 0);
        the_imgui = (rizz_api_imgui*)plugin->api->get_api_byname("imgui", 0);

        init();
        break;

    case RIZZ_PLUGIN_EVENT_LOAD:
        break;

    case RIZZ_PLUGIN_EVENT_UNLOAD:
        break;

    case RIZZ_PLUGIN_EVENT_SHUTDOWN:
        shutdown();
        break;
    }

    return 0;
}

rizz_plugin_decl_event_handler(ecsminigame, e)
{
    switch (e->type) {
    case RIZZ_APP_EVENTTYPE_SUSPENDED:
        break;
    case RIZZ_APP_EVENTTYPE_RESTORED:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_DOWN:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_UP:
        break;
    case RIZZ_APP_EVENTTYPE_MOUSE_MOVE:
        break;
    default:
        break;
    }
}

rizz_game_decl_config(conf)
{
    conf->app_name = "ecsminigame";
    conf->app_version = 1000;
    conf->app_title = "ecsminigame";
    conf->window_width = 800;
    conf->window_height = 600;
    conf->core_flags |= RIZZ_CORE_FLAG_VERBOSE;
    conf->multisample_count = 4;
    conf->swap_interval = 2;
    conf->plugins[0] = "imgui";
}
