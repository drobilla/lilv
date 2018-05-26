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

#include "lilv_config.h"
#include "lilv_internal.h"

#include "lilv/lilv.h"
#include "lv2/core/lv2.h"
#include "lv2/presets/presets.h"
#include "lv2/state/state.h"
#include "serd/serd.h"
#include "zix/common.h"
#include "zix/tree.h"

#ifdef LILV_DYN_MANIFEST
#    include "lv2/dynmanifest/dynmanifest.h"
#    include <dlfcn.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int
lilv_world_drop_graph(LilvWorld* world, const SerdNode* graph);

LILV_API LilvWorld*
lilv_world_new(void)
{
	LilvWorld* world = (LilvWorld*)calloc(1, sizeof(LilvWorld));

	world->world = serd_world_new();
	if (!world->world) {
		goto fail;
	}

	world->model = serd_model_new(
		world->world, SERD_INDEX_SPO | SERD_INDEX_OPS | SERD_INDEX_GRAPHS);
	if (!world->model) {
		goto fail;
	}

	world->specs          = NULL;
	world->plugin_classes = lilv_plugin_classes_new();
	world->plugins        = lilv_plugins_new();
	world->zombies        = lilv_plugins_new();
	world->loaded_files   = zix_tree_new(
		false, lilv_resource_node_cmp, NULL, (ZixDestroyFunc)lilv_node_free);

	world->libs = zix_tree_new(false, lilv_lib_compare, NULL, NULL);

#define NS_DCTERMS "http://purl.org/dc/terms/"
#define NS_DYNMAN  "http://lv2plug.in/ns/ext/dynmanifest#"
#define NS_OWL     "http://www.w3.org/2002/07/owl#"

#define NEW_URI(uri) serd_new_uri(uri)

	world->uris.dc_replaces         = NEW_URI(NS_DCTERMS   "replaces");
	world->uris.dman_DynManifest    = NEW_URI(NS_DYNMAN    "DynManifest");
	world->uris.doap_name           = NEW_URI(LILV_NS_DOAP "name");
	world->uris.lv2_Plugin          = NEW_URI(LV2_CORE__Plugin);
	world->uris.lv2_Specification   = NEW_URI(LV2_CORE__Specification);
	world->uris.lv2_appliesTo       = NEW_URI(LV2_CORE__appliesTo);
	world->uris.lv2_binary          = NEW_URI(LV2_CORE__binary);
	world->uris.lv2_default         = NEW_URI(LV2_CORE__default);
	world->uris.lv2_designation     = NEW_URI(LV2_CORE__designation);
	world->uris.lv2_extensionData   = NEW_URI(LV2_CORE__extensionData);
	world->uris.lv2_index           = NEW_URI(LV2_CORE__index);
	world->uris.lv2_latency         = NEW_URI(LV2_CORE__latency);
	world->uris.lv2_maximum         = NEW_URI(LV2_CORE__maximum);
	world->uris.lv2_microVersion    = NEW_URI(LV2_CORE__microVersion);
	world->uris.lv2_minimum         = NEW_URI(LV2_CORE__minimum);
	world->uris.lv2_minorVersion    = NEW_URI(LV2_CORE__minorVersion);
	world->uris.lv2_name            = NEW_URI(LV2_CORE__name);
	world->uris.lv2_optionalFeature = NEW_URI(LV2_CORE__optionalFeature);
	world->uris.lv2_port            = NEW_URI(LV2_CORE__port);
	world->uris.lv2_portProperty    = NEW_URI(LV2_CORE__portProperty);
	world->uris.lv2_reportsLatency  = NEW_URI(LV2_CORE__reportsLatency);
	world->uris.lv2_requiredFeature = NEW_URI(LV2_CORE__requiredFeature);
	world->uris.lv2_project         = NEW_URI(LV2_CORE__project);
	world->uris.lv2_prototype       = NEW_URI(LV2_CORE__prototype);
	world->uris.lv2_symbol          = NEW_URI(LV2_CORE__symbol);
	world->uris.owl_Ontology        = NEW_URI(NS_OWL "Ontology");
	world->uris.pset_Preset         = NEW_URI(LV2_PRESETS__Preset);
	world->uris.pset_value          = NEW_URI(LV2_PRESETS__value);
	world->uris.rdf_a               = NEW_URI(LILV_NS_RDF  "type");
	world->uris.rdf_value           = NEW_URI(LILV_NS_RDF  "value");
	world->uris.rdfs_Class          = NEW_URI(LILV_NS_RDFS "Class");
	world->uris.rdfs_label          = NEW_URI(LILV_NS_RDFS "label");
	world->uris.rdfs_seeAlso        = NEW_URI(LILV_NS_RDFS "seeAlso");
	world->uris.rdfs_subClassOf     = NEW_URI(LILV_NS_RDFS "subClassOf");
	world->uris.state_state         = NEW_URI(LV2_STATE__state);
	world->uris.xsd_base64Binary    = NEW_URI(LILV_NS_XSD  "base64Binary");
	world->uris.xsd_boolean         = NEW_URI(LILV_NS_XSD  "boolean");
	world->uris.xsd_decimal         = NEW_URI(LILV_NS_XSD  "decimal");
	world->uris.xsd_double          = NEW_URI(LILV_NS_XSD  "double");
	world->uris.xsd_int             = NEW_URI(LILV_NS_XSD  "int");
	world->uris.xsd_integer         = NEW_URI(LILV_NS_XSD  "integer");
	world->uris.null_uri            = NULL;

	world->lv2_plugin_class = lilv_plugin_class_new(
		world, NULL, world->uris.lv2_Plugin, "Plugin");
	assert(world->lv2_plugin_class);

	world->n_read_files        = 0;
	world->opt.filter_language = true;
	world->opt.dyn_manifest    = true;

	return world;

fail:
	/* keep on rockin' in the */ free(world);
	return NULL;
}

LILV_API void
lilv_world_free(LilvWorld* world)
{
	if (!world) {
		return;
	}

	lilv_plugin_class_free(world->lv2_plugin_class);
	world->lv2_plugin_class = NULL;

	for (SerdNode** n = (SerdNode**)&world->uris; *n; ++n) {
		serd_node_free(*n);
	}

	for (LilvSpec* spec = world->specs; spec;) {
		LilvSpec* next = spec->next;
		serd_node_free(spec->spec);
		serd_node_free(spec->bundle);
		lilv_nodes_free(spec->data_uris);
		free(spec);
		spec = next;
	}
	world->specs = NULL;

	LILV_FOREACH(plugins, i, world->plugins) {
		const LilvPlugin* p = lilv_plugins_get(world->plugins, i);
		lilv_plugin_free((LilvPlugin*)p);
	}
	zix_tree_free((ZixTree*)world->plugins);
	world->plugins = NULL;

	LILV_FOREACH(plugins, i, world->zombies) {
		const LilvPlugin* p = lilv_plugins_get(world->zombies, i);
		lilv_plugin_free((LilvPlugin*)p);
	}
	zix_tree_free((ZixTree*)world->zombies);
	world->zombies = NULL;

	zix_tree_free((ZixTree*)world->loaded_files);
	world->loaded_files = NULL;

	zix_tree_free(world->libs);
	world->libs = NULL;

	zix_tree_free((ZixTree*)world->plugin_classes);
	world->plugin_classes = NULL;

	serd_model_free(world->model);
	world->model = NULL;

	serd_world_free(world->world);
	world->world = NULL;

	free(world->opt.lv2_path);
	free(world);
}

LILV_API void
lilv_world_set_option(LilvWorld*      world,
                      const char*     uri,
                      const LilvNode* value)
{
	if (!strcmp(uri, LILV_OPTION_DYN_MANIFEST)) {
		if (lilv_node_is_bool(value)) {
			world->opt.dyn_manifest = lilv_node_as_bool(value);
			return;
		}
	} else if (!strcmp(uri, LILV_OPTION_FILTER_LANG)) {
		if (lilv_node_is_bool(value)) {
			world->opt.filter_language = lilv_node_as_bool(value);
			return;
		}
	} else if (!strcmp(uri, LILV_OPTION_LV2_PATH)) {
		if (lilv_node_is_string(value)) {
			world->opt.lv2_path = lilv_strdup(lilv_node_as_string(value));
			return;
		}
	}
	LILV_WARNF("Unrecognized or invalid option `%s'\n", uri);
}

LILV_API LilvNodes*
lilv_world_find_nodes(LilvWorld*      world,
                      const LilvNode* subject,
                      const LilvNode* predicate,
                      const LilvNode* object)
{
	if (subject && !lilv_node_is_uri(subject) && !lilv_node_is_blank(subject)) {
		LILV_ERRORF("Subject `%s' is not a resource\n",
		            serd_node_get_string(subject));
		return NULL;
	} else if (!predicate) {
		LILV_ERROR("Missing required predicate\n");
		return NULL;
	} else if (!lilv_node_is_uri(predicate)) {
		LILV_ERRORF("Predicate `%s' is not a URI\n",
		            serd_node_get_string(predicate));
		return NULL;
	} else if (!subject && !object) {
		LILV_ERROR("Both subject and object are NULL\n");
		return NULL;
	}

	return lilv_world_find_nodes_internal(world, subject, predicate, object);
}

LILV_API LilvNode*
lilv_world_get(LilvWorld*      world,
               const LilvNode* subject,
               const LilvNode* predicate,
               const LilvNode* object)
{
	return serd_node_copy(
		serd_model_get(world->model, subject, predicate, object, NULL));
}

LILV_API bool
lilv_world_ask(LilvWorld*      world,
               const LilvNode* subject,
               const LilvNode* predicate,
               const LilvNode* object)
{
	return serd_model_ask(world->model, subject, predicate, object, NULL);
}

SerdModel*
lilv_world_filter_model(LilvWorld*      world,
                        SerdModel*      model,
                        const SerdNode* subject,
                        const SerdNode* predicate,
                        const SerdNode* object,
                        const SerdNode* graph)
{
	SerdModel* results = serd_model_new(world->world, SERD_INDEX_SPO);
	SerdRange* r       = serd_model_range(model, subject, predicate, object, graph);
	for (; !serd_range_empty(r); serd_range_next(r)) {
		serd_model_insert(results, serd_range_front(r));
	}
	serd_range_free(r);
	return results;
}

LilvNodes*
lilv_world_find_nodes_internal(LilvWorld*      world,
                               const SerdNode* subject,
                               const SerdNode* predicate,
                               const SerdNode* object)
{
	return lilv_nodes_from_range(
		world,
		serd_model_range(world->model, subject, predicate, object, NULL),
		(object == NULL) ? SERD_OBJECT : SERD_SUBJECT);
}

const char*
lilv_world_blank_node_prefix(LilvWorld* world)
{
	static char str[32];
	snprintf(str, sizeof(str), "%d", world->n_read_files++);
	return str;
}

/** Comparator for sequences (e.g. world->plugins). */
int
lilv_header_compare_by_uri(const void* a, const void* b, void* user_data)
{
	const struct LilvHeader* const header_a = (const struct LilvHeader*)a;
	const struct LilvHeader* const header_b = (const struct LilvHeader*)b;
	return strcmp(lilv_node_as_uri(header_a->uri),
	              lilv_node_as_uri(header_b->uri));
}

/**
   Comparator for libraries (world->libs).

   Libraries do have a LilvHeader, but we must also compare the bundle to
   handle the case where the same library is loaded with different bundles, and
   consequently different contents (mainly plugins).
 */
int
lilv_lib_compare(const void* a, const void* b, void* user_data)
{
	const LilvLib* const lib_a = (const LilvLib*)a;
	const LilvLib* const lib_b = (const LilvLib*)b;
	int cmp = strcmp(lilv_node_as_uri(lib_a->uri),
	                 lilv_node_as_uri(lib_b->uri));
	return cmp ? cmp : strcmp(lib_a->bundle_path, lib_b->bundle_path);
}

/** Get an element of a collection of any object with an LilvHeader by URI. */
static ZixTreeIter*
lilv_collection_find_by_uri(const ZixTree* seq, const LilvNode* uri)
{
	ZixTreeIter* i = NULL;
	if (lilv_node_is_uri(uri)) {
		struct LilvHeader key = { NULL, (LilvNode*)uri };
		zix_tree_find(seq, &key, &i);
	}
	return i;
}

/** Get an element of a collection of any object with an LilvHeader by URI. */
struct LilvHeader*
lilv_collection_get_by_uri(const ZixTree* seq, const LilvNode* uri)
{
	ZixTreeIter* const i = lilv_collection_find_by_uri(seq, uri);

	return i ? (struct LilvHeader*)zix_tree_get(i) : NULL;
}

static void
lilv_world_add_spec(LilvWorld*      world,
                    const SerdNode* specification_node,
                    const SerdNode* bundle_node)
{
	LilvSpec* spec = (LilvSpec*)malloc(sizeof(LilvSpec));
	spec->spec      = serd_node_copy(specification_node);
	spec->bundle    = serd_node_copy(bundle_node);
	spec->data_uris = lilv_nodes_new();

	// Add all data files (rdfs:seeAlso)
	FOREACH_PAT(s,
	            world->model,
	            specification_node,
	            world->uris.rdfs_seeAlso,
	            NULL,
	            NULL)
	{
		const SerdNode* file_node = serd_statement_get_object(s);
		zix_tree_insert((ZixTree*)spec->data_uris,
		                serd_node_copy(file_node),
		                NULL);
	}

	// Add specification to world specification list
	spec->next   = world->specs;
	world->specs = spec;
}

static void
lilv_world_add_plugin(LilvWorld*      world,
                      const SerdNode* plugin_uri,
                      const LilvNode* manifest_uri,
                      void*           dynmanifest,
                      const SerdNode* bundle)
{
	ZixTreeIter* z          = NULL;
	LilvPlugin*  plugin     = (LilvPlugin*)lilv_plugins_get_by_uri(
		world->plugins, plugin_uri);

	if (plugin) {
		// Existing plugin, if this is different bundle, ignore it
		// (use the first plugin found in LV2_PATH)
		const LilvNode* last_bundle    = lilv_plugin_get_bundle_uri(plugin);
		const char*     plugin_uri_str = lilv_node_as_uri(plugin_uri);
		if (serd_node_equals(bundle, last_bundle)) {
			LILV_WARNF("Reloading plugin <%s>\n", plugin_uri_str);
			plugin->loaded = false;
		} else {
			LILV_WARNF("Duplicate plugin <%s>\n", plugin_uri_str);
			LILV_WARNF("... found in %s\n", lilv_node_as_string(last_bundle));
			LILV_WARNF("... and      %s (ignored)\n", serd_node_get_string(bundle));
			return;
		}
	} else if ((z = lilv_collection_find_by_uri((const ZixTree*)world->zombies,
	                                            plugin_uri))) {
		// Plugin bundle has been re-loaded, move from zombies to plugins
		plugin = (LilvPlugin*)zix_tree_get(z);
		zix_tree_remove((ZixTree*)world->zombies, z);
		zix_tree_insert((ZixTree*)world->plugins, plugin, NULL);
		lilv_plugin_clear(plugin, bundle);
	} else {
		// Add new plugin to the world
		plugin = lilv_plugin_new(world, plugin_uri, bundle);

		// Add manifest as plugin data file (as if it were rdfs:seeAlso)
		zix_tree_insert((ZixTree*)plugin->data_uris,
		                lilv_node_duplicate(manifest_uri),
		                NULL);

		// Add plugin to world plugin sequence
		zix_tree_insert((ZixTree*)world->plugins, plugin, NULL);
	}


#ifdef LILV_DYN_MANIFEST
	// Set dynamic manifest library URI, if applicable
	if (dynmanifest) {
		plugin->dynmanifest = (LilvDynManifest*)dynmanifest;
		++((LilvDynManifest*)dynmanifest)->refs;
	}
#endif

	// Add all plugin data files (rdfs:seeAlso)
	FOREACH_PAT(
		s, world->model, plugin_uri, world->uris.rdfs_seeAlso, NULL, NULL)
	{
		const SerdNode* file_node = serd_statement_get_object(s);
		zix_tree_insert((ZixTree*)plugin->data_uris,
		                serd_node_copy(file_node),
		                NULL);
	}
}

SerdStatus
lilv_world_load_graph(LilvWorld* world, const SerdNode* graph, const LilvNode* uri)
{
	SerdEnv*      env      = serd_env_new(uri);
	SerdInserter* inserter = serd_inserter_new(world->model, env, graph);
	SerdReader*   reader =
		serd_reader_new(world->world,
		                SERD_TURTLE,
		                0,
		                serd_inserter_get_sink(inserter),
		                LILV_READER_STACK_SIZE);

	const SerdStatus st = lilv_world_load_file(world, reader, uri);

	serd_reader_free(reader);
	serd_inserter_free(inserter);
	serd_env_free(env);
	return st;
}

static void
lilv_world_load_dyn_manifest(LilvWorld*      world,
                             const SerdNode* bundle_node,
                             const LilvNode* manifest)
{
#ifdef LILV_DYN_MANIFEST
	if (!world->opt.dyn_manifest) {
		return;
	}

	LV2_Dyn_Manifest_Handle handle = NULL;

	// ?dman a dynman:DynManifest bundle_node
	SerdModel* model = lilv_world_filter_model(world,
	                                           world->model,
	                                           NULL,
	                                           world->uris.rdf_a,
	                                           world->uris.dman_DynManifest,
	                                           bundle_node);
	SerdIter* iter = serd_begin(model);
	for (; !serd_iter_end(iter); serd_iter_next(iter)) {
		const SerdNode* dmanifest = serd_iter_get_node(iter, SERD_SUBJECT);

		// ?dman lv2:binary ?binary
		SerdIter* binaries = serd_model_find(world->model,
		                                     dmanifest,
		                                     world->uris.lv2_binary,
		                                     NULL,
		                                     bundle_node);
		if (serd_iter_end(binaries)) {
			serd_iter_free(binaries);
			LILV_ERRORF("Dynamic manifest in <%s> has no binaries, ignored\n",
			            serd_node_get_string(bundle_node));
			continue;
		}

		// Get binary path
		const SerdNode* binary   = serd_iter_get_node(binaries, SERD_OBJECT);
		const uint8_t*  lib_uri  = serd_node_get_string(binary);
		char*           lib_path = lilv_file_uri_parse(lib_uri, 0);
		if (!lib_path) {
			LILV_ERROR("No dynamic manifest library path\n");
			serd_iter_free(binaries);
			continue;
		}

		// Open library
		dlerror();
		void* lib = dlopen(lib_path, RTLD_LAZY);
		if (!lib) {
			LILV_ERRORF("Failed to open dynmanifest library `%s' (%s)\n",
			            lib_path, dlerror());
			serd_iter_free(binaries);
			lilv_free(lib_path);
			continue;
		}

		// Open dynamic manifest
		typedef int (*OpenFunc)(LV2_Dyn_Manifest_Handle*,
		                        const LV2_Feature *const *);
		OpenFunc dmopen = (OpenFunc)lilv_dlfunc(lib, "lv2_dyn_manifest_open");
		if (!dmopen || dmopen(&handle, &dman_features)) {
			LILV_ERRORF("No `lv2_dyn_manifest_open' in `%s'\n", lib_path);
			serd_iter_free(binaries);
			dlclose(lib);
			lilv_free(lib_path);
			continue;
		}

		// Get subjects (the data that would be in manifest.ttl)
		typedef int (*GetSubjectsFunc)(LV2_Dyn_Manifest_Handle, FILE*);
		GetSubjectsFunc get_subjects_func = (GetSubjectsFunc)lilv_dlfunc(
			lib, "lv2_dyn_manifest_get_subjects");
		if (!get_subjects_func) {
			LILV_ERRORF("No `lv2_dyn_manifest_get_subjects' in `%s'\n",
			            lib_path);
			serd_iter_free(binaries);
			dlclose(lib);
			lilv_free(lib_path);
			continue;
		}

		LilvDynManifest* desc = (LilvDynManifest*)malloc(sizeof(LilvDynManifest));
		desc->bundle = serd_node_copy(bundle_node);
		desc->lib    = lib;
		desc->handle = handle;
		desc->refs   = 0;

		serd_iter_free(binaries);

		// Generate data file
		FILE* fd = tmpfile();
		get_subjects_func(handle, fd);
		rewind(fd);

		// Parse generated data file into temporary model
		// FIXME
		const SerdNode* base   = dmanifest;
		SerdEnv*        env    = serd_env_new(base);
		SerdReader*     reader = serd_model_new_reader(
			world->model, env, SERD_TURTLE, serd_node_copy(dmanifest));
		serd_reader_add_blank_prefix(reader,
		                             lilv_world_blank_node_prefix(world));
		serd_reader_read_file_handle(reader, fd, "(dyn-manifest)");
		serd_reader_free(reader);
		serd_env_free(env);

		// Close (and automatically delete) temporary data file
		fclose(fd);

		// ?plugin a lv2:Plugin
		SerdModel* plugins = lilv_world_filter_model(world,
		                                             world->model,
		                                             NULL,
		                                             world->uris.rdf_a,
		                                             world->uris.lv2_Plugin,
		                                             dmanifest);
		SerdIter* p = serd_begin(plugins);
		FOREACH_MATCH(s, p) {
			const SerdNode* plug = serd_statement_get_subject(s);
			lilv_world_add_plugin(world, plug, manifest, desc, bundle_node);
		}
		if (desc->refs == 0) {
			free(desc);
		}
		serd_iter_free(p);
		serd_free(plugins);
		lilv_free(lib_path);
	}
	serd_iter_free(iter);
	serd_free(model);
#endif  // LILV_DYN_MANIFEST
}

LilvNode*
lilv_world_get_manifest_uri(LilvWorld* world, const LilvNode* bundle_uri)
{
	return serd_new_resolved_uri("manifest.ttl", bundle_uri);
}

static SerdModel*
load_plugin_model(LilvWorld*      world,
                  const LilvNode* bundle_uri,
                  const LilvNode* plugin_uri)
{
	// Create model and reader for loading into it
	SerdModel*    model       = serd_model_new(world->world, SERD_INDEX_SPO | SERD_INDEX_OPS);
	SerdEnv*      env         = serd_env_new(bundle_uri);
	SerdInserter* inserter    = serd_inserter_new(model, env, NULL);
	SerdReader*   reader      =
		serd_reader_new(world->world,
		                SERD_TURTLE,
		                0,
		                serd_inserter_get_sink(inserter),
		                LILV_READER_STACK_SIZE);

	// Load manifest
	LilvNode* manifest_uri = lilv_world_get_manifest_uri(world, bundle_uri);
	serd_reader_add_blank_prefix(reader, lilv_world_blank_node_prefix(world));
	serd_reader_start_file(reader, lilv_node_as_string(manifest_uri), true);
	serd_reader_read_document(reader);
	serd_reader_finish(reader);

	// Load any seeAlso files
	SerdModel* files = lilv_world_filter_model(
		world, model, plugin_uri, world->uris.rdfs_seeAlso, NULL, NULL);

	SerdIter* f = serd_model_begin(files);
	for (; !serd_iter_equals(f, serd_model_end(files)); serd_iter_next(f)) {
		const SerdNode* file     = serd_statement_get_object(serd_iter_get(f));
		const char*     file_str = serd_node_get_string(file);
		if (serd_node_get_type(file) == SERD_URI) {
			serd_reader_add_blank_prefix(
				reader, lilv_world_blank_node_prefix(world));
			serd_reader_start_file(reader, file_str, true);
			serd_reader_read_document(reader);
			serd_reader_finish(reader);
		}
	}

	serd_iter_free(f);
	serd_model_free(files);
	serd_reader_free(reader);
	serd_env_free(env);
	lilv_node_free(manifest_uri);

	return model;
}

static LilvVersion
get_version(LilvWorld* world, SerdModel* model, const LilvNode* subject)
{
	const SerdNode* minor_node = serd_model_get(
		model, subject, world->uris.lv2_minorVersion, NULL, NULL);
	const SerdNode* micro_node = serd_model_get(
		model, subject, world->uris.lv2_microVersion, NULL, NULL);

	LilvVersion version = { 0, 0 };
	if (minor_node && micro_node) {
		version.minor = atoi(serd_node_get_string(minor_node));
		version.micro = atoi(serd_node_get_string(micro_node));
	}

	return version;
}

LILV_API void
lilv_world_load_bundle(LilvWorld* world, const LilvNode* bundle_uri)
{
	if (!lilv_node_is_uri(bundle_uri)) {
		LILV_ERRORF("Bundle URI `%s' is not a URI\n",
		            serd_node_get_string(bundle_uri));
		return;
	}

	LilvNode* manifest = lilv_world_get_manifest_uri(world, bundle_uri);

	// Read manifest into model with graph = bundle_uri
	SerdStatus st = lilv_world_load_graph(world, bundle_uri, manifest);
	if (st > SERD_FAILURE) {
		LILV_ERRORF("Error reading %s\n", lilv_node_as_string(manifest));
		lilv_node_free(manifest);
		return;
	}

	// ?plugin a lv2:Plugin
	SerdRange* plug_results = serd_model_range(world->model,
	                                           NULL,
	                                           world->uris.rdf_a,
	                                           world->uris.lv2_Plugin,
	                                           bundle_uri);

	// Find any loaded plugins that will be replaced with a newer version
	LilvNodes* unload_uris = lilv_nodes_new();
	FOREACH_MATCH(s, plug_results) {
		const SerdNode* plug = serd_statement_get_subject(s);

		LilvNode*         plugin_uri  = serd_node_copy(plug);
		const LilvPlugin* plugin      = lilv_plugins_get_by_uri(world->plugins, plugin_uri);
		const LilvNode*   last_bundle = plugin ? lilv_plugin_get_bundle_uri(plugin) : NULL;
		if (!plugin || serd_node_equals(bundle_uri, last_bundle)) {
			// No previously loaded version, or it's from the same bundle
			lilv_node_free(plugin_uri);
			continue;
		}

		// Compare versions
		SerdModel*  this_model   = load_plugin_model(world, bundle_uri, plugin_uri);
		LilvVersion this_version = get_version(world, this_model, plugin_uri);
		SerdModel*  last_model   = load_plugin_model(world, last_bundle, plugin_uri);
		LilvVersion last_version = get_version(world, last_model, plugin_uri);
		serd_model_free(this_model);
		serd_model_free(last_model);
		const int cmp = lilv_version_cmp(&this_version, &last_version);
		if (cmp > 0) {
			zix_tree_insert((ZixTree*)unload_uris,
			                lilv_node_duplicate(plugin_uri),
			                NULL);
			LILV_WARNF("Replacing version %d.%d of <%s> from <%s>\n",
			           last_version.minor, last_version.micro,
			           serd_node_get_string(plug),
			           serd_node_get_string(last_bundle));
			LILV_NOTEF("New version %d.%d found in <%s>\n",
			           this_version.minor, this_version.micro,
			           serd_node_get_string(bundle_uri));
		} else if (cmp < 0) {
			LILV_WARNF("Ignoring bundle <%s>\n",
			           serd_node_get_string(bundle_uri));
			LILV_NOTEF("Newer version of <%s> loaded from <%s>\n",
			           serd_node_get_string(plug),
			           serd_node_get_string(last_bundle));
			lilv_node_free(plugin_uri);
			serd_range_free(plug_results);
			lilv_world_drop_graph(world, bundle_uri);
			lilv_node_free(manifest);
			lilv_nodes_free(unload_uris);
			return;
		}
		lilv_node_free(plugin_uri);
	}

	serd_range_free(plug_results);

	// Unload any old conflicting plugins
	LilvNodes* unload_bundles = lilv_nodes_new();
	LILV_FOREACH(nodes, i, unload_uris) {
		const LilvNode*   uri    = lilv_nodes_get(unload_uris, i);
		const LilvPlugin* plugin = lilv_plugins_get_by_uri(world->plugins, uri);
		const LilvNode*   bundle = lilv_plugin_get_bundle_uri(plugin);

		// Unload plugin and record bundle for later unloading
		lilv_world_unload_resource(world, uri);
		zix_tree_insert((ZixTree*)unload_bundles,
		                lilv_node_duplicate(bundle),
		                NULL);

	}
	lilv_nodes_free(unload_uris);

	// Now unload the associated bundles
	// This must be done last since several plugins could be in the same bundle
	LILV_FOREACH(nodes, i, unload_bundles) {
		lilv_world_unload_bundle(world, lilv_nodes_get(unload_bundles, i));
	}
	lilv_nodes_free(unload_bundles);

	// Re-search for plugin results now that old plugins are gone
	plug_results = serd_model_range(world->model,
	                                NULL,
	                                world->uris.rdf_a,
	                                world->uris.lv2_Plugin,
	                                bundle_uri);

	FOREACH_MATCH(s, plug_results) {
		const SerdNode* plug = serd_statement_get_subject(s);
		lilv_world_add_plugin(world, plug, manifest, NULL, bundle_uri);
	}
	serd_range_free(plug_results);

	lilv_world_load_dyn_manifest(world, bundle_uri, manifest);

	// ?spec a lv2:Specification
	// ?spec a owl:Ontology
	const SerdNode* spec_preds[] = { world->uris.lv2_Specification,
	                                 world->uris.owl_Ontology,
	                                 NULL };
	for (const SerdNode** p = spec_preds; *p; ++p) {
		FOREACH_PAT(s, world->model, NULL, world->uris.rdf_a, *p, bundle_uri) {
			const SerdNode* spec = serd_statement_get_subject(s);
			lilv_world_add_spec(world, spec, bundle_uri);
		}
	}

	lilv_node_free(manifest);
}

static int
lilv_world_drop_graph(LilvWorld* world, const SerdNode* graph)
{
	SerdRange*       r  = serd_model_range(world->model, NULL, NULL, NULL, graph);
	const SerdStatus st = serd_model_erase_range(world->model, r);

	if (st) {
		LILV_ERRORF("Error dropping graph <%s> (%s)\n",
		            serd_node_get_string(graph), serd_strerror(st));
		return st;
	}

	serd_range_free(r);
	return 0;
}

/** Remove loaded_files entry so file will be reloaded if requested. */
static int
lilv_world_unload_file(LilvWorld* world, const LilvNode* file)
{
	ZixTreeIter* iter;
	if (!zix_tree_find((ZixTree*)world->loaded_files, file, &iter)) {
		zix_tree_remove((ZixTree*)world->loaded_files, iter);
		return 0;
	}
	return 1;
}

LILV_API int
lilv_world_unload_bundle(LilvWorld* world, const LilvNode* bundle_uri)
{
	if (!bundle_uri) {
		return 0;
	}

	// Find all loaded files that are inside the bundle
	LilvNodes* files = lilv_nodes_new();
	LILV_FOREACH(nodes, i, world->loaded_files) {
		const LilvNode* file = lilv_nodes_get(world->loaded_files, i);
		if (!strncmp(lilv_node_as_string(file),
		             lilv_node_as_string(bundle_uri),
		             strlen(lilv_node_as_string(bundle_uri)))) {
			zix_tree_insert((ZixTree*)files,
			                lilv_node_duplicate(file),
			                NULL);
		}
	}

	// Unload all loaded files in the bundle
	LILV_FOREACH(nodes, i, files) {
		const LilvNode* file = lilv_nodes_get(world->plugins, i);
		lilv_world_unload_file(world, file);
	}

	lilv_nodes_free(files);

	/* Remove any plugins in the bundle from the plugin list.  Since the
	   application may still have a pointer to the LilvPlugin, it can not be
	   destroyed here.  Instead, we move it to the zombie plugin list, so it
	   will not be in the list returned by lilv_world_get_all_plugins() but can
	   still be used.
	*/
	ZixTreeIter* i = zix_tree_begin((ZixTree*)world->plugins);
	while (i != zix_tree_end((ZixTree*)world->plugins)) {
		LilvPlugin*  p    = (LilvPlugin*)zix_tree_get(i);
		ZixTreeIter* next = zix_tree_iter_next(i);

		if (lilv_node_equals(lilv_plugin_get_bundle_uri(p), bundle_uri)) {
			zix_tree_remove((ZixTree*)world->plugins, i);
			zix_tree_insert((ZixTree*)world->zombies, p, NULL);
		}

		i = next;
	}

	// Drop everything in bundle graph
	return lilv_world_drop_graph(world, bundle_uri);
}

static void
load_dir_entry(const char* dir, const char* name, void* data)
{
	LilvWorld* world = (LilvWorld*)data;
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		return;
	}

	char*     path = lilv_strjoin(dir, "/", name, "/", NULL);
	SerdNode* suri = serd_new_file_uri(path, 0);
	LilvNode* node = lilv_new_uri(world, serd_node_get_string(suri));

	lilv_world_load_bundle(world, node);
	lilv_node_free(node);
	serd_node_free(suri);
	free(path);
}

/** Load all bundles in the directory at `dir_path`. */
static void
lilv_world_load_directory(LilvWorld* world, const char* dir_path)
{
	char* path = lilv_expand(dir_path);
	if (path) {
		lilv_dir_for_each(path, world, load_dir_entry);
		free(path);
	}
}

static const char*
first_path_sep(const char* path)
{
	for (const char* p = path; *p != '\0'; ++p) {
		if (*p == LILV_PATH_SEP[0]) {
			return p;
		}
	}
	return NULL;
}

/** Load all bundles found in `lv2_path`.
 * @param lv2_path A colon-delimited list of directories.  These directories
 * should contain LV2 bundle directories (ie the search path is a list of
 * parent directories of bundles, not a list of bundle directories).
 */
static void
lilv_world_load_path(LilvWorld*  world,
                     const char* lv2_path)
{
	while (lv2_path[0] != '\0') {
		const char* const sep = first_path_sep(lv2_path);
		if (sep) {
			const size_t dir_len = sep - lv2_path;
			char* const  dir     = (char*)malloc(dir_len + 1);
			memcpy(dir, lv2_path, dir_len);
			dir[dir_len] = '\0';
			lilv_world_load_directory(world, dir);
			free(dir);
			lv2_path += dir_len + 1;
		} else {
			lilv_world_load_directory(world, lv2_path);
			lv2_path = "\0";
		}
	}
}

void
lilv_world_load_specifications(LilvWorld* world)
{
	for (LilvSpec* spec = world->specs; spec; spec = spec->next) {
		LILV_FOREACH(nodes, f, spec->data_uris) {
			LilvNode* file = (LilvNode*)lilv_collection_get(spec->data_uris, f);
			lilv_world_load_graph(world, NULL, file);
		}
	}
}

void
lilv_world_load_plugin_classes(LilvWorld* world)
{
	/* FIXME: This loads all classes, not just lv2:Plugin subclasses.
	   However, if the host gets all the classes via lilv_plugin_class_get_children
	   starting with lv2:Plugin as the root (which is e.g. how a host would build
	   a menu), they won't be seen anyway...
	*/

	FOREACH_PAT(s,
	            world->model,
	            NULL,
	            world->uris.rdf_a,
	            world->uris.rdfs_Class,
	            NULL)
	{
		const SerdNode* class_node = serd_statement_get_subject(s);

		const SerdNode* parent = serd_model_get(
			world->model, class_node, world->uris.rdfs_subClassOf, NULL, NULL);
		if (!parent || serd_node_get_type(parent) != SERD_URI) {
			continue;
		}

		const SerdNode* label = serd_model_get(
			world->model, class_node, world->uris.rdfs_label, NULL, NULL);
		if (!label) {
			continue;
		}

		LilvPluginClass* pclass = lilv_plugin_class_new(
			world, parent, class_node,
			serd_node_get_string(label));
		if (pclass) {
			zix_tree_insert((ZixTree*)world->plugin_classes, pclass, NULL);
		}
	}
}

LILV_API void
lilv_world_load_all(LilvWorld* world)
{
	const char* lv2_path = world->opt.lv2_path;
	if (!lv2_path) {
		lv2_path = getenv("LV2_PATH");
	}
	if (!lv2_path) {
		lv2_path = LILV_DEFAULT_LV2_PATH;
	}

	// Discover bundles and read all manifest files into model
	lilv_world_load_path(world, lv2_path);

	LILV_FOREACH(plugins, p, world->plugins) {
		const LilvPlugin* plugin = (const LilvPlugin*)lilv_collection_get(
			(ZixTree*)world->plugins, p);

		// ?new dc:replaces plugin
		if (serd_model_ask(world->model,
		                   NULL,
		                   world->uris.dc_replaces,
		                   lilv_plugin_get_uri(plugin),
		                   NULL)) {
			// TODO: Check if replacement is a known plugin? (expensive)
			((LilvPlugin*)plugin)->replaced = true;
		}
	}

	// Query out things to cache
	lilv_world_load_specifications(world);
	lilv_world_load_plugin_classes(world);
}

SerdStatus
lilv_world_load_file(LilvWorld* world, SerdReader* reader, const LilvNode* uri)
{
	ZixTreeIter* iter;
	if (!zix_tree_find((ZixTree*)world->loaded_files, uri, &iter)) {
		return SERD_FAILURE;  // File has already been loaded
	}

	const char* const uri_str = serd_node_get_string(uri);
	const size_t      uri_len = serd_node_get_length(uri);
	if (strncmp(uri_str, "file:", 5)) {
		return SERD_FAILURE;  // Not a local file
	} else if (strcmp(uri_str + uri_len - 4, ".ttl")) {
		return SERD_FAILURE;  // Not a Turtle file
	}

	serd_reader_add_blank_prefix(reader, lilv_world_blank_node_prefix(world));
	SerdStatus st = SERD_SUCCESS;
	if ((st = serd_reader_start_file(reader, uri_str, true)) ||
	    (st = serd_reader_read_document(reader)) ||
	    (st = serd_reader_finish(reader))) {
		LILV_ERRORF("Error loading file `%s'\n", lilv_node_as_string(uri));
		return st;
	}

	zix_tree_insert((ZixTree*)world->loaded_files,
	                lilv_node_duplicate(uri),
	                NULL);
	return SERD_SUCCESS;
}

LILV_API int
lilv_world_load_resource(LilvWorld*      world,
                         const LilvNode* resource)
{
	if (!lilv_node_is_uri(resource) && !lilv_node_is_blank(resource)) {
		LILV_ERRORF("Node `%s' is not a resource\n",
		            serd_node_get_string(resource));
		return -1;
	}

	SerdModel* files = lilv_world_filter_model(world,
	                                           world->model,
	                                           resource,
	                                           world->uris.rdfs_seeAlso,
	                                           NULL, NULL);

	SerdIter* f      = serd_model_begin(files);
	int       n_read = 0;
	for (; !serd_iter_equals(f, serd_model_end(files)); serd_iter_next(f)) {
		const SerdNode* file      = serd_statement_get_object(serd_iter_get(f));
		const char*     file_str  = serd_node_get_string(file);
		LilvNode*       file_node = serd_node_copy(file);
		if (serd_node_get_type(file) != SERD_URI) {
			LILV_ERRORF("rdfs:seeAlso node `%s' is not a URI\n", file_str);
		} else if (!lilv_world_load_graph(world, (SerdNode*)file, file_node)) {
			++n_read;
		}
		lilv_node_free(file_node);
	}
	serd_iter_free(f);

	serd_model_free(files);
	return n_read;
}

LILV_API int
lilv_world_unload_resource(LilvWorld*      world,
                           const LilvNode* resource)
{
	if (!lilv_node_is_uri(resource) && !lilv_node_is_blank(resource)) {
		LILV_ERRORF("Node `%s' is not a resource\n",
		            serd_node_get_string(resource));
		return -1;
	}

	SerdModel* files = lilv_world_filter_model(world,
	                                           world->model,
	                                           resource,
	                                           world->uris.rdfs_seeAlso,
	                                           NULL, NULL);

	SerdIter* f         = serd_model_begin(files);
	int       n_dropped = 0;
	for (; !serd_iter_equals(f, serd_model_end(files)); serd_iter_next(f)) {
		const SerdNode* file      = serd_statement_get_object(serd_iter_get(f));
		LilvNode*       file_node = serd_node_copy(file);
		if (serd_node_get_type(file) != SERD_URI) {
			LILV_ERRORF("rdfs:seeAlso node `%s' is not a URI\n",
			            serd_node_get_string(file));
		} else if (!lilv_world_drop_graph(world, file_node)) {
			lilv_world_unload_file(world, file_node);
			++n_dropped;
		}
		lilv_node_free(file_node);
	}
	serd_iter_free(f);

	serd_model_free(files);
	return n_dropped;
}

LILV_API const LilvPluginClass*
lilv_world_get_plugin_class(const LilvWorld* world)
{
	return world->lv2_plugin_class;
}

LILV_API const LilvPluginClasses*
lilv_world_get_plugin_classes(const LilvWorld* world)
{
	return world->plugin_classes;
}

LILV_API const LilvPlugins*
lilv_world_get_all_plugins(const LilvWorld* world)
{
	return world->plugins;
}

LILV_API LilvNode*
lilv_world_get_symbol(LilvWorld* world, const LilvNode* subject)
{
	// Check for explicitly given symbol
	const SerdNode* snode = serd_model_get(
		world->model, subject, world->uris.lv2_symbol, NULL, NULL);

	if (snode) {
		return serd_node_copy(snode);
	}

	if (!lilv_node_is_uri(subject)) {
		return NULL;
	}

	// Find rightmost segment of URI
	SerdURI uri;
	serd_uri_parse(lilv_node_as_uri(subject), &uri);
	const char* str = "_";
	if (uri.fragment.buf) {
		str = uri.fragment.buf + 1;
	} else if (uri.query.buf) {
		str = uri.query.buf;
	} else if (uri.path.buf) {
		const char* last_slash = strrchr(uri.path.buf, '/');
		str = last_slash ? (last_slash + 1) : uri.path.buf;
	}

	// Replace invalid characters
	const size_t len = strlen(str);
	char* const  sym = (char*)calloc(1, len + 1);
	for (size_t i = 0; i < len; ++i) {
		const char c = str[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c == '_') || (i > 0 && c >= '0' && c <= '9'))) {
			sym[i] = '_';
		} else {
			sym[i] = str[i];
		}
	}

	LilvNode* ret = lilv_new_string(world, sym);
	free(sym);
	return ret;
}
