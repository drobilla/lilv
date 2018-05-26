/*
  Copyright 2007-2019 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "lilv_internal.h"

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "sratom/sratom.h"
#include "zix/tree.h"

#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/core/lv2.h"
#include "lv2/presets/presets.h"
#include "lv2/state/state.h"
#include "lv2/urid/urid.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	void*    value;  ///< Value/Object
	size_t   size;   ///< Size of value
	uint32_t key;    ///< Key/Predicate (URID)
	uint32_t type;   ///< Type of value (URID)
	uint32_t flags;  ///< State flags (POD, etc)
} Property;

typedef struct {
	char*     symbol; ///< Symbol of port
	LV2_Atom* atom;   ///< Value in port
} PortValue;

typedef struct {
	char* abs;  ///< Absolute path of actual file
	char* rel;  ///< Abstract path (relative path in state dir)
} PathMap;

typedef struct {
	size_t    n;
	Property* props;
} PropertyArray;

struct LilvStateImpl {
	LilvNode*     plugin_uri;   ///< Plugin URI
	LilvNode*     uri;          ///< State/preset URI
	char*         dir;          ///< Save directory (if saved)
	char*         scratch_dir;  ///< Directory for files created by plugin
	char*         copy_dir;     ///< Directory for snapshots of external files
	char*         link_dir;     ///< Directory for links to external files
	char*         label;        ///< State/Preset label
	ZixTree*      abs2rel;      ///< PathMap sorted by abs
	ZixTree*      rel2abs;      ///< PathMap sorted by rel
	PropertyArray props;        ///< State properties
	PropertyArray metadata;     ///< State metadata
	PortValue*    values;       ///< Port values
	uint32_t      atom_Path;    ///< atom:Path URID
	uint32_t      n_values;     ///< Number of port values
};

static int
abs_cmp(const void* a, const void* b, void* user_data)
{
	return strcmp(((const PathMap*)a)->abs, ((const PathMap*)b)->abs);
}

static int
rel_cmp(const void* a, const void* b, void* user_data)
{
	return strcmp(((const PathMap*)a)->rel, ((const PathMap*)b)->rel);
}

static int
property_cmp(const void* a, const void* b)
{
	return ((const Property*)a)->key - ((const Property*)b)->key;
}

static int
value_cmp(const void* a, const void* b)
{
	return strcmp(((const PortValue*)a)->symbol,
	              ((const PortValue*)b)->symbol);
}

static void
path_rel_free(void* ptr)
{
	free(((PathMap*)ptr)->abs);
	free(((PathMap*)ptr)->rel);
	free(ptr);
}

static LV2_Atom*
new_atom(const uint32_t size, const uint32_t type, const void* const value)
{
	LV2_Atom* atom = (LV2_Atom*)malloc(sizeof(LV2_Atom) + size);
	atom->size = size;
	atom->type = type;
	memcpy(atom + 1, value, size);
	return atom;
}

static PortValue*
append_port_value(LilvState* state, const char* port_symbol, LV2_Atom* atom)
{
	PortValue* pv = NULL;
	if (atom) {
		state->values = (PortValue*)realloc(
			state->values, (++state->n_values) * sizeof(PortValue));

		pv         = &state->values[state->n_values - 1];
		pv->symbol = lilv_strdup(port_symbol);
		pv->atom   = atom;
	}
	return pv;
}

static PortValue*
append_port_value_raw(LilvState*  state,
                      const char* port_symbol,
                      const void* value,
                      uint32_t    size,
                      uint32_t    type)
{
	return value ? append_port_value(
	                       state, port_symbol, new_atom(size, type, value))
	             : NULL;
}

static const char*
lilv_state_rel2abs(const LilvState* state, const char* path)
{
	ZixTreeIter*  iter = NULL;
	const PathMap key  = { NULL, (char*)path };
	if (state->rel2abs && !zix_tree_find(state->rel2abs, &key, &iter)) {
		return ((const PathMap*)zix_tree_get(iter))->abs;
	}
	return path;
}

static void
append_property(LilvState*     state,
                PropertyArray* array,
                uint32_t       key,
                const void*    value,
                size_t         size,
                uint32_t       type,
                uint32_t       flags)
{
	array->props = (Property*)realloc(
		array->props, (++array->n) * sizeof(Property));

	Property* const prop = &array->props[array->n - 1];
	if ((flags & LV2_STATE_IS_POD) || type == state->atom_Path) {
		prop->value = malloc(size);
		memcpy(prop->value, value, size);
	} else {
		prop->value = (void*)value;
	}

	prop->size  = size;
	prop->key   = key;
	prop->type  = type;
	prop->flags = flags;
}

static const Property*
find_property(const LilvState* const state, const uint32_t key)
{
	const Property search_key = {NULL, 0, key, 0, 0};

	return (const Property*)bsearch(&search_key,
	                                state->props.props,
	                                state->props.n,
	                                sizeof(Property),
	                                property_cmp);
}

static LV2_State_Status
store_callback(LV2_State_Handle handle,
               uint32_t         key,
               const void*      value,
               size_t           size,
               uint32_t         type,
               uint32_t         flags)
{
	LilvState* const state = (LilvState*)handle;

	if (!key) {
		return LV2_STATE_ERR_UNKNOWN; // TODO: Add status for bad arguments
	}

	if (find_property((const LilvState*)handle, key)) {
		return LV2_STATE_ERR_UNKNOWN; // TODO: Add status for duplicate keys
	}

	append_property(state, &state->props, key, value, size, type, flags);
	return LV2_STATE_SUCCESS;
}

static const void*
retrieve_callback(LV2_State_Handle handle,
                  uint32_t         key,
                  size_t*          size,
                  uint32_t*        type,
                  uint32_t*        flags)
{
	const Property* const prop = find_property((const LilvState*)handle, key);

	if (prop) {
		*size  = prop->size;
		*type  = prop->type;
		*flags = prop->flags;
		return prop->value;
	}
	return NULL;
}

static bool
lilv_state_has_path(const char* path, const void* state)
{
	return lilv_state_rel2abs((const LilvState*)state, path) != path;
}

static char*
make_path(LV2_State_Make_Path_Handle handle, const char* path)
{
	LilvState* state = (LilvState*)handle;
	lilv_mkdir_p(state->dir);

	return lilv_path_join(state->dir, path);
}

static char*
abstract_path(LV2_State_Map_Path_Handle handle,
              const char*               abs_path)
{
	LilvState*    state     = (LilvState*)handle;
	char*         path      = NULL;
	char*         real_path = lilv_realpath(abs_path);
	const PathMap key       = { real_path, NULL };
	ZixTreeIter*  iter      = NULL;

	if (abs_path[0] == '\0') {
		return lilv_strdup(abs_path);
	} else if (!zix_tree_find(state->abs2rel, &key, &iter)) {
		// Already mapped path in a previous call
		PathMap* pm = (PathMap*)zix_tree_get(iter);
		free(real_path);
		return lilv_strdup(pm->rel);
	} else if (lilv_path_is_child(real_path, state->dir)) {
		// File in state directory (loaded, or created by plugin during save)
		path = lilv_path_relative_to(real_path, state->dir);
	} else if (lilv_path_is_child(real_path, state->scratch_dir)) {
		// File created by plugin earlier
		path = lilv_path_relative_to(real_path, state->scratch_dir);
		if (state->copy_dir) {
			int st = lilv_mkdir_p(state->copy_dir);
			if (st) {
				LILV_ERRORF("Error creating directory %s (%s)\n",
				            state->copy_dir, strerror(st));
			}

			char* cpath = lilv_path_join(state->copy_dir, path);
			char* copy  = lilv_get_latest_copy(real_path, cpath);
			if (!copy || !lilv_file_equals(real_path, copy)) {
				// No recent enough copy, make a new one
				free(copy);
				copy = lilv_find_free_path(cpath, lilv_path_exists, NULL);
				if ((st = lilv_copy_file(real_path, copy))) {
					LILV_ERRORF("Error copying state file %s (%s)\n",
					            copy, strerror(st));
				}
			}
			free(real_path);
			free(cpath);

			// Refer to the latest copy in plugin state
			real_path = copy;
		}
	} else if (state->link_dir) {
		// New path outside state directory, make a link
		const char* slash = strrchr(real_path, '/');
		const char* name  = slash ? (slash + 1) : real_path;

		// Find a free name in the (virtual) state directory
		path = lilv_find_free_path(name, lilv_state_has_path, state);
	} else {
		// No link directory, preserve absolute path
		path = lilv_strdup(abs_path);
	}

	// Add record to path mapping
	PathMap* pm = (PathMap*)malloc(sizeof(PathMap));
	pm->abs = real_path;
	pm->rel = lilv_strdup(path);
	zix_tree_insert(state->abs2rel, pm, NULL);
	zix_tree_insert(state->rel2abs, pm, NULL);

	return path;
}

static char*
absolute_path(LV2_State_Map_Path_Handle handle,
              const char*               state_path)
{
	LilvState* state = (LilvState*)handle;
	char*      path  = NULL;
	if (lilv_path_is_absolute(state_path)) {
		// Absolute path, return identical path
		path = lilv_strdup(state_path);
	} else if (state->dir) {
		// Relative path inside state directory
		path = lilv_path_join(state->dir, state_path);
	} else {
		// State has not been saved, unmap
		path = lilv_strdup(lilv_state_rel2abs(state, state_path));
	}

	return path;
}

/** Return a new features array with built-in features added to `features`. */
static const LV2_Feature**
add_features(const LV2_Feature* const* features,
             const LV2_Feature*        map,
             const LV2_Feature*        make,
             const LV2_Feature*        free)
{
	size_t n_features = 0;
	for (; features && features[n_features]; ++n_features) {}

	const LV2_Feature** ret = (const LV2_Feature**)calloc(
		n_features + 4, sizeof(LV2_Feature*));

	if (features) {
		memcpy(ret, features, n_features * sizeof(LV2_Feature*));
	}

	size_t i = n_features;
	if (map) {
		ret[i++] = map;
	}
	if (make) {
		ret[i++] = make;
	}
	if (free) {
		ret[i++] = free;
	}

	return ret;
}

static char*
absolute_dir(const char* path)
{
	char* abs_path = lilv_path_absolute(path);
	char* base     = lilv_path_join(abs_path, NULL);
	free(abs_path);
	return base;
}

static const char*
state_strerror(LV2_State_Status st)
{
	switch (st) {
	case LV2_STATE_SUCCESS:         return "Completed successfully";
	case LV2_STATE_ERR_BAD_TYPE:    return "Unsupported type";
	case LV2_STATE_ERR_BAD_FLAGS:   return "Unsupported flags";
	case LV2_STATE_ERR_NO_FEATURE:  return "Missing features";
	case LV2_STATE_ERR_NO_PROPERTY: return "Missing property";
	default:                        return "Unknown error";
	}
}

static void
lilv_free_path(LV2_State_Free_Path_Handle handle, char* path)
{
	lilv_free(path);
}

LILV_API LilvState*
lilv_state_new_from_instance(const LilvPlugin*          plugin,
                             LilvInstance*              instance,
                             LV2_URID_Map*              map,
                             const char*                scratch_dir,
                             const char*                copy_dir,
                             const char*                link_dir,
                             const char*                save_dir,
                             LilvGetPortValueFunc       get_value,
                             void*                      user_data,
                             uint32_t                   flags,
                             const LV2_Feature *const * features)
{
	const LV2_Feature** sfeatures = NULL;
	LilvWorld* const    world     = plugin->world;
	LilvState* const    state     = (LilvState*)calloc(1, sizeof(LilvState));
	state->plugin_uri  = lilv_node_duplicate(lilv_plugin_get_uri(plugin));
	state->abs2rel     = zix_tree_new(false, abs_cmp, NULL, path_rel_free);
	state->rel2abs     = zix_tree_new(false, rel_cmp, NULL, NULL);
	state->scratch_dir = scratch_dir ? absolute_dir(scratch_dir) : NULL;
	state->copy_dir    = copy_dir ? absolute_dir(copy_dir) : NULL;
	state->link_dir    = link_dir ? absolute_dir(link_dir) : NULL;
	state->dir         = save_dir ? absolute_dir(save_dir) : NULL;
	state->atom_Path   = map->map(map->handle, LV2_ATOM__Path);

	LV2_State_Map_Path  pmap          = { state, abstract_path, absolute_path };
	LV2_Feature         pmap_feature  = { LV2_STATE__mapPath, &pmap };
	LV2_State_Make_Path pmake         = { state, make_path };
	LV2_Feature         pmake_feature = { LV2_STATE__makePath, &pmake };
	LV2_State_Free_Path pfree         = { NULL, lilv_free_path };
	LV2_Feature         pfree_feature = { LV2_STATE__freePath, &pfree };
	features = sfeatures = add_features(features, &pmap_feature,
	                                    save_dir ? &pmake_feature : NULL,
	                                    &pfree_feature);

	// Store port values
	if (get_value) {
		LilvNode* lv2_ControlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
		LilvNode* lv2_InputPort   = lilv_new_uri(world, LV2_CORE__InputPort);
		for (uint32_t i = 0; i < plugin->num_ports; ++i) {
			const LilvPort* const port = plugin->ports[i];
			if (lilv_port_is_a(plugin, port, lv2_ControlPort)
			    && lilv_port_is_a(plugin, port, lv2_InputPort)) {
				uint32_t size, type;
				const char* sym   = lilv_node_as_string(port->symbol);
				const void* value = get_value(sym, user_data, &size, &type);
				append_port_value_raw(state, sym, value, size, type);
			}
		}
		lilv_node_free(lv2_ControlPort);
		lilv_node_free(lv2_InputPort);
	}

	// Store properties
	const LV2_Descriptor*      desc  = instance->lv2_descriptor;
	const LV2_State_Interface* iface = (desc->extension_data)
		? (const LV2_State_Interface*)desc->extension_data(LV2_STATE__interface)
		: NULL;

	if (iface) {
		LV2_State_Status st = iface->save(
			instance->lv2_handle, store_callback, state, flags, features);
		if (st) {
			LILV_ERRORF("Error saving plugin state: %s\n", state_strerror(st));
			free(state->props.props);
			state->props.props = NULL;
			state->props.n     = 0;
		} else {
			qsort(state->props.props, state->props.n, sizeof(Property), property_cmp);
		}
	}

	qsort(state->values, state->n_values, sizeof(PortValue), value_cmp);

	free(sfeatures);
	return state;
}

LILV_API void
lilv_state_emit_port_values(const LilvState*     state,
                            LilvSetPortValueFunc set_value,
                            void*                user_data)
{
	for (uint32_t i = 0; i < state->n_values; ++i) {
		const PortValue* value = &state->values[i];
		const LV2_Atom*  atom  = value->atom;
		set_value(value->symbol, user_data, atom + 1, atom->size, atom->type);
	}
}

LILV_API void
lilv_state_restore(const LilvState*           state,
                   LilvInstance*              instance,
                   LilvSetPortValueFunc       set_value,
                   void*                      user_data,
                   uint32_t                   flags,
                   const LV2_Feature *const * features)
{
	if (!state) {
		LILV_ERROR("lilv_state_restore() called on NULL state\n");
		return;
	}

	LV2_State_Map_Path map_path = {
		(LilvState*)state, abstract_path, absolute_path };
	LV2_Feature map_feature = { LV2_STATE__mapPath, &map_path };

	LV2_State_Free_Path free_path    = { NULL, lilv_free_path };
	LV2_Feature         free_feature = { LV2_STATE__freePath, &free_path };

	if (instance) {
		const LV2_Descriptor* desc = instance->lv2_descriptor;
		if (desc->extension_data) {
			const LV2_State_Interface* iface = (const LV2_State_Interface*)
				desc->extension_data(LV2_STATE__interface);

			if (iface && iface->restore) {
				const LV2_Feature** sfeatures = add_features(
					features, &map_feature, NULL, &free_feature);

				iface->restore(instance->lv2_handle, retrieve_callback,
				               (LV2_State_Handle)state, flags, sfeatures);

				free(sfeatures);
			}
		}
	}


	if (set_value) {
		lilv_state_emit_port_values(state, set_value, user_data);
	}
}

static void
set_state_dir_from_model(LilvState* state, const SerdNode* graph)
{
	if (!state->dir && graph) {
		const char* uri = serd_node_get_string(graph);
		state->dir = lilv_file_uri_parse(uri, NULL);
	}
	assert(!state->dir || lilv_path_is_absolute(state->dir));
}

static void
add_object_to_properties(SerdModel*           model,
                         const SerdEnv*       env,
                         LV2_URID_Map*        map,
                         const SerdStatement* s,
                         SratomForger*        forger,
                         LV2_URID             atom_Path,
                         PropertyArray*       props)
{
	const SerdNode* p   = serd_statement_get_predicate(s);
	const SerdNode* o   = serd_statement_get_object(s);
	const char*     key = serd_node_get_string(p);

	LV2_Atom* atom =
		sratom_from_model(forger, serd_env_get_base_uri(env), model, o, 0);

	Property        prop = {
		malloc(atom->size),
		atom->size,
		map->map(map->handle, key),
		atom->type,
		LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE
	};

	memcpy(prop.value, LV2_ATOM_BODY_CONST(atom), atom->size);
	if (atom->type == atom_Path) {
		prop.flags = LV2_STATE_IS_POD;
	}

	if (prop.value) {
		props->props = (Property*)realloc(
			props->props, (++props->n) * sizeof(Property));
		props->props[props->n - 1] = prop;
	}

	sratom_free(atom);
}

static LilvState*
new_state_from_model(LilvWorld*       world,
                     LV2_URID_Map*    map,
                     SerdModel*       model,
                     const SerdNode*  node,
                     const char*      dir)
{
	// Check that we know at least something about this state subject
	if (!serd_model_ask(model, node, 0, 0, 0)) {
		return NULL;
	}

	// Allocate state
	LilvState* const state = (LilvState*)calloc(1, sizeof(LilvState));
	state->dir       = lilv_strdup(dir);
	state->atom_Path = map->map(map->handle, LV2_ATOM__Path);
	state->uri       = serd_node_copy(node);

	// Get the plugin URI this state applies to
	SerdIter* i = serd_model_find(model, node, world->uris.lv2_appliesTo, 0, 0);
	if (i) {
		const SerdStatement* s      = serd_iter_get(i);
		const SerdNode*      object = serd_statement_get_object(s);
		const SerdNode*      graph  = serd_statement_get_graph(s);
		state->plugin_uri = serd_node_copy(object);
		set_state_dir_from_model(state, graph);
		serd_iter_free(i);
	} else if (serd_model_ask(model,
	                          node,
	                          world->uris.rdf_a,
	                          world->uris.lv2_Plugin, 0)) {
		// Loading plugin description as state (default state)
		state->plugin_uri = serd_node_copy(node);
	} else {
		LILV_ERRORF("State %s missing lv2:appliesTo property\n",
		            serd_node_get_string(node));
	}

	// Get the state label
	i = serd_model_find(model, node, world->uris.rdfs_label, NULL, NULL);
	if (i) {
		const SerdStatement* s      = serd_iter_get(i);
		const SerdNode*      object = serd_statement_get_object(s);
		const SerdNode*      graph  = serd_statement_get_graph(s);
		state->label = lilv_strdup(serd_node_get_string(object));
		set_state_dir_from_model(state, graph);
		serd_iter_free(i);
	}

	SerdEnv*       env    = serd_env_new(NULL);
	SratomForger*  forger = sratom_forger_new(world->world, map);
	SerdBuffer     buffer = {NULL, 0};

	// Get metadata
	SerdRange* meta = serd_model_range(model, node, 0, 0, 0);
	FOREACH_MATCH(s, meta) {
		const SerdNode* p = serd_statement_get_predicate(s);
		const SerdNode* o = serd_statement_get_object(s);
		if (!serd_node_equals(p, world->uris.state_state) &&
		    !serd_node_equals(p, world->uris.lv2_appliesTo) &&
		    !serd_node_equals(p, world->uris.lv2_port) &&
		    !serd_node_equals(p, world->uris.rdfs_label) &&
		    !serd_node_equals(p, world->uris.rdfs_seeAlso) &&
		    !(serd_node_equals(p, world->uris.rdf_a) &&
		      serd_node_equals(o, world->uris.pset_Preset))) {
			add_object_to_properties(
				model, env, map, s, forger, state->atom_Path, &state->metadata);
		}
	}
	serd_range_free(meta);

	// Get port values
	SerdRange* ports = serd_model_range(model, node, world->uris.lv2_port, 0, 0);
	FOREACH_MATCH(s, ports) {
		const SerdNode* port = serd_statement_get_object(s);

		const SerdNode* label  = serd_model_get(model, port, world->uris.rdfs_label, 0, 0);
		const SerdNode* symbol = serd_model_get(model, port, world->uris.lv2_symbol, 0, 0);
		const SerdNode* value  = serd_model_get(model, port, world->uris.pset_value, 0, 0);
		if (!value) {
			value = serd_model_get(model, port, world->uris.lv2_default, 0, 0);
		}
		if (!symbol) {
			LILV_ERRORF("State `%s' port missing symbol.\n",
			            serd_node_get_string(node));
		} else if (value) {
			append_port_value(state,
			                  serd_node_get_string(symbol),
			                  sratom_from_model(forger, NULL, model, value, 0));

			if (label) {
				lilv_state_set_label(state, serd_node_get_string(label));
			}
		}
	}
	serd_range_free(ports);

	// Get properties
	const SerdNode* state_node =
		serd_model_get(model, node, world->uris.state_state, NULL, NULL);
	if (state_node) {
		SerdRange* props = serd_model_range(model, state_node, 0, 0, 0);
		FOREACH_MATCH(s, props) {
			add_object_to_properties(
				model, env, map, s, forger, state->atom_Path, &state->props);
		}
		serd_range_free(props);
	}

	serd_free(buffer.buf);
	sratom_forger_free(forger);

	if (state->props.props) {
		qsort(state->props.props, state->props.n, sizeof(Property), property_cmp);
	}
	if (state->values) {
		qsort(state->values, state->n_values, sizeof(PortValue), value_cmp);
	}

	return state;
}

LILV_API LilvState*
lilv_state_new_from_world(LilvWorld*      world,
                          LV2_URID_Map*   map,
                          const LilvNode* node)
{
	if (!lilv_node_is_uri(node) && !lilv_node_is_blank(node)) {
		LILV_ERRORF("Subject `%s' is not a URI or blank node.\n",
		            lilv_node_as_string(node));
		return NULL;
	}

	return new_state_from_model(world, map, world->model, node, NULL);
}

LILV_API LilvState*
lilv_state_new_from_file(LilvWorld*      world,
                         LV2_URID_Map*   map,
                         const LilvNode* subject,
                         const char*     path)
{
	if (subject && !lilv_node_is_uri(subject)
	    && !lilv_node_is_blank(subject)) {
		LILV_ERRORF("Subject `%s' is not a URI or blank node.\n",
		            lilv_node_as_string(subject));
		return NULL;
	}

	char*         abs_path = lilv_path_absolute(path);
	SerdNode*     node     = serd_new_file_uri(abs_path, NULL);
	SerdEnv*      env      = serd_env_new(node);
	SerdModel*    model    = serd_model_new(world->world, SERD_INDEX_SPO);
	SerdInserter* inserter = serd_inserter_new(model, env, NULL);
	SerdReader*   reader =
		serd_reader_new(world->world,
		                SERD_TURTLE,
		                0,
		                serd_inserter_get_sink(inserter),
		                LILV_READER_STACK_SIZE);

	serd_reader_start_file(reader, serd_node_get_string(node), true);
	serd_reader_read_document(reader);
	serd_reader_finish(reader);

	const SerdNode* subject_node = subject ? subject : node;

	char* dirname   = lilv_dirname(path);
	char* real_path = lilv_realpath(dirname);
	LilvState* state = new_state_from_model(
		world, map, model, subject_node, real_path);
	free(dirname);
	free(real_path);

	serd_node_free(node);
	free(abs_path);
	serd_reader_free(reader);
	serd_model_free(model);
	serd_env_free(env);
	return state;
}

static void
set_prefixes(SerdEnv* env)
{
#define SET_PSET(e, p, u) serd_env_set_prefix_from_strings(e, p, u)
	SET_PSET(env, "atom",  LV2_ATOM_PREFIX);
	SET_PSET(env, "lv2",   LV2_CORE_PREFIX);
	SET_PSET(env, "pset",  LV2_PRESETS_PREFIX);
	SET_PSET(env, "rdf",   LILV_NS_RDF);
	SET_PSET(env, "rdfs",  LILV_NS_RDFS);
	SET_PSET(env, "state", LV2_STATE_PREFIX);
	SET_PSET(env, "xsd",   LILV_NS_XSD);
}

LILV_API LilvState*
lilv_state_new_from_string(LilvWorld*    world,
                           LV2_URID_Map* map,
                           const char*   str)
{
	if (!str) {
		return NULL;
	}

	SerdEnv*      env      = serd_env_new(NULL);
	SerdModel*    model =
	        serd_model_new(world->world, SERD_INDEX_SPO | SERD_INDEX_OPS);
	SerdInserter* inserter = serd_inserter_new(model, env, NULL);
	SerdReader*   reader   =
		serd_reader_new(world->world,
		                SERD_TURTLE,
		                0,
		                serd_inserter_get_sink(inserter),
		                LILV_READER_STACK_SIZE);

	set_prefixes(env);
	serd_reader_start_string(reader, str, NULL);
	serd_reader_read_document(reader);

	SerdNode*       o = serd_new_uri(LV2_PRESETS__Preset);
	const SerdNode* s = serd_model_get(model, NULL, world->uris.rdf_a, o, NULL);

	LilvState* state = new_state_from_model(world, map, model, s, NULL);

	serd_node_free(o);
	serd_reader_free(reader);
	serd_model_free(model);
	serd_env_free(env);

	return state;
}

static SerdWriter*
ttl_writer(SerdWorld*      world,
           SerdWriteFunc   sink,
           void*           stream,
           const SerdNode* base,
           SerdEnv**       new_env)
{
	SerdEnv* env = *new_env ? *new_env : serd_env_new(base);
	set_prefixes(env);

	SerdWriter* writer =
		serd_writer_new(world, SERD_TURTLE, 0, env, sink, stream);

	if (!*new_env) {
		*new_env = env;
	}

	return writer;
}

static SerdWriter*
ttl_file_writer(SerdWorld* world, FILE* fd, const SerdNode* node, SerdEnv** env)
{
	SerdWriter* writer =
		ttl_writer(world, (SerdWriteFunc)fwrite, fd, node, env);

	fseek(fd, 0, SEEK_END);
	if (ftell(fd) == 0) {
		serd_env_write_prefixes(*env, serd_writer_get_sink(writer));
	} else {
		fprintf(fd, "\n");
	}

	return writer;
}

static void
remove_manifest_entry(SerdWorld* world, SerdModel* model, const char* subject)
{
	SerdNode*  s = serd_new_uri(subject);
	SerdRange* r = serd_model_range(model, s, NULL, NULL, NULL);

	serd_model_erase_range(model, r);

	serd_range_free(r);
	serd_node_free(s);
}

static int
add_state_to_manifest(LilvWorld*      lworld,
                      const LilvNode* plugin_uri,
                      const char*     manifest_path,
                      const char*     state_uri,
                      const char*     state_path)
{
	SerdWorld*    world    = lworld->world;
	SerdNode*     manifest = serd_new_file_uri(manifest_path, 0);
	SerdNode*     file     = serd_new_file_uri(state_path, 0);
	SerdEnv*      env      = serd_env_new(manifest);
	SerdModel* model = serd_model_new(world, SERD_INDEX_SPO | SERD_INDEX_OPS);
	SerdInserter* inserter = serd_inserter_new(model, env, NULL);

	FILE* rfd = fopen(manifest_path, "r");
	if (rfd) {
		// Read manifest into model
		SerdReader* reader =
			serd_reader_new(world,
			                SERD_TURTLE,
			                0,
			                serd_inserter_get_sink(inserter),
			                LILV_READER_STACK_SIZE);
		lilv_flock(rfd, true);
		serd_reader_start_stream(reader,
		                         (SerdReadFunc)fread,
		                         (SerdStreamErrorFunc)ferror,
		                         rfd,
		                         manifest,
		                         PAGE_SIZE);
		serd_reader_read_document(reader);
		serd_reader_free(reader);
	}

	// Choose state URI (use file URI if not given)
	if (!state_uri) {
		state_uri = serd_node_get_string(file);
	}

	// Remove any existing manifest entries for this state
	remove_manifest_entry(world, model, state_uri);

	// Add manifest entry for this state to model
	SerdNode* s = serd_new_uri(state_uri);

	// <state> a pset:Preset
	serd_model_add(model,
	               s,
	               lworld->uris.rdf_a,
	               lworld->uris.pset_Preset,
	               NULL);

	// <state> rdfs:seeAlso <file>
	serd_model_add(model,
	               s,
	               lworld->uris.rdfs_seeAlso,
	               file,
	               NULL);

	// <state> lv2:appliesTo <plugin>
	serd_model_add(model,
	               s,
	               lworld->uris.lv2_appliesTo,
	               plugin_uri,
	               NULL);

	// Write manifest model to file
	FILE* wfd = fopen(manifest_path, "w");
	if (wfd) {
		SerdWriter* writer = ttl_file_writer(world, wfd, manifest, &env);
		SerdRange* all = serd_model_all(model);
		serd_range_serialise(all,
		                     serd_writer_get_sink(writer),
		                     0);
		serd_range_free(all);
		serd_writer_free(writer);
		fclose(wfd);
	} else {
		LILV_ERRORF("Failed to open %s for writing (%s)\n",
		            manifest_path,
		            strerror(errno));
	}

	serd_node_free(s);
	serd_model_free(model);
	serd_node_free(file);
	serd_node_free(manifest);
	serd_env_free(env);

	if (rfd) {
		lilv_flock(rfd, false);
		fclose(rfd);
	}

	return 0;
}

static bool
link_exists(const char* path, const void* data)
{
	const char* target = (const char*)data;
	if (!lilv_path_exists(path, NULL)) {
		return false;
	}
	char* real_path = lilv_realpath(path);
	bool  matches   = !strcmp(real_path, target);
	free(real_path);
	return !matches;
}

static int
maybe_symlink(const char* oldpath, const char* newpath)
{
  return link_exists(newpath, oldpath) ? 0 : lilv_symlink(oldpath, newpath);
}

static void
write_property_array(const LilvState*     state,
                     const PropertyArray* array,
                     SratomStreamer*      streamer,
                     const SratomFlags    flags,
                     const SerdSink*      sink,
                     const SerdNode*      subject,
                     LV2_URID_Unmap*      unmap,
                     const char*          dir)
{
	for (uint32_t i = 0; i < array->n; ++i) {
		Property*   prop = &array->props[i];
		const char* key  = unmap->unmap(unmap->handle, prop->key);

		SerdNode* p = serd_new_uri(key);
		if (prop->type == state->atom_Path && !dir) {
			const char* path     = (const char*)prop->value;
			const char* abs_path = lilv_state_rel2abs(state, path);
			LILV_WARNF("Writing absolute path %s\n", abs_path);
			sratom_write(streamer, sink, subject, p,
			             prop->type, strlen(abs_path) + 1, abs_path, 0);
		} else if (prop->flags & LV2_STATE_IS_POD ||
		           prop->type == state->atom_Path) {
			sratom_write(streamer, sink, subject, p,
			             prop->type, prop->size, prop->value, 0);
		} else {
			LILV_WARNF("Lost non-POD property <%s> on save\n", key);
		}
		serd_node_free(p);
	}
}

static int
lilv_state_write(LilvWorld*       world,
                 LV2_URID_Map*    map,
                 LV2_URID_Unmap*  unmap,
                 const LilvState* state,
                 SerdWriter*      writer,
                 const char*      uri,
                 const char*      dir)
{
	const SerdNode* plugin_uri = state->plugin_uri;
	SerdNode*       subject    = serd_new_uri(uri ? uri : "");
	const SerdSink* iface      = serd_writer_get_sink(writer);

	// <subject> a pset:Preset
	serd_sink_write(iface,
	                0,
	                subject,
	                world->uris.rdf_a,
	                world->uris.pset_Preset,
	                NULL);

	// <subject> lv2:appliesTo <http://example.org/plugin>
	serd_sink_write(iface,
	                0,
	                subject,
	                world->uris.lv2_appliesTo,
	                plugin_uri,
	                NULL);

	// <subject> rdfs:label label
	if (state->label) {
		SerdNode* label = serd_new_string(state->label);
		serd_sink_write(iface,
		                0,
		                subject,
		                world->uris.rdfs_label,
		                label,
		                NULL);
		serd_node_free(label);
	}

	SratomStreamer* streamer = sratom_streamer_new(world->world, map, unmap);
	const SerdSink* sink     = serd_writer_get_sink(writer);

	// Write metadata (with precise types)
	write_property_array(state, &state->metadata, streamer, 0, sink,
	                     subject, unmap, dir);

	// Write port values (with pretty numbers)
	for (uint32_t i = 0; i < state->n_values; ++i) {
		PortValue* const value = &state->values[i];
		SerdNode*        port  = serd_new_blank(value->symbol);

		// <> lv2:port _:symbol
		serd_sink_write(iface,
		                SERD_ANON_O,
		                subject,
		                world->uris.lv2_port,
		                port,
		                NULL);

		// _:symbol lv2:symbol "symbol"
		SerdNode* symbol = serd_new_string(value->symbol);
		serd_sink_write(iface,
		                0,
		                port,
		                world->uris.lv2_symbol,
		                symbol,
		                NULL);

		// _:symbol pset:value value
		sratom_stream(streamer,
		              sink,
		              port,
		              world->uris.pset_value,
		              value->atom,
		              SRATOM_PRETTY_NUMBERS);

		serd_sink_write_end(iface, port);
	}

	// Write properties (with precise types)
	SerdNode* body = serd_new_blank("body");
	if (state->props.n > 0) {
		serd_sink_write(iface,
		                SERD_ANON_O,
		                subject,
		                world->uris.state_state,
		                body,
		                NULL);
	}
	write_property_array(
		state, &state->props, streamer, 0, sink, body, unmap, dir);

	if (state->props.n > 0) {
		serd_sink_write_end(iface, body);
	}

	serd_node_free(body);
	sratom_streamer_free(streamer);
	return 0;
}

static void
lilv_state_make_links(const LilvState* state, const char* dir)
{
	// Create symlinks to files
	for (ZixTreeIter* i = zix_tree_begin(state->abs2rel);
	     i != zix_tree_end(state->abs2rel);
	     i = zix_tree_iter_next(i)) {
		const PathMap* pm = (const PathMap*)zix_tree_get(i);

		char* path = lilv_path_join(dir, pm->rel);
		if (lilv_path_is_child(pm->abs, state->copy_dir)
		    && strcmp(state->copy_dir, dir)) {
			// Link directly to snapshot in the copy directory
			char* target = lilv_path_relative_to(pm->abs, dir);
			maybe_symlink(target, path);
			free(target);
		} else if (!lilv_path_is_child(pm->abs, dir)) {
			const char* link_dir = state->link_dir ? state->link_dir : dir;
			char*       pat      = lilv_path_join(link_dir, pm->rel);
			if (!strcmp(dir, link_dir)) {
				// Link directory is save directory, make link at exact path
				remove(pat);
				maybe_symlink(pm->abs, pat);
			} else {
				// Make a link in the link directory to external file
				char* lpath = lilv_find_free_path(pat, link_exists, pm->abs);
				if (!lilv_path_exists(lpath, NULL)) {
					lilv_symlink(pm->abs, lpath);
				}

				// Make a link in the save directory to the external link
				char* target = lilv_path_relative_to(lpath, dir);
				maybe_symlink(target, path);
				free(target);
				free(lpath);
			}
			free(pat);
		}
		free(path);
	}
}

LILV_API int
lilv_state_save(LilvWorld*       world,
                LV2_URID_Map*    map,
                LV2_URID_Unmap*  unmap,
                const LilvState* state,
                const char*      uri,
                const char*      dir,
                const char*      filename)
{
	if (!filename || !dir || lilv_mkdir_p(dir)) {
		return 1;
	}

	char*       abs_dir = absolute_dir(dir);
	char* const path    = lilv_path_join(abs_dir, filename);
	FILE*       fd      = fopen(path, "w");
	if (!fd) {
		LILV_ERRORF("Failed to open %s (%s)\n", path, strerror(errno));
		free(abs_dir);
		free(path);
		return 4;
	}

	// Create symlinks to files if necessary
	lilv_state_make_links(state, abs_dir);

	// Write state to Turtle file
	SerdNode*   file = serd_new_file_uri(path, NULL);
	SerdNode*   node = uri ? serd_new_uri(uri) : file;
	SerdEnv*    env  = NULL;
	SerdWriter* ttl  = ttl_file_writer(world->world, fd, file, &env);
	int         ret  = lilv_state_write(
		world, map, unmap, state, ttl, serd_node_get_string(node), dir);

	// Set saved dir and uri (FIXME: const violation)
	free(state->dir);
	lilv_node_free(state->uri);
	((LilvState*)state)->dir = lilv_strdup(abs_dir);
	((LilvState*)state)->uri = serd_node_copy(node);

	serd_node_free(file);
	serd_writer_free(ttl);
	serd_env_free(env);
	fclose(fd);

	// Add entry to manifest
	char* const manifest = lilv_path_join(abs_dir, "manifest.ttl");
	add_state_to_manifest(world, state->plugin_uri, manifest, uri, path);

	free(manifest);
	free(abs_dir);
	free(path);
	return ret;
}

LILV_API char*
lilv_state_to_string(LilvWorld*       world,
                     LV2_URID_Map*    map,
                     LV2_URID_Unmap*  unmap,
                     const LilvState* state,
                     const char*      uri,
                     const char*      base_uri)
{
	if (!uri) {
		LILV_ERROR("Attempt to serialise state with no URI\n");
		return NULL;
	}

	SerdBuffer    buffer = { NULL, 0 };
	SerdEnv*      env    = NULL;
	SerdNode*     base   = serd_new_uri(base_uri);
	SerdWriter*   writer = ttl_writer(
		world->world, (SerdWriteFunc)serd_buffer_sink, &buffer, base, &env);

	lilv_state_write(world, map, unmap, state, writer, uri, NULL);

	serd_writer_free(writer);
	serd_env_free(env);
	char* str    = serd_buffer_sink_finish(&buffer);
	char* result = lilv_strdup(str);
	serd_free(str);
	return result;
}

static void
try_unlink(const char* path)
{
	if (unlink(path)) {
		LILV_ERRORF("Failed to remove %s (%s)\n", path, strerror(errno));
	}
}

LILV_API int
lilv_state_delete(LilvWorld*       world,
                  const LilvState* state)
{
	if (!state->dir || !state->uri) {
		LILV_ERROR("Attempt to delete unsaved state\n");
		return -1;
	}

	LilvNode*  bundle        = lilv_new_file_uri(world, NULL, state->dir);
	LilvNode*  manifest      = lilv_world_get_manifest_uri(world, bundle);
	char*      manifest_path = lilv_node_get_path(manifest, NULL);
	SerdModel* model         = serd_model_new(world->world, SERD_INDEX_SPO);

	{
		// Read manifest into temporary local model
		SerdEnv*      env      = serd_env_new(manifest);
		SerdInserter* inserter = serd_inserter_new(model, env, NULL);
		SerdReader*   reader   =
			serd_reader_new(world->world,
			                SERD_TURTLE,
			                0,
			                serd_inserter_get_sink(inserter),
			                LILV_READER_STACK_SIZE);

		serd_reader_start_file(reader, manifest_path, true);
		serd_reader_read_document(reader);
		serd_reader_free(reader);
		serd_env_free(env);
	}

	const SerdNode* file = serd_model_get(
		model, state->uri, world->uris.rdfs_seeAlso, NULL, NULL);
	if (file) {
		// Remove state file
		char* path = serd_file_uri_parse(serd_node_get_string(file), NULL);
		try_unlink(path);
		serd_free(path);
	}

	// Remove any existing manifest entries for this state
	remove_manifest_entry(
		world->world, model, lilv_node_as_string(state->uri));
	remove_manifest_entry(
		world->world, world->model, lilv_node_as_string(state->uri));

	// Drop bundle from model
	lilv_world_unload_bundle(world, bundle);

	if (serd_model_size(model) == 0) {
		// Manifest is empty, attempt to remove bundle entirely
		try_unlink(manifest_path);

		// Remove all known files from state bundle
		for (ZixTreeIter* i = zix_tree_begin(state->abs2rel);
		     i != zix_tree_end(state->abs2rel);
		     i = zix_tree_iter_next(i)) {
			const PathMap* pm   = (const PathMap*)zix_tree_get(i);
			char*          path = lilv_path_join(state->dir, pm->rel);
			try_unlink(path);
			free(path);
		}

		if (rmdir(state->dir)) {
			LILV_ERRORF("Failed to remove directory %s (%s)\n",
			            state->dir, strerror(errno));
		}
	} else {
		// Still something in the manifest, reload bundle
		lilv_world_load_bundle(world, bundle);
	}

	serd_model_free(model);
	lilv_free(manifest_path);
	lilv_node_free(manifest);
	lilv_node_free(bundle);

	return 0;
}

static void
free_property_array(LilvState* state, PropertyArray* array)
{
	for (uint32_t i = 0; i < array->n; ++i) {
		Property* prop = &array->props[i];
		if ((prop->flags & LV2_STATE_IS_POD) ||
		    prop->type == state->atom_Path) {
			free(prop->value);
		}
	}
	free(array->props);
}

LILV_API void
lilv_state_free(LilvState* state)
{
	if (state) {
		free_property_array(state, &state->props);
		free_property_array(state, &state->metadata);
		for (uint32_t i = 0; i < state->n_values; ++i) {
			free(state->values[i].atom);
			free(state->values[i].symbol);
		}
		lilv_node_free(state->plugin_uri);
		lilv_node_free(state->uri);
		zix_tree_free(state->abs2rel);
		zix_tree_free(state->rel2abs);
		free(state->values);
		free(state->label);
		free(state->dir);
		free(state->scratch_dir);
		free(state->copy_dir);
		free(state->link_dir);
		free(state);
	}
}

LILV_API bool
lilv_state_equals(const LilvState* a, const LilvState* b)
{
	if (!lilv_node_equals(a->plugin_uri, b->plugin_uri)
	    || (a->label && !b->label)
	    || (b->label && !a->label)
	    || (a->label && b->label && strcmp(a->label, b->label))
	    || a->props.n != b->props.n
	    || a->n_values != b->n_values) {
		return false;
	}

	for (uint32_t i = 0; i < a->n_values; ++i) {
		PortValue* const av = &a->values[i];
		PortValue* const bv = &b->values[i];
		if (av->atom->size != bv->atom->size ||
		    av->atom->type != bv->atom->type ||
		    strcmp(av->symbol, bv->symbol) ||
		    memcmp(av->atom + 1, bv->atom + 1, av->atom->size)) {
			return false;
		}
	}

	for (uint32_t i = 0; i < a->props.n; ++i) {
		Property* const ap = &a->props.props[i];
		Property* const bp = &b->props.props[i];
		if (ap->key != bp->key
		    || ap->type != bp->type
		    || ap->flags != bp->flags) {
			return false;
		} else if (ap->type == a->atom_Path) {
			if (!lilv_file_equals(lilv_state_rel2abs(a, (char*)ap->value),
			                      lilv_state_rel2abs(b, (char*)bp->value))) {
				return false;
			}
		} else if (ap->size != bp->size
		           || memcmp(ap->value, bp->value, ap->size)) {
			return false;
		}
	}

	return true;
}

LILV_API unsigned
lilv_state_get_num_properties(const LilvState* state)
{
	return state->props.n;
}

LILV_API const LilvNode*
lilv_state_get_plugin_uri(const LilvState* state)
{
	return state->plugin_uri;
}

LILV_API const LilvNode*
lilv_state_get_uri(const LilvState* state)
{
	return state->uri;
}

LILV_API const char*
lilv_state_get_label(const LilvState* state)
{
	return state->label;
}

LILV_API void
lilv_state_set_label(LilvState* state, const char* label)
{
	const size_t len = strlen(label);
	state->label = (char*)realloc(state->label, len + 1);
	memcpy(state->label, label, len + 1);
}

LILV_API int
lilv_state_set_metadata(LilvState*  state,
                        uint32_t    key,
                        const void* value,
                        size_t      size,
                        uint32_t    type,
                        uint32_t    flags)
{
	append_property(state, &state->metadata, key, value, size, type, flags);
	return LV2_STATE_SUCCESS;
}
