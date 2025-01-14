//
// Copyright 2019 Sepehr Taghdisian (septag@github). All rights reserved.
// License: https://github.com/septag/rizz#license-bsd-2-clause
//
#include "config.h"

#include "rizz/asset.h"
#include "rizz/core.h"
#include "rizz/reflect.h"
#include "rizz/types.h"
#include "rizz/vfs.h"

#include "sx/allocator.h"
#include "sx/array.h"
#include "sx/atomic.h"
#include "sx/handle.h"
#include "sx/hash.h"
#include "sx/jobs.h"
#include "sx/os.h"
#include "sx/pool.h"
#include "sx/string.h"

#include "sjson/sjson.h"

// Asset managers are managers for each type of asset
// For example, 'texture' has it's own manager, 'model' has it's manager, ...
// They handle loading, unloading, reloading asset objects
// They also have metdata and parameters memory containers
typedef struct {
    char name[32];
    rizz_asset_callbacks callbacks;
    uint32_t name_hash;
    int params_size;
    int metadata_size;
    char params_type_name[32];
    char metadata_type_name[32];
    rizz_asset_obj failed_obj;
    rizz_asset_obj async_obj;
    uint8_t* params_buff;                  // sx_array (byte-array, item-size: params_size)
    uint8_t* metadata_buff;                // sx_array (byte-array, item-size: metadata_size)
    rizz_asset_load_flags forced_flags;    // these flags are foced upon every load-call
    bool unreg;
} rizz__asset_mgr;

// Assets consist of files (resource) on disk and load params combination
// One file may be loaded with different parameters (different allocator, mip-map, LOD, etc.)
// Each asset is binded to a resource and params data
typedef struct {
    const sx_alloc* alloc;
    sx_handle_t handle;
    uint32_t params_id;      // id-to: rizz__asset_mgr.params_buff
    uint32_t resource_id;    // id-to: rizz__asset_mgr.metadata_buff
    int asset_mgr_id;        // index-to: rizz__asset_lib.asset_mgrs
    int ref_count;
    rizz_asset_obj obj;
    rizz_asset_obj dead_obj;
    uint32_t hash;
    uint32_t tags;
    rizz_asset_load_flags load_flags;
    rizz_asset_state state;
} rizz__asset;

// Resources are the actual files on the file-system
// Each resource has a metadata
// Likely to be populated by asset-db, but can grow on run-time
typedef struct {
    char path[RIZZ_MAX_PATH];         // path that is referenced in database and code
    char real_path[RIZZ_MAX_PATH];    // real path on disk, resolved by asset-db and variation
    uint32_t path_hash;               // hash of 'real_path'
    uint32_t metadata_id;             // id-to: rizz__asset_mgr.metadata_buff
    uint64_t last_modified;           // last-modified time-stamp
    int asset_mgr_id;                 // index-to: rizz__asset_lib.asset_mgrs
    bool used;
} rizz__asset_resource;

// Async loads are queued for each new async file loads
// To track which file points to which asset
typedef struct {
    uint32_t path_hash;    // hash of real_path
    rizz_asset asset;
} rizz__asset_async_load_req;

typedef enum {
    ASSET_JOB_STATE_SPAWN = 0,
    ASSET_JOB_STATE_LOAD_FAILED,
    ASSET_JOB_STATE_SUCCESS,
    _ASSET_JOB_STATE_ = INT32_MAX
} rizz__asset_job_state;

typedef sx_align_decl(8, struct) rizz__asset_async_job
{
    rizz_asset_load_data load_data;
    sx_mem_block* mem;
    rizz__asset_mgr* amgr;
    rizz_asset_load_params lparams;
    sx_job_t job;
    rizz__asset_job_state state;
    rizz_asset asset;
    struct rizz__asset_async_job* next;
    struct rizz__asset_async_job* prev;
}
rizz__asset_async_job;

typedef struct {
    rizz_asset* assets;    // sx_array
} rizz__asset_group;

typedef struct {
    const sx_alloc* alloc;    // allocator passed on init
    char asset_db_file[RIZZ_MAX_PATH];
    char variation[32];
    rizz_vfs_async_callbacks vfs_callbacks;    // vfs async-io
    rizz_vfs_async_callbacks prev_vfs_callbacks;
    rizz__asset_mgr* asset_mgrs;    // sx_array
    uint32_t* asset_name_hashes;    // sx_array (count = count(asset_mgrs))
    rizz__asset* assets;            // loaded assets
    sx_handle_pool* asset_handles;
    sx_hashtbl* asset_tbl;       // key: hash(path+params), value: handle (asset_handles)
    sx_hashtbl* resource_tbl;    // key  hash(path), value: index-to resources
    rizz__asset_resource*
        resources;    // resource database, this can be constructed when asset-db is loaded
    sx_hash_xxh32_t* hasher;
    rizz__asset_async_load_req* async_reqs;
    rizz__asset_async_job* async_job_list;
    rizz__asset_async_job* async_job_list_last;
    rizz__asset_group* groups;
    sx_handle_pool* group_handles;
    rizz_asset_group cur_group;
    sx_lock_t assets_lk;    // used for locking assets-array
} rizz__asset_lib;

static rizz__asset_lib g_asset;

#define rizz__asset_errmsg(_path, _realpath, _msgpref)                           \
    if (!sx_strequal(_path, _realpath))                                          \
        rizz_log_warn("%s asset '%s -> %s' failed", _msgpref, _path, _realpath); \
    else                                                                         \
        rizz_log_warn("%s asset '%s' failed", _msgpref, _path);

static rizz_asset rizz__asset_load_hashed(uint32_t name_hash, const char* path, const void* params,
                                          rizz_asset_load_flags flags, const sx_alloc* obj_alloc,
                                          uint32_t tags);

static inline int rizz__asset_find_async_req(const char* path)
{
    uint32_t path_hash = sx_hash_fnv32_str(path);
    for (int i = 0, c = sx_array_count(g_asset.async_reqs); i < c; i++) {
        rizz__asset_async_load_req* req = &g_asset.async_reqs[i];
        if (req->path_hash == path_hash)
            return i;
    }

    return -1;
}

static inline void rizz__asset_job_add_list(rizz__asset_async_job** pfirst,
                                            rizz__asset_async_job** plast,
                                            rizz__asset_async_job* node)
{
    // Add to the end of the list
    if (*plast) {
        (*plast)->next = node;
        node->prev = *plast;
    }
    *plast = node;
    if (*pfirst == NULL)
        *pfirst = node;
}

static inline void rizz__asset_job_remove_list(rizz__asset_async_job** pfirst,
                                               rizz__asset_async_job** plast,
                                               rizz__asset_async_job* node)
{
    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;
    if (*pfirst == node)
        *pfirst = node->next;
    if (*plast == node)
        *plast = node->prev;
    node->prev = node->next = NULL;
}

// async callbacks
static void rizz__asset_on_read_error(const char* path)
{
    int async_req_idx = rizz__asset_find_async_req(path);
    if (async_req_idx != -1) {
        rizz__asset_async_load_req* req = &g_asset.async_reqs[async_req_idx];
        rizz_asset asset = req->asset;
        rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
        sx_assert(a->resource_id);
        rizz__asset_resource* res = &g_asset.resources[rizz_to_index(a->resource_id)];
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];

        rizz__asset_errmsg(res->path, res->real_path, "opening");
        a->state = RIZZ_ASSET_STATE_FAILED;
        a->obj = amgr->failed_obj;

        sx_array_pop(g_asset.async_reqs, async_req_idx);
    }
}

static void rizz__asset_on_write_error(const char* path)
{
    sx_unused(path);
}

static void rizz__asset_load_job_cb(int start, int end, int thrd_index, void* user)
{
    sx_unused(start);
    sx_unused(end);
    sx_unused(thrd_index);
    rizz__asset_async_job* ajob = user;

    ajob->state = ajob->amgr->callbacks.on_load(&ajob->load_data, &ajob->lparams, ajob->mem)
                      ? ASSET_JOB_STATE_SUCCESS
                      : ASSET_JOB_STATE_LOAD_FAILED;
}

static void rizz__asset_on_read_complete(const char* path, sx_mem_block* mem)
{
    int async_req_idx = rizz__asset_find_async_req(path);
    if (async_req_idx == -1) {
        sx_mem_destroy_block(mem);
        return;
    }

    rizz__asset_async_load_req* req = &g_asset.async_reqs[async_req_idx];
    rizz_asset asset = req->asset;
    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    sx_assert(a->resource_id);
    rizz__asset_resource* res = &g_asset.resources[rizz_to_index(a->resource_id)];
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];

    const void* params_ptr = NULL;
    if (a->params_id)
        params_ptr = &amgr->params_buff[rizz_to_index(a->params_id)];

    rizz_asset_load_params aparams = (rizz_asset_load_params){ .path = res->path,
                                                               .params = params_ptr,
                                                               .alloc = a->alloc,
                                                               .tags = a->tags,
                                                               .flags = a->load_flags };

    // we don't have the metadata, allocate space and fetch it from the asset-loader
    if (!res->metadata_id) {
        res->metadata_id = rizz_to_id(sx_array_count(amgr->metadata_buff));
        amgr->callbacks.on_read_metadata(
            sx_array_add(g_asset.alloc, amgr->metadata_buff, amgr->metadata_size), &aparams, mem);
    } else if (aparams.flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) {
        amgr->callbacks.on_read_metadata(&amgr->metadata_buff[rizz_to_index(res->metadata_id)],
                                         &aparams, mem);
    }

    rizz_asset_load_data load_data =
        amgr->callbacks.on_prepare(&aparams, &amgr->metadata_buff[rizz_to_index(res->metadata_id)]);

    sx_array_pop(g_asset.async_reqs, async_req_idx);
    if (!load_data.obj.id) {
        rizz__asset_errmsg(res->path, res->real_path, "preparing");
        sx_mem_destroy_block(mem);
        return;
    }

    // dispatch job request for on_load
    // save a copy of path and params
    uint8_t* buff =
        sx_malloc(g_asset.alloc, sizeof(rizz__asset_async_job) + amgr->params_size + RIZZ_MAX_PATH);
    rizz__asset_async_job* ajob = (rizz__asset_async_job*)buff;
    buff += sizeof(rizz__asset_async_job);
    aparams.path = (const char*)buff;
    sx_memcpy(buff, res->path, RIZZ_MAX_PATH);
    buff += RIZZ_MAX_PATH;
    if (params_ptr) {
        sx_assert((uintptr_t)buff % 8 == 0);
        aparams.params = buff;
        sx_memcpy(buff, params_ptr, amgr->params_size);
    }

    *ajob = (rizz__asset_async_job){
        .load_data = load_data, .mem = mem, .amgr = amgr, .lparams = aparams, .asset = asset
    };

    ajob->job = the__core.job_dispatch(1, rizz__asset_load_job_cb, ajob, SX_JOB_PRIORITY_HIGH, 0);
    rizz__asset_job_add_list(&g_asset.async_job_list, &g_asset.async_job_list_last, ajob);
}

static void rizz__asset_on_write_complete(const char* path, int bytes_written, sx_mem_block* mem)
{
    sx_unused(path);
    sx_unused(bytes_written);
    sx_unused(mem);
}

static void rizz__asset_on_modified(const char* path)
{
    sx_assert(RIZZ_CONFIG_HOT_LOADING);

    char unixpath[RIZZ_MAX_PATH];
    sx_os_path_unixpath(unixpath, sizeof(unixpath), path);
    uint32_t path_hash = sx_hash_fnv32_str(unixpath);

    // search in resources
    for (int i = 0, c = sx_array_count(g_asset.resources); i < c; i++) {
        if (g_asset.resources[i].path_hash == path_hash) {
            g_asset.resources[i].last_modified = the__vfs.last_modified(path);

            uint32_t resource_id = rizz_to_id(i);
            // find any asset that have this resource and reload it
            for (int k = 0, kc = g_asset.asset_handles->count; k < kc; k++) {
                sx_handle_t handle = sx_handle_at(g_asset.asset_handles, k);
                rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
                if (a->resource_id == resource_id) {
                    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];
                    const void* params_ptr = NULL;
                    if (a->params_id)
                        params_ptr = &amgr->params_buff[rizz_to_index(a->params_id)];

                    // always reloads in `blocking` mode
                    rizz__asset_load_hashed(amgr->name_hash, unixpath, params_ptr,
                                            RIZZ_ASSET_LOAD_FLAG_RELOAD, a->alloc, a->tags);
                }
            }

            break;
        }
    }
}
// end: async callbacks

static inline uint32_t rizz__asset_hash(const char* path, const void* params, int params_size,
                                        const sx_alloc* alloc)
{
    sx_hash_xxh32_init(g_asset.hasher, 0);
    sx_hash_xxh32_update(g_asset.hasher, path, sx_strlen(path));
    if (params_size)
        sx_hash_xxh32_update(g_asset.hasher, params, params_size);
    sx_hash_xxh32_update(g_asset.hasher, &alloc, sizeof(alloc));
    return sx_hash_xxh32_digest(g_asset.hasher);
}

static inline int rizz__asset_find_asset_mgr(uint32_t name_hash)
{
    for (int i = 0, c = sx_array_count(g_asset.asset_name_hashes); i < c; i++) {
        if (g_asset.asset_name_hashes[i] == name_hash)
            return i;
    }
    return -1;
}

// asset database
static void rizz__asset_meta_read_item(const rizz_refl_field* f, sjson_node* jmeta)
{
    const rizz_refl_info* r = &f->info;
    void* value = f->value;
    rizz_refl_field fields[32];

    sjson_node* jfield = sjson_find_member(jmeta, r->name);
    if (jfield) {
        sx_assert(!(r->flags & RIZZ_REFL_FLAG_IS_PTR));

        if (r->flags & RIZZ_REFL_FLAG_IS_ENUM) {
            int eval = the__refl.get_enum(jfield->string_, 0);
            sx_memcpy(value, &eval, r->size);
        } else if (r->flags & RIZZ_REFL_FLAG_IS_STRUCT) {
            if (r->flags & RIZZ_REFL_FLAG_IS_ARRAY) {
                for (int i = 0; i < r->array_size; i++) {
                    int num_fields = the__refl.get_fields(
                        r->type, (uint8_t*)value + (size_t)i * (size_t)r->stride, fields,
                        sizeof(fields) / sizeof(rizz_refl_field));
                    for (int fi = 0; fi < num_fields; fi++) {
                        rizz__asset_meta_read_item(&fields[fi], sjson_find_element(jfield, i));
                    }
                }
            } else {
                int num_fields = the__refl.get_fields(r->type, value, fields,
                                                      sizeof(fields) / sizeof(rizz_refl_field));
                for (int fi = 0; fi < num_fields; fi++) {
                    rizz__asset_meta_read_item(&fields[fi], jfield);
                }
            }
        } else {
            if (sx_strequal(r->type, "int")) {
                int n = (int)jfield->number_;
                sx_memcpy(value, &n, r->size);
            } else if (sx_strequal(r->type, "float")) {
                float n = (float)jfield->number_;
                sx_memcpy(value, &n, r->size);
            } else if (sx_strequal(r->type, "bool")) {
                sx_memcpy(value, &jfield->bool_, r->size);
            } else if (the__refl.is_cstring(r)) {
                sx_memcpy(value, jfield->string_, r->size);
            }
        }
    }
}

static void rizz__asset_meta_write_item(const rizz_refl_field* field, sjson_context* jctx,
                                        sjson_node* jmeta)
{
    const rizz_refl_info* r = &field->info;
    void* value = field->value;
    rizz_refl_field fields[32];

    if (r->flags & RIZZ_REFL_FLAG_IS_ENUM) {
        int _e = *(int*)value;
        sjson_put_string(jctx, jmeta, r->name, the__refl.get_enum_name(r->type, _e));
    } else if (r->flags & RIZZ_REFL_FLAG_IS_STRUCT) {
        if (r->flags & RIZZ_REFL_FLAG_IS_ARRAY) {
            sjson_node* js = sjson_put_array(jctx, jmeta, r->name);
            for (int i = 0; i < r->array_size; i++) {
                sjson_node* jitem = sjson_mkobject(jctx);
                int num_fields =
                    the__refl.get_fields(r->type, (uint8_t*)value + (size_t)i * (size_t)r->stride,
                                         fields, sizeof(fields) / sizeof(rizz_refl_field));
                for (int fi = 0; fi < num_fields; fi++) {
                    rizz__asset_meta_write_item(&fields[fi], jctx, jitem);
                }
                sjson_append_element(js, jitem);
            }
        } else {
            sjson_node* js = sjson_put_obj(jctx, jmeta, r->name);
            int num_fields = the__refl.get_fields(r->type, value, fields,
                                                  sizeof(fields) / sizeof(rizz_refl_field));
            for (int fi = 0; fi < num_fields; fi++) {
                rizz__asset_meta_write_item(&fields[fi], jctx, js);
            }
        }
    } else {
        if (sx_strequal(r->type, "int")) {
            sjson_put_int(jctx, jmeta, r->name, *(int*)value);
        } else if (sx_strequal(r->type, "float")) {
            sjson_put_float(jctx, jmeta, r->name, *(float*)value);
        } else if (sx_strequal(r->type, "bool")) {
            sjson_put_bool(jctx, jmeta, r->name, *(bool*)value);
        } else if (the__refl.is_cstring(r)) {
            sjson_put_string(jctx, jmeta, r->name, (const char*)value);
        }
    }
}

static bool rizz__asset_load_meta_cache()
{
    sx_mem_block* db = the__vfs.read(g_asset.asset_db_file, RIZZ_VFS_FLAG_TEXT_FILE, g_asset.alloc);
    if (!db) {
        return false;
    }

    sjson_context* jctx = sjson_create_context(0, 0, (void*)g_asset.alloc);
    if (!jctx)
        return false;
    sjson_node* jroot = sjson_decode(jctx, (const char*)db->data);
    if (!jroot) {
        rizz_log_warn("loading asset database failed: %s", g_asset.asset_db_file);
        return false;
    }

    sjson_node* jitem;
    rizz_refl_field meta_fields[32];
    sjson_foreach(jitem, jroot)
    {
        const char* name = sjson_get_string(jitem, "name", "");
        if (name[0]) {
            rizz__asset_resource rs = { .path = { 0 } };

            // check last-modified date of the actual file
            // NOTE: on mobile, we still don't have any solution
            //       so we just rely on tools to provide the latest database file
#if !SX_PLATFORM_ANDROID && !SX_PLATFORM_IOS
            uint64_t cur_lastmod = the__vfs.last_modified(name);
            if (cur_lastmod == 0)
                continue;    // file isn't valid are does not exist
            // if last-modified date is changed, remove it from database
            uint64_t lastmod = (uint64_t)sjson_get_double(jitem, "last_modified", 0);
            if (lastmod == 0 || cur_lastmod != lastmod)
                continue;
            rs.last_modified = cur_lastmod;
#endif    // !SX_PLATFORM_ANDROID && !SX_PLATFORM_IOS

            sx_strcpy(rs.path, sizeof(rs.path), name);
            sx_strcpy(rs.real_path, sizeof(rs.real_path), sjson_get_string(jitem, "path", name));
            rs.path_hash = sx_hash_fnv32_str(rs.real_path);

            // metadata
            const char* type_name = sjson_get_string(jitem, "type_name", "");
            sjson_node* jmeta = sjson_find_member(jitem, "metadata");
            int amgr_idx = rizz__asset_find_asset_mgr(sx_hash_fnv32_str(type_name));
            if (type_name[0] && amgr_idx != -1 && jmeta) {
                rizz__asset_mgr* amgr = &g_asset.asset_mgrs[amgr_idx];

                rs.metadata_id = rizz_to_id(sx_array_count(amgr->metadata_buff));
                rs.asset_mgr_id = amgr_idx;

                uint8_t* meta_buff =
                    sx_array_add(g_asset.alloc, amgr->metadata_buff, amgr->metadata_size);
                sx_memset(meta_buff, 0x0, amgr->metadata_size);

                int num_fields =
                    the__refl.get_fields(amgr->metadata_type_name, meta_buff, meta_fields,
                                         sizeof(meta_fields) / sizeof(rizz_refl_field));
                for (int fi = 0; fi < num_fields; fi++) {
                    rizz__asset_meta_read_item(&meta_fields[fi], jmeta);
                }

                int res_idx = sx_array_count(g_asset.resources);
                sx_array_push(g_asset.alloc, g_asset.resources, rs);
                sx_hashtbl_add_and_grow(g_asset.resource_tbl, rs.path_hash, res_idx, g_asset.alloc);
            }    //
        }
    }

    sjson_destroy_context(jctx);
    sx_mem_destroy_block(db);
    return true;
}

bool rizz__asset_save_meta_cache()
{
    sjson_context* jctx = sjson_create_context(0, 0, (void*)g_asset.alloc);
    if (!jctx)
        return false;

    rizz_refl_field meta_fields[32];
    sjson_node* jroot = sjson_mkarray(jctx);

    for (int i = 0, c = sx_array_count(g_asset.resources); i < c; i++) {
        const rizz__asset_resource* rs = &g_asset.resources[i];
        sjson_node* jitem = sjson_mkobject(jctx);
        sjson_put_string(jctx, jitem, "name", rs->path);
        sjson_put_double(jctx, jitem, "last_modified", (double)the__vfs.last_modified(rs->path));
        if (!sx_strequal(rs->path, rs->real_path))
            sjson_put_string(jctx, jitem, "path", rs->real_path);

        sx_assert(rs->asset_mgr_id >= 0 && rs->asset_mgr_id < sx_array_count(g_asset.asset_mgrs));
        const rizz__asset_mgr* amgr = &g_asset.asset_mgrs[rs->asset_mgr_id];
        sjson_put_string(jctx, jitem, "type_name", amgr->name);

        // metadata
        if (rs->metadata_id && amgr->metadata_size) {
            sjson_node* jmeta = sjson_mkobject(jctx);
            uint8_t* meta_buff = &amgr->metadata_buff[rizz_to_index(rs->metadata_id)];

            int num_fields = the__refl.get_fields(amgr->metadata_type_name, meta_buff, meta_fields,
                                                  sizeof(meta_fields) / sizeof(rizz_refl_field));
            for (int fi = 0; fi < num_fields; fi++) {
                rizz__asset_meta_write_item(&meta_fields[fi], jctx, jmeta);
            }
            sjson_append_member(jctx, jitem, "metadata", jmeta);
        }

        sjson_append_element(jroot, jitem);
    }

    char* jstr = sjson_stringify(jctx, jroot, "  ");
    if (!jstr)
        return false;
    sx_mem_block db_mem;
    sx_mem_init_block_ptr(&db_mem, jstr, sx_strlen(jstr));
    the__vfs.write(g_asset.asset_db_file, &db_mem, 0);

    sjson_free_string(jctx, jstr);
    sjson_destroy_context(jctx);

    return true;
}

bool rizz__asset_dump_unused(const char* filepath)
{
    sjson_context* jctx = sjson_create_context(0, 0, (void*)g_asset.alloc);
    if (!jctx)
        return false;

    sjson_node* jroot = sjson_mkarray(jctx);
    for (int i = 0, c = sx_array_count(g_asset.resources); i < c; i++) {
        if (!g_asset.resources[i].used) {
            sjson_append_element(jroot, sjson_mkstring(jctx, g_asset.resources[i].path));
        }
    }

    char* jstr = sjson_stringify(jctx, jroot, "  ");
    if (!jstr)
        return false;
    sx_file_writer writer;
    if (sx_file_open_writer(&writer, filepath, 0)) {
        sx_file_write_text(&writer, jstr);
        sx_file_close_writer(&writer);
    }
    sjson_free_string(jctx, jstr);
    sjson_destroy_context(jctx);
    return true;
}
// end: asset database


static rizz_asset rizz__asset_create_new(const char* path, const void* params, rizz_asset_obj obj,
                                         uint32_t name_hash, const sx_alloc* obj_alloc,
                                         rizz_asset_load_flags flags, uint32_t tags)
{
    // find asset manager
    int amgr_id = rizz__asset_find_asset_mgr(name_hash);
    sx_assert(amgr_id != -1 && "asset type is not registered");
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[amgr_id];

    // check resources, if doesn't exist, add new resource
    uint32_t path_hash = sx_hash_fnv32_str(path);
    int res_idx = sx_hashtbl_find_get(g_asset.resource_tbl, path_hash, -1);
    if (res_idx == -1) {
        rizz__asset_resource res = { .used = true };
        sx_strcpy(res.path, sizeof(res.path), path);
        sx_strcpy(res.real_path, sizeof(res.real_path), path);
        res.path_hash = path_hash;
        res.asset_mgr_id = amgr_id;
#if !SX_PLATFORM_ANDROID && !SX_PLATFORM_IOS
        res.last_modified = the__vfs.last_modified(res.real_path);
#endif
        res_idx = sx_array_count(g_asset.resources);
        sx_array_push(g_asset.alloc, g_asset.resources, res);
        sx_hashtbl_add_and_grow(g_asset.resource_tbl, path_hash, res_idx, g_asset.alloc);
    } else {
        g_asset.resources[res_idx].used = true;
    }

    // new param
    int params_size = amgr->params_size;
    uint32_t params_id = 0;
    if (params_size > 0) {
        params_id =
            rizz_to_id(sx_array_count(amgr->params_buff));    // actually, it stores the byte-offset
        sx_memcpy(sx_array_add(g_asset.alloc, amgr->params_buff, params_size), params, params_size);
    }

    sx_handle_t handle = sx_handle_new_and_grow(g_asset.asset_handles, g_asset.alloc);
    sx_assert(handle);

    // add asset to database
    rizz__asset asset =
        (rizz__asset){ .alloc = obj_alloc,
                       .handle = handle,
                       .params_id = params_id,
                       .resource_id = rizz_to_id(res_idx),
                       .asset_mgr_id = amgr_id,
                       .ref_count = 1,
                       .obj = obj,
                       .hash = rizz__asset_hash(path, params, params_size, obj_alloc),
                       .tags = tags,
                       .load_flags = flags,
                       .state = RIZZ_ASSET_STATE_ZOMBIE };

    // have to protected this block of code with a lock
    // because we may regrow the asset-array
    // asset-array may be accessed in worker-threads with `obj_threadsafe()` function
    sx_lock(&g_asset.assets_lk, 1);
    sx_array_push_byindex(g_asset.alloc, g_asset.assets, asset, sx_handle_index(handle));
    sx_unlock(&g_asset.assets_lk);

    sx_hashtbl_add_and_grow(g_asset.asset_tbl, asset.hash, handle, g_asset.alloc);

    return (rizz_asset){ handle };
}

static void rizz__asset_destroy_delete(rizz_asset a, rizz__asset_mgr* amgr)
{
    sx_assert(a.id);
    rizz__asset* asset = &g_asset.assets[sx_handle_index(a.id)];

    // delete param
    if (asset->params_id) {
        int params_size = amgr->params_size;
        int offset = rizz_to_index(asset->params_id);
        int last_offset = sx_array_count(amgr->params_buff) - params_size;
        if (offset != last_offset) {
            uint32_t last_id = rizz_to_id(last_offset);

            // find parameter and change the last_id
            for (int i = 0, c = g_asset.asset_handles->count; i < c; i++) {
                sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
                rizz__asset* aa = &g_asset.assets[sx_handle_index(handle)];
                if (aa->params_id == last_id) {
                    aa->params_id = asset->params_id;
                    break;
                }
            }

            sx_assert(amgr->params_buff);
            sx_memcpy(&amgr->params_buff[offset], &amgr->params_buff[last_offset], params_size);
        }
        sx_assert(amgr->params_buff);
        sx_array_pop_lastn(amgr->params_buff, params_size);
    }

    // unload embeded object
    if (asset->obj.id != amgr->async_obj.id && asset->obj.id != amgr->failed_obj.id) {
        amgr->callbacks.on_release(asset->obj, asset->alloc);
        asset->obj = amgr->failed_obj;
    }

    sx_hashtbl_remove_if_found(g_asset.asset_tbl, asset->hash);
    sx_handle_del(g_asset.asset_handles, a.id);
}

static rizz_asset rizz__asset_add(const char* path, const void* params, rizz_asset_obj obj,
                                  uint32_t name_hash, const sx_alloc* obj_alloc,
                                  rizz_asset_load_flags flags, uint32_t tags,
                                  rizz_asset override_asset)
{
    rizz_asset asset = override_asset;
    if (asset.id) {
        // this block of code actually happens on RELOAD process
        sx_assert(flags & RIZZ_ASSET_LOAD_FLAG_RELOAD);
        rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];

        // keep the current object so it can be released later
        sx_assert(a->handle == asset.id);
        if (a->state == RIZZ_ASSET_STATE_OK)
            a->dead_obj = a->obj;
        a->obj = obj;
        sx_assert(a->alloc == obj_alloc && "allocator must not change in reload");
        if (amgr->params_size > 0) {
            sx_assert(a->params_id);
            sx_memcpy(&amgr->params_buff[rizz_to_index(a->params_id)], params, amgr->params_size);
        }
    } else {
        asset = rizz__asset_create_new(path, params, obj, name_hash, obj_alloc, flags, tags);
    }

    return asset;
}

static rizz_asset rizz__asset_load_hashed(uint32_t name_hash, const char* path, const void* params,
                                          rizz_asset_load_flags flags, const sx_alloc* obj_alloc,
                                          uint32_t tags)
{
    if (!path[0]) {
        rizz_log_warn("empty path for asset");
        return (rizz_asset){ 0 };
    }

    if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD)
        flags |= RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD;

    int amgr_id = rizz__asset_find_asset_mgr(name_hash);
    sx_assert(amgr_id != -1 && "asset type is not registered");
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[amgr_id];
    flags |= amgr->forced_flags;

    // find if asset is already loaded
    rizz_asset asset = (rizz_asset){ sx_hashtbl_find_get(
        g_asset.asset_tbl, rizz__asset_hash(path, params, amgr->params_size, obj_alloc), 0) };
    if (asset.id && !(flags & RIZZ_ASSET_LOAD_FLAG_RELOAD)) {
        ++g_asset.assets[sx_handle_index(asset.id)].ref_count;
    } else {
        // find resource and resolve the real file path
        int res_idx = sx_hashtbl_find_get(g_asset.resource_tbl, sx_hash_fnv32_str(path), -1);
        const char* real_path = path;
        rizz__asset_resource* res = NULL;
        if (res_idx != -1) {
            res = &g_asset.resources[res_idx];
            real_path = res->real_path;
        }

        if (!(flags & RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD)) {
            // Async load
            asset = rizz__asset_create_new(path, params, amgr->async_obj, name_hash, obj_alloc,
                                           flags, tags);
            rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
            a->state = RIZZ_ASSET_STATE_LOADING;

            rizz__asset_async_load_req req =
                (rizz__asset_async_load_req){ .path_hash = sx_hash_fnv32_str(real_path),
                                              .asset = asset };
            sx_array_push(g_asset.alloc, g_asset.async_reqs, req);

            the__vfs.read_async(
                real_path,
                (flags & RIZZ_ASSET_LOAD_FLAG_ABSOLUTE_PATH) ? RIZZ_VFS_FLAG_ABSOLUTE_PATH : 0,
                the__core.alloc(RIZZ_MEMID_CORE));
        } else {
            // Blocking load (+ reloads)
            asset =
                rizz__asset_add(path, params, amgr->failed_obj, name_hash, obj_alloc, flags, tags,
                                (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) ? asset : (rizz_asset){ 0 });

            sx_mem_block* mem = the__vfs.read(
                real_path,
                (flags & RIZZ_ASSET_LOAD_FLAG_ABSOLUTE_PATH) ? RIZZ_VFS_FLAG_ABSOLUTE_PATH : 0,
                the__core.alloc(RIZZ_MEMID_CORE));

            if (!mem) {
                rizz__asset_errmsg(path, real_path, "opening");
                return asset;
            }

            rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];

            if (!res) {
                sx_assert(a->resource_id);
                res = &g_asset.resources[rizz_to_index(a->resource_id)];
            }

            rizz_asset_load_params aparams = (rizz_asset_load_params){
                .path = path, .params = params, .alloc = obj_alloc, .tags = tags, .flags = flags
            };

            // we don't have the metadata, allocate space and fetch it from the asset-loader
            if (!res->metadata_id) {
                res->metadata_id = rizz_to_id(sx_array_count(amgr->metadata_buff));
                amgr->callbacks.on_read_metadata(
                    sx_array_add(g_asset.alloc, amgr->metadata_buff, amgr->metadata_size), &aparams,
                    mem);
            } else if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) {
                amgr->callbacks.on_read_metadata(
                    &amgr->metadata_buff[rizz_to_index(res->metadata_id)], &aparams, mem);
            }

            bool success = false;
            rizz_asset_load_data load_data = amgr->callbacks.on_prepare(
                &aparams, &amgr->metadata_buff[rizz_to_index(res->metadata_id)]);

            // revive pointer to asset, because during `on_prepare` some resource dependencies
            // may be loaded and `assets` array may be resized
            a = &g_asset.assets[sx_handle_index(asset.id)];
            if (load_data.obj.id) {
                if (amgr->callbacks.on_load(&load_data, &aparams, mem)) {
                    amgr->callbacks.on_finalize(&load_data, &aparams, mem);
                    success = true;
                }
            }

            sx_mem_destroy_block(mem);
            if (success) {
                a->state = RIZZ_ASSET_STATE_OK;
                a->obj = load_data.obj;
            } else {
                if (load_data.obj.id)
                    amgr->callbacks.on_release(load_data.obj, a->alloc);
                rizz__asset_errmsg(path, real_path, "loading");
                if (a->obj.id && !a->dead_obj.id) {
                    a->state = RIZZ_ASSET_STATE_FAILED;
                } else {
                    a->obj = a->dead_obj;    // rollback
                    a->dead_obj = (rizz_asset_obj){ .id = 0 };
                }
            }

            // do we have extra work in reload?
            if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) {
                amgr->callbacks.on_reload(asset, a->dead_obj, obj_alloc);
                if (a->dead_obj.id) {
                    amgr->callbacks.on_release(a->dead_obj, obj_alloc);
                    a->dead_obj = (rizz_asset_obj){ .id = 0 };
                }
            }
        }
    }

    return asset;
}

bool rizz__asset_init(const sx_alloc* alloc, const char* dbfile, const char* variation)
{
    sx_assert(dbfile);

    g_asset.alloc = alloc;
    sx_strcpy(g_asset.variation, sizeof(g_asset.variation), variation);
    sx_strcpy(g_asset.asset_db_file, sizeof(g_asset.asset_db_file), dbfile);

    g_asset.vfs_callbacks =
        (rizz_vfs_async_callbacks){ .on_read_error = rizz__asset_on_read_error,
                                    .on_write_error = rizz__asset_on_write_error,
                                    .on_read_complete = rizz__asset_on_read_complete,
                                    .on_write_complete = rizz__asset_on_write_complete,
                                    .on_modified = rizz__asset_on_modified };
    g_asset.prev_vfs_callbacks = the__vfs.set_async_callbacks(&g_asset.vfs_callbacks);

    g_asset.asset_tbl = sx_hashtbl_create(alloc, RIZZ_CONFIG_ASSET_POOL_SIZE);
    g_asset.resource_tbl = sx_hashtbl_create(alloc, RIZZ_CONFIG_ASSET_POOL_SIZE);
    sx_assert(g_asset.asset_tbl);

    g_asset.asset_handles = sx_handle_create_pool(alloc, RIZZ_CONFIG_ASSET_POOL_SIZE);
    sx_assert(g_asset.asset_handles);

    g_asset.group_handles = sx_handle_create_pool(alloc, 32);
    sx_assert(g_asset.group_handles);

    g_asset.hasher = sx_hash_create_xxh32(alloc);
    sx_assert(g_asset.hasher);

    return true;
}

void rizz__asset_release()
{
    if (!g_asset.alloc)
        return;

    const sx_alloc* alloc = g_asset.alloc;

    if (g_asset.asset_handles) {
        for (int i = 0; i < g_asset.asset_handles->count; i++) {
            sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
            rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
            if (a->state == RIZZ_ASSET_STATE_OK) {
                sx_assert(a->resource_id);
                rizz_log_warn("un-released asset: %s",
                              g_asset.resources[rizz_to_index(a->resource_id)].path);
                if (a->obj.id) {
                    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];
                    if (!amgr->unreg)
                        amgr->callbacks.on_release(a->obj, a->alloc);
                }
            }
        }
    }

    for (int i = 0; i < sx_array_count(g_asset.asset_mgrs); i++) {
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[i];
        sx_array_free(alloc, amgr->params_buff);
        sx_array_free(alloc, amgr->metadata_buff);
    }

    for (int i = 0; i < sx_array_count(g_asset.groups); i++) {
        sx_array_free(alloc, g_asset.groups[i].assets);
    }

    sx_array_free(alloc, g_asset.asset_mgrs);
    sx_array_free(alloc, g_asset.assets);
    sx_array_free(alloc, g_asset.asset_name_hashes);
    sx_array_free(alloc, g_asset.resources);
    sx_array_free(alloc, g_asset.groups);
    sx_array_free(alloc, g_asset.async_reqs);

    if (g_asset.asset_handles)
        sx_handle_destroy_pool(g_asset.asset_handles, alloc);
    if (g_asset.asset_tbl)
        sx_hashtbl_destroy(g_asset.asset_tbl, alloc);
    if (g_asset.resource_tbl)
        sx_hashtbl_destroy(g_asset.resource_tbl, alloc);
    if (g_asset.group_handles)
        sx_handle_destroy_pool(g_asset.group_handles, alloc);

    if (g_asset.hasher)
        sx_hash_destroy_xxh32(g_asset.hasher, g_asset.alloc);

    rizz__asset_async_job* ajob = g_asset.async_job_list;
    while (ajob) {
        rizz__asset_async_job* next = ajob->next;
        if (ajob->mem)
            sx_mem_destroy_block(ajob->mem);
        sx_free(g_asset.alloc, ajob);
        ajob = next;
    }
    g_asset.async_job_list = g_asset.async_job_list_last = NULL;

    the__vfs.set_async_callbacks(&g_asset.prev_vfs_callbacks);
}

void rizz__asset_update()
{
    rizz__asset_async_job* ajob = g_asset.async_job_list;
    while (ajob) {
        rizz__asset_async_job* next = ajob->next;
        if (the__core.job_test_and_del(ajob->job)) {
            rizz__asset* a = &g_asset.assets[sx_handle_index(ajob->asset.id)];
            sx_assert(a->resource_id);
            rizz__asset_resource* res = &g_asset.resources[rizz_to_index(a->resource_id)];

            switch (ajob->state) {
            case ASSET_JOB_STATE_SUCCESS:
                ajob->amgr->callbacks.on_finalize(&ajob->load_data, &ajob->lparams, ajob->mem);
                a->obj = ajob->load_data.obj;
                a->state = RIZZ_ASSET_STATE_OK;
                break;

            case ASSET_JOB_STATE_LOAD_FAILED:
                rizz__asset_errmsg(res->path, res->real_path, "loading");
                a->obj = ajob->amgr->failed_obj;
                a->state = RIZZ_ASSET_STATE_FAILED;

                if (ajob->load_data.obj.id)
                    ajob->amgr->callbacks.on_release(ajob->load_data.obj, ajob->lparams.alloc);

                break;

            default:
                sx_assert(0 && "finished job should not be any other state");
                break;
            }

            sx_assert(!(ajob->lparams.flags & RIZZ_ASSET_LOAD_FLAG_RELOAD));

            sx_mem_destroy_block(ajob->mem);

            rizz__asset_job_remove_list(&g_asset.async_job_list, &g_asset.async_job_list_last,
                                        ajob);
            sx_free(g_asset.alloc, ajob);
        }    // if (job-is-done)

        ajob = next;
    }
}

static rizz_asset rizz__asset_load(const char* name, const char* path, const void* params,
                                   rizz_asset_load_flags flags, const sx_alloc* alloc,
                                   uint32_t tags)
{
    rizz_asset asset =
        rizz__asset_load_hashed(sx_hash_fnv32_str(name), path, params, flags, alloc, tags);
    if (asset.id && g_asset.cur_group.id) {
        rizz__asset_group* g = &g_asset.groups[sx_handle_index(g_asset.cur_group.id)];
        sx_array_push(g_asset.alloc, g->assets, asset);
    }
    return asset;
}

static rizz_asset rizz__asset_load_from_mem(const char* name, const char* path_alias,
                                            sx_mem_block* mem, const void* params,
                                            rizz_asset_load_flags flags, const sx_alloc* alloc,
                                            uint32_t tags)
{
    if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD)
        flags |= RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD;

    uint32_t name_hash = sx_hash_fnv32_str(name);

    int amgr_id = rizz__asset_find_asset_mgr(name_hash);
    sx_assert(amgr_id != -1 && "asset type is not registered");
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[amgr_id];
    flags |= amgr->forced_flags;

    // find if asset is already loaded
    rizz_asset asset = (rizz_asset){ sx_hashtbl_find_get(
        g_asset.asset_tbl, rizz__asset_hash(path_alias, params, amgr->params_size, alloc), 0) };

    if (asset.id && !(flags & RIZZ_ASSET_LOAD_FLAG_RELOAD)) {
        ++g_asset.assets[sx_handle_index(asset.id)].ref_count;
    } else {
        // find resource and resolve the real file path
        int res_idx = sx_hashtbl_find_get(g_asset.resource_tbl, sx_hash_fnv32_str(path_alias), -1);
        const char* real_path = path_alias;
        rizz__asset_resource* res = NULL;
        if (res_idx != -1) {
            res = &g_asset.resources[res_idx];
            real_path = res->real_path;
        }

        if (!(flags & RIZZ_ASSET_LOAD_FLAG_WAIT_ON_LOAD)) {
            // Async load
            asset = rizz__asset_create_new(path_alias, params, amgr->async_obj, name_hash, alloc,
                                           flags, tags);
            rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
            a->state = RIZZ_ASSET_STATE_LOADING;

            rizz__asset_async_load_req req =
                (rizz__asset_async_load_req){ .path_hash = sx_hash_fnv32_str(real_path),
                                              .asset = asset };
            sx_array_push(g_asset.alloc, g_asset.async_reqs, req);

            rizz__asset_on_read_complete(real_path, mem);
        } else {
            // Blocking load (+ reloads)
            // Add asset entry
            asset =
                rizz__asset_add(path_alias, params, amgr->failed_obj, name_hash, alloc, flags, tags,
                                (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) ? asset : (rizz_asset){ 0 });
            rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];

            if (!res) {
                sx_assert(a->resource_id);
                res = &g_asset.resources[rizz_to_index(a->resource_id)];
            }

            // we don't have the metadata, allocate space and fetch it from the asset-loader
            if (!res->metadata_id) {
                res->metadata_id = rizz_to_id(sx_array_count(amgr->metadata_buff));
                amgr->callbacks.on_read_metadata(
                    sx_array_add(g_asset.alloc, amgr->metadata_buff, amgr->metadata_size), params,
                    mem);
            } else if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) {
                amgr->callbacks.on_read_metadata(
                    &amgr->metadata_buff[rizz_to_index(res->metadata_id)], params, mem);
            }

            rizz_asset_load_params aparams = (rizz_asset_load_params){
                .path = path_alias, .params = params, .alloc = alloc, .tags = tags, .flags = flags
            };

            bool success = false;
            rizz_asset_load_data load_data = amgr->callbacks.on_prepare(
                &aparams, &amgr->metadata_buff[rizz_to_index(res->metadata_id)]);
            if (load_data.obj.id) {
                if (amgr->callbacks.on_load(&load_data, &aparams, mem)) {
                    amgr->callbacks.on_finalize(&load_data, &aparams, mem);
                    success = true;
                }
            }

            sx_mem_destroy_block(mem);
            if (success) {
                a->state = RIZZ_ASSET_STATE_OK;
                a->obj = load_data.obj;
            } else {
                if (load_data.obj.id)
                    amgr->callbacks.on_release(load_data.obj, a->alloc);
                rizz__asset_errmsg(path_alias, real_path, "loading");
                if (a->obj.id && !a->dead_obj.id) {
                    a->state = RIZZ_ASSET_STATE_FAILED;
                } else {
                    a->obj = a->dead_obj;    // rollback
                    a->dead_obj = (rizz_asset_obj){ .id = 0 };
                }
            }

            // do we have extra work in reload?
            if (flags & RIZZ_ASSET_LOAD_FLAG_RELOAD) {
                amgr->callbacks.on_reload(asset, a->dead_obj, alloc);
                if (a->dead_obj.id) {
                    amgr->callbacks.on_release(a->dead_obj, a->alloc);
                    a->dead_obj = (rizz_asset_obj){ .id = 0 };
                }
            }
        }
    }
    return asset;
}

static void rizz__asset_unload(rizz_asset asset)
{
    sx_assert(asset.id);
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    sx_assert(a->ref_count > 0);

    if (--a->ref_count == 0) {
        // remove from async requests
        for (int i = 0, c = sx_array_count(g_asset.async_reqs); i < c; i++) {
            rizz__asset_async_load_req* req = &g_asset.async_reqs[i];
            if (req->asset.id == asset.id) {
                sx_array_pop(g_asset.async_reqs, i);
                break;
            }
        }

        // remove from async jobs
        for (rizz__asset_async_job* ajob = g_asset.async_job_list; ajob; ajob = ajob->next) {
            if (ajob->asset.id == asset.id) {
                rizz__asset_job_remove_list(&g_asset.async_job_list, &g_asset.async_job_list_last,
                                            ajob);
                sx_free(g_asset.alloc, ajob);
                break;
            }
        }

        // release internal object
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];
        rizz__asset_destroy_delete(asset, amgr);
    }
}

static void rizz__register_asset_type(const char* name, rizz_asset_callbacks callbacks,
                                      const char* params_type_name, int params_size,
                                      const char* metadata_type_name, int metadata_size,
                                      rizz_asset_obj failed_obj, rizz_asset_obj async_obj,
                                      rizz_asset_load_flags forced_flags)
{
    uint32_t name_hash = sx_hash_fnv32_str(name);
    for (int i = 0; i < sx_array_count(g_asset.asset_name_hashes); i++) {
        if (name_hash == g_asset.asset_name_hashes[i]) {
            sx_assert(0 && "asset-mgr already registered");
            return;
        }
    }

    rizz__asset_mgr amgr = { .callbacks = callbacks,
                             .name_hash = name_hash,
                             .params_size = params_size,
                             .metadata_size = metadata_size,
                             .failed_obj = failed_obj,
                             .async_obj = async_obj,
                             .forced_flags = forced_flags };
    sx_strcpy(amgr.name, sizeof(amgr.name), name);
    sx_strcpy(amgr.params_type_name, sizeof(amgr.params_type_name),
              params_type_name ? params_type_name : "");
    sx_strcpy(amgr.metadata_type_name, sizeof(amgr.metadata_type_name),
              metadata_type_name ? metadata_type_name : "");

    sx_array_push(g_asset.alloc, g_asset.asset_mgrs, amgr);
    sx_array_push(g_asset.alloc, g_asset.asset_name_hashes, name_hash);
}

static void rizz__unregister_asset_type(const char* name)
{
    int amgr_id = rizz__asset_find_asset_mgr(sx_hash_fnv32_str(name));
    sx_assert(amgr_id != -1 && "asset type is not registered");
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[amgr_id];
    amgr->unreg = true;
}

static rizz_asset_state rizz__asset_state(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return a->state;
}

static const char* rizz__asset_path(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    sx_assert(a->resource_id);
    return g_asset.resources[rizz_to_index(a->resource_id)].path;
}

static const char* rizz__asset_typename(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return g_asset.asset_mgrs[a->asset_mgr_id].name;
}

static const void* rizz__asset_params(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];

    return a->params_id ? &amgr->params_buff[rizz_to_index(a->params_id)] : NULL;
}

static uint32_t rizz__asset_tags(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return a->tags;
}

static rizz_asset_obj rizz__asset_obj(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return a->obj;
}

static rizz_asset_obj rizz__asset_obj_threadsafe(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    sx_lock(&g_asset.assets_lk, 1);
    rizz_asset_obj obj = g_asset.assets[sx_handle_index(asset.id)].obj;
    sx_unlock(&g_asset.assets_lk);
    return obj;
}

static int rizz__asset_ref_add(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return ++a->ref_count;
}

static int rizz__asset_ref_count(rizz_asset asset)
{
    sx_assert_rel(sx_handle_valid(g_asset.asset_handles, asset.id));

    rizz__asset* a = &g_asset.assets[sx_handle_index(asset.id)];
    return a->ref_count;
}

static void rizz__asset_reload_by_type(const char* name)
{
    uint32_t name_hash = sx_hash_fnv32_str(name);
    int asset_mgr_id = -1;
    for (int i = 0, c = sx_array_count(g_asset.asset_mgrs); i < c; i++) {
        if (g_asset.asset_mgrs[i].name_hash == name_hash) {
            asset_mgr_id = i;
            break;
        }
    }

    if (asset_mgr_id != -1) {
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[asset_mgr_id];
        for (int i = 0, c = g_asset.asset_handles->count; i < c; i++) {
            sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
            rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
            if (a->asset_mgr_id == asset_mgr_id) {
                sx_assert(a->resource_id);
                rizz__asset_load_hashed(
                    name_hash, g_asset.resources[rizz_to_index(a->resource_id)].path,
                    a->params_id ? &amgr->params_buff[rizz_to_index(a->params_id)] : NULL,
                    a->load_flags, a->alloc, a->tags);
            }
        }
    }
}

static int rizz__asset_gather_by_type(const char* name, rizz_asset* out_handles, int max_handles)
{
    uint32_t name_hash = sx_hash_fnv32_str(name);
    int asset_mgr_id = -1;
    for (int i = 0, c = sx_array_count(g_asset.asset_mgrs); i < c; i++) {
        if (g_asset.asset_mgrs[i].name_hash == name_hash) {
            asset_mgr_id = i;
            break;
        }
    }

    int count = 0;
    if (asset_mgr_id != -1) {
        for (int i = 0, c = g_asset.asset_handles->count; i < c && count < max_handles; i++) {
            sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
            rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
            if (a->asset_mgr_id == asset_mgr_id) {
                if (out_handles) {
                    out_handles[count] = (rizz_asset){ handle };
                }
                count++;
            }
        }
    }

    return count;
}

static void rizz__asset_unload_by_type(const char* name)
{
    uint32_t name_hash = sx_hash_fnv32_str(name);
    int asset_mgr_id = -1;
    for (int i = 0, c = sx_array_count(g_asset.asset_mgrs); i < c; i++) {
        if (g_asset.asset_mgrs[i].name_hash == name_hash) {
            asset_mgr_id = i;
            break;
        }
    }

    if (asset_mgr_id != -1) {
        rizz__asset_mgr* amgr = &g_asset.asset_mgrs[asset_mgr_id];
        for (int i = 0, c = g_asset.asset_handles->count; i < c; i++) {
            sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
            rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
            if (a->asset_mgr_id == asset_mgr_id && a->obj.id && a->state == RIZZ_ASSET_STATE_OK) {
                if (a->obj.id != amgr->async_obj.id && a->obj.id != amgr->failed_obj.id) {
                    amgr->callbacks.on_release(a->obj, a->alloc);
                    a->obj = amgr->async_obj;
                    a->state = RIZZ_ASSET_STATE_ZOMBIE;
                }
            }
        }
    }
}

static void rizz__asset_reload_by_tags(uint32_t tags)
{
    for (int i = 0, c = g_asset.asset_handles->count; i < c; i++) {
        sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
        rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
        if (a->tags & tags) {
            sx_assert(a->resource_id);
            rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];
            rizz__asset_load_hashed(
                amgr->name_hash, g_asset.resources[rizz_to_index(a->resource_id)].path,
                a->params_id ? &amgr->params_buff[rizz_to_index(a->params_id)] : NULL,
                a->load_flags, a->alloc, a->tags);
        }
    }
}

static int rizz__asset_gather_by_tags(uint32_t tags, rizz_asset* out_handles, int max_handles)
{
    int count = 0;
    for (int i = 0, c = g_asset.asset_handles->count; i < c && count < max_handles; i++) {
        sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
        rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
        if (a->tags & tags) {
            if (out_handles) {
                out_handles[count] = (rizz_asset){ handle };
            }
            count++;
        }
    }
    return count;
}

static void rizz__asset_unload_by_tags(uint32_t tags)
{
    for (int i = 0, c = g_asset.asset_handles->count; i < c; i++) {
        sx_handle_t handle = sx_handle_at(g_asset.asset_handles, i);
        rizz__asset* a = &g_asset.assets[sx_handle_index(handle)];
        if ((a->tags & tags) && a->obj.id && a->state == RIZZ_ASSET_STATE_OK) {
            sx_assert(a->resource_id);
            rizz__asset_mgr* amgr = &g_asset.asset_mgrs[a->asset_mgr_id];
            if (a->obj.id != amgr->async_obj.id && a->obj.id != amgr->failed_obj.id) {
                amgr->callbacks.on_release(a->obj, a->alloc);
                a->obj = amgr->async_obj;
                a->state = RIZZ_ASSET_STATE_ZOMBIE;
            }
        }
    }
}

static rizz_asset_group rizz__asset_group_begin(rizz_asset_group group)
{
    sx_assert(!g_asset.cur_group.id && "another group is already set, call (group_end)");

    if (!group.id) {
        sx_handle_t handle = sx_handle_new_and_grow(g_asset.group_handles, g_asset.alloc);
        sx_assert(handle);
        rizz__asset_group g = { 0 };
        int index = sx_handle_index(handle);
        if (index < sx_array_count(g_asset.groups))
            g_asset.groups[index] = g;
        else
            sx_array_push(g_asset.alloc, g_asset.groups, g);
        group = (rizz_asset_group){ handle };
    }

    g_asset.cur_group = group;
    return group;
}

static void rizz__asset_group_end(rizz_asset_group group)
{
    sx_assert(g_asset.cur_group.id == group.id && "group must be set (group_begin)");
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));
    sx_unused(group);

    g_asset.cur_group = (rizz_asset_group){ 0 };
}

static void rizz__asset_group_wait(rizz_asset_group group)
{
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));

    rizz__asset_group* g = &g_asset.groups[sx_handle_index(group.id)];
    bool loaded = false;

    while (!loaded) {
        loaded = true;
        for (int i = 0, c = sx_array_count(g->assets); i < c; i++) {
            rizz__asset* a = &g_asset.assets[sx_handle_index(g->assets[i].id)];
            if (a->state == RIZZ_ASSET_STATE_LOADING) {
                loaded = false;
                break;
            }
        }

        if (!loaded) {
            rizz__asset_update();
            sx_yield_cpu();
        }
    }
}

static bool rizz__asset_group_loaded(rizz_asset_group group)
{
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));

    rizz__asset_group* g = &g_asset.groups[sx_handle_index(group.id)];

    for (int i = 0, c = sx_array_count(g->assets); i < c; i++) {
        rizz__asset* a = &g_asset.assets[sx_handle_index(g->assets[i].id)];
        if (a->state == RIZZ_ASSET_STATE_LOADING)
            return false;
    }

    return true;
}

static void rizz__asset_group_delete(rizz_asset_group group)
{
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));

    int index = sx_handle_index(group.id);
    rizz__asset_group* g = &g_asset.groups[index];
    sx_array_free(g_asset.alloc, g->assets);
    sx_array_pop(g_asset.groups, index);
    sx_handle_del(g_asset.group_handles, group.id);
}

static void rizz__asset_group_unload(rizz_asset_group group)
{
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));

    rizz__asset_group* g = &g_asset.groups[sx_handle_index(group.id)];
    for (int i = 0, c = sx_array_count(g->assets); i < c; i++) {
        rizz_asset asset = g->assets[i];
        if (asset.id)
            rizz__asset_unload(asset);
    }
    sx_array_clear(g->assets);
}

static int rizz__asset_group_gather(rizz_asset_group group, rizz_asset* out_handles,
                                    int max_handles)
{
    sx_assert_rel(sx_handle_valid(g_asset.group_handles, group.id));

    rizz__asset_group* g = &g_asset.groups[sx_handle_index(group.id)];
    int count = 0;
    for (int i = 0, c = sx_array_count(g->assets); i < c && count < max_handles; i++) {
        out_handles[count++] = g->assets[i];
    }
    return count;
}

rizz_api_asset the__asset = { .register_asset_type = rizz__register_asset_type,
                              .unregister_asset_type = rizz__unregister_asset_type,
                              .load = rizz__asset_load,
                              .load_from_mem = rizz__asset_load_from_mem,
                              .unload = rizz__asset_unload,
                              .load_meta_cache = rizz__asset_load_meta_cache,
                              .state = rizz__asset_state,
                              .path = rizz__asset_path,
                              .type_name = rizz__asset_typename,
                              .params = rizz__asset_params,
                              .tags = rizz__asset_tags,
                              .obj = rizz__asset_obj,
                              .obj_threadsafe = rizz__asset_obj_threadsafe,
                              .ref_add = rizz__asset_ref_add,
                              .ref_count = rizz__asset_ref_count,
                              .reload_by_type = rizz__asset_reload_by_type,
                              .gather_by_type = rizz__asset_gather_by_type,
                              .unload_by_type = rizz__asset_unload_by_type,
                              .reload_by_tags = rizz__asset_reload_by_tags,
                              .gather_by_tags = rizz__asset_gather_by_tags,
                              .unload_by_tags = rizz__asset_unload_by_tags,
                              .group_begin = rizz__asset_group_begin,
                              .group_end = rizz__asset_group_end,
                              .group_wait = rizz__asset_group_wait,
                              .group_loaded = rizz__asset_group_loaded,
                              .group_delete = rizz__asset_group_delete,
                              .group_unload = rizz__asset_group_unload,
                              .group_gather = rizz__asset_group_gather };
