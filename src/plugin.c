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

#define __STDC_LIMIT_MACROS

#include "lilv_config.h"
#include "lilv_internal.h"

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "zix/tree.h"

#include "lv2/core/lv2.h"
#include "lv2/ui/ui.h"

#ifdef LILV_DYN_MANIFEST
#    include "lv2/dynmanifest/dynmanifest.h"
#    include <dlfcn.h>
#endif

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_DOAP "http://usefulinc.com/ns/doap#"
#define NS_FOAF "http://xmlns.com/foaf/0.1/"
#define NS_LILV "http://drobilla.net/ns/lilv#"
#define NS_LV2  "http://lv2plug.in/ns/lv2core#"
#define NS_OWL  "http://www.w3.org/2002/07/owl#"
#define NS_RDF  "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_RDFS "http://www.w3.org/2000/01/rdf-schema#"
#define NS_XSD  "http://www.w3.org/2001/XMLSchema#"

static void
lilv_plugin_init(LilvPlugin* plugin, const LilvNode* bundle_uri)
{
	plugin->bundle_uri   = lilv_node_duplicate(bundle_uri);
	plugin->binary_uri   = NULL;
#ifdef LILV_DYN_MANIFEST
	plugin->dynmanifest  = NULL;
#endif
	plugin->plugin_class = NULL;
	plugin->data_uris    = lilv_nodes_new();
	plugin->ports        = NULL;
	plugin->num_ports    = 0;
	plugin->loaded       = false;
	plugin->parse_errors = false;
	plugin->replaced     = false;
}

LilvPlugin*
lilv_plugin_new(LilvWorld*      world,
                const LilvNode* uri,
                const LilvNode* bundle_uri)
{
	LilvPlugin* plugin = (LilvPlugin*)malloc(sizeof(LilvPlugin));

	plugin->world      = world;
	plugin->plugin_uri = lilv_node_duplicate(uri);

	lilv_plugin_init(plugin, bundle_uri);
	return plugin;
}

void
lilv_plugin_clear(LilvPlugin* plugin, const LilvNode* bundle_uri)
{
	lilv_node_free(plugin->bundle_uri);
	lilv_node_free(plugin->binary_uri);
	lilv_nodes_free(plugin->data_uris);
	lilv_plugin_init(plugin, bundle_uri);
}

static void
lilv_plugin_free_ports(LilvPlugin* plugin)
{
	if (plugin->ports) {
		for (uint32_t i = 0; i < plugin->num_ports; ++i) {
			lilv_port_free(plugin, plugin->ports[i]);
		}
		free(plugin->ports);
		plugin->num_ports = 0;
		plugin->ports     = NULL;
	}
}

void
lilv_plugin_free(LilvPlugin* plugin)
{
#ifdef LILV_DYN_MANIFEST
	if (plugin->dynmanifest && --plugin->dynmanifest->refs == 0) {
		typedef int (*CloseFunc)(LV2_Dyn_Manifest_Handle);
		CloseFunc close_func = (CloseFunc)lilv_dlfunc(plugin->dynmanifest->lib,
		                                              "lv2_dyn_manifest_close");
		if (close_func) {
			close_func(plugin->dynmanifest->handle);
		}

		dlclose(plugin->dynmanifest->lib);
		lilv_node_free(plugin->dynmanifest->bundle);
		free(plugin->dynmanifest);
	}
#endif

	lilv_node_free(plugin->plugin_uri);
	plugin->plugin_uri = NULL;

	lilv_node_free(plugin->bundle_uri);
	plugin->bundle_uri = NULL;

	lilv_node_free(plugin->binary_uri);
	plugin->binary_uri = NULL;

	lilv_plugin_free_ports(plugin);

	lilv_nodes_free(plugin->data_uris);
	plugin->data_uris = NULL;

	free(plugin);
}

static LilvNode*
lilv_plugin_get_one(const LilvPlugin* plugin,
                    const SerdNode*   subject,
                    const SerdNode*   predicate)
{
	return serd_node_copy(
		serd_model_get(plugin->world->model, subject, predicate, NULL, NULL));
}

LilvNode*
lilv_plugin_get_unique(const LilvPlugin* plugin,
                       const SerdNode*   subject,
                       const SerdNode*   predicate)
{
	LilvNode* ret = lilv_plugin_get_one(plugin, subject, predicate);
	if (!ret) {
		LILV_ERRORF("No value found for (%s %s ...) property\n",
		            serd_node_get_string(subject),
		            serd_node_get_string(predicate));
	}
	return ret;
}

static void
lilv_plugin_load(LilvPlugin* plugin)
{
	SerdNode* bundle_uri_node = plugin->bundle_uri;

	SerdEnv*      env = serd_env_new(bundle_uri_node);
	SerdInserter* inserter =
		serd_inserter_new(plugin->world->model, env, bundle_uri_node);

	SerdReader* reader =
		serd_reader_new(plugin->world->world,
		                SERD_TURTLE,
		                0,
		                serd_inserter_get_sink(inserter),
		                LILV_READER_STACK_SIZE);

	SerdModel* prots = lilv_world_filter_model(
		plugin->world,
		plugin->world->model,
		plugin->plugin_uri,
		plugin->world->uris.lv2_prototype,
		NULL, NULL);
	SerdModel* skel = serd_model_new(plugin->world->world, SERD_INDEX_SPO);
	SerdIter*  iter = serd_model_begin(prots);
	for (; !serd_iter_equals(iter, serd_model_end(prots));
	     serd_iter_next(iter)) {
		const SerdStatement* statement = serd_iter_get(iter);
		const SerdNode*      t         = serd_statement_get_object(statement);
		LilvNode*            prototype = serd_node_copy(t);

		lilv_world_load_resource(plugin->world, prototype);

		FOREACH_PAT(s, plugin->world->model, prototype, NULL, NULL, NULL) {
			serd_model_add(skel,
			               plugin->plugin_uri,
			               serd_statement_get_predicate(s),
			               serd_statement_get_object(s),
			               NULL);
		}

		lilv_node_free(prototype);
	}
	serd_iter_free(iter);

	SerdRange* all = serd_model_all(skel);
	serd_model_add_range(plugin->world->model, all);
	serd_range_free(all);

	serd_model_free(skel);
	serd_model_free(prots);

	// Parse all the plugin's data files into RDF model
	SerdStatus st = SERD_SUCCESS;
	LILV_FOREACH(nodes, i, plugin->data_uris) {
		const LilvNode* data_uri = lilv_nodes_get(plugin->data_uris, i);

		serd_env_set_base_uri(env, data_uri);
		st = lilv_world_load_file(plugin->world, reader, data_uri);
		if (st > SERD_FAILURE) {
			break;
		}
	}

	if (st > SERD_FAILURE) {
		plugin->loaded       = true;
		plugin->parse_errors = true;
		serd_reader_free(reader);
		serd_inserter_free(inserter);
		serd_env_free(env);
		return;
	}

#ifdef LILV_DYN_MANIFEST
	// Load and parse dynamic manifest data, if this is a library
	if (plugin->dynmanifest) {
		typedef int (*GetDataFunc)(LV2_Dyn_Manifest_Handle handle,
		                           FILE*                   fp,
		                           const char*             uri);
		GetDataFunc get_data_func = (GetDataFunc)lilv_dlfunc(
			plugin->dynmanifest->lib, "lv2_dyn_manifest_get_data");
		if (get_data_func) {
			const SerdNode* bundle = plugin->dynmanifest->bundle;
			serd_env_set_base_uri(env, bundle);
			FILE* fd = tmpfile();
			get_data_func(plugin->dynmanifest->handle, fd,
			              lilv_node_as_string(plugin->plugin_uri));
			rewind(fd);
			serd_reader_add_blank_prefix(
				reader, lilv_world_blank_node_prefix(plugin->world));
			serd_reader_read_file_handle(reader, fd, "(dyn-manifest)");
			fclose(fd);
		}
	}
#endif
	serd_reader_free(reader);
	serd_inserter_free(inserter);
	serd_env_free(env);

	plugin->loaded = true;
}

static bool
is_symbol(const char* str)
{
	for (const char* s = str; *s; ++s) {
		if (!((*s >= 'a' && *s <= 'z') ||
		      (*s >= 'A' && *s <= 'Z') ||
		      (s > str && *s >= '0' && *s <= '9') ||
		      *s == '_')) {
			return false;
		}
	}
	return true;
}

static void
lilv_plugin_load_ports_if_necessary(const LilvPlugin* const_plugin)
{
	LilvPlugin* plugin = (LilvPlugin*)const_plugin;

	lilv_plugin_load_if_necessary(plugin);

	if (!plugin->ports) {
		plugin->ports = (LilvPort**)malloc(sizeof(LilvPort*));
		plugin->ports[0] = NULL;

		SerdRange* ports = serd_model_range(
			plugin->world->model,
			plugin->plugin_uri,
			plugin->world->uris.lv2_port,
			NULL,
			NULL);

		FOREACH_MATCH(s, ports) {
			const SerdNode* port = serd_statement_get_object(s);
			LilvNode* index = lilv_plugin_get_unique(
				plugin, port, plugin->world->uris.lv2_index);
			LilvNode* symbol = lilv_plugin_get_unique(
				plugin, port, plugin->world->uris.lv2_symbol);

			if (!lilv_node_is_string(symbol) ||
			    !is_symbol(serd_node_get_string(symbol))) {
				LILV_ERRORF("Plugin <%s> port symbol `%s' is invalid\n",
				            lilv_node_as_uri(plugin->plugin_uri),
				            lilv_node_as_string(symbol));
				lilv_node_free(symbol);
				lilv_node_free(index);
				lilv_plugin_free_ports(plugin);
				break;
			}

			if (!lilv_node_is_int(index)) {
				LILV_ERRORF("Plugin <%s> port index is not an integer\n",
				            lilv_node_as_uri(plugin->plugin_uri));
				lilv_node_free(symbol);
				lilv_node_free(index);
				lilv_plugin_free_ports(plugin);
				break;
			}

			uint32_t  this_index = lilv_node_as_int(index);
			LilvPort* this_port  = NULL;
			if (plugin->num_ports > this_index) {
				this_port = plugin->ports[this_index];
			} else {
				plugin->ports = (LilvPort**)realloc(
					plugin->ports, (this_index + 1) * sizeof(LilvPort*));
				memset(plugin->ports + plugin->num_ports, '\0',
				       (this_index - plugin->num_ports) * sizeof(LilvPort*));
				plugin->num_ports = this_index + 1;
			}

			// Havn't seen this port yet, add it to array
			if (!this_port) {
				this_port = lilv_port_new(plugin->world,
				                          port,
				                          this_index,
				                          lilv_node_as_string(symbol));
				plugin->ports[this_index] = this_port;
			}

			SerdRange* types = serd_model_range(plugin->world->model,
			                                    port,
			                                    plugin->world->uris.rdf_a,
			                                    NULL,
			                                    NULL);
			FOREACH_MATCH(t, types)
			{
				const SerdNode* type = serd_statement_get_object(t);
				if (serd_node_get_type(type) == SERD_URI) {
					zix_tree_insert((ZixTree*)this_port->classes,
					                serd_node_copy(type),
					                NULL);
				} else {
					LILV_WARNF("Plugin <%s> port type is not a URI\n",
					           lilv_node_as_uri(plugin->plugin_uri));
				}
			}
			serd_range_free(types);

			lilv_node_free(symbol);
			lilv_node_free(index);
		}
		serd_range_free(ports);

		// Check sanity
		for (uint32_t i = 0; i < plugin->num_ports; ++i) {
			if (!plugin->ports[i]) {
				LILV_ERRORF("Plugin <%s> is missing port %d/%d\n",
				            lilv_node_as_uri(plugin->plugin_uri), i, plugin->num_ports);
				lilv_plugin_free_ports(plugin);
				break;
			}
		}
	}
}

void
lilv_plugin_load_if_necessary(const LilvPlugin* plugin)
{
	if (!plugin->loaded) {
		lilv_plugin_load((LilvPlugin*)plugin);
	}
}

LILV_API const LilvNode*
lilv_plugin_get_uri(const LilvPlugin* plugin)
{
	return plugin->plugin_uri;
}

LILV_API const LilvNode*
lilv_plugin_get_bundle_uri(const LilvPlugin* plugin)
{
	return plugin->bundle_uri;
}

LILV_API const LilvNode*
lilv_plugin_get_library_uri(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary((LilvPlugin*)plugin);
	if (!plugin->binary_uri) {
		// <plugin> lv2:binary ?binary
		SerdRange* i = serd_model_range(plugin->world->model,
		                                plugin->plugin_uri,
		                                plugin->world->uris.lv2_binary,
		                                NULL,
		                                NULL);
		FOREACH_MATCH(s, i) {
			const SerdNode* binary_node = serd_statement_get_object(s);
			if (serd_node_get_type(binary_node) == SERD_URI) {
				((LilvPlugin*)plugin)->binary_uri = serd_node_copy(binary_node);
				break;
			}
		}
		serd_range_free(i);
	}
	if (!plugin->binary_uri) {
		LILV_WARNF("Plugin <%s> has no lv2:binary\n",
		           lilv_node_as_uri(lilv_plugin_get_uri(plugin)));
	}
	return plugin->binary_uri;
}

LILV_API const LilvNodes*
lilv_plugin_get_data_uris(const LilvPlugin* plugin)
{
	return plugin->data_uris;
}

LILV_API const LilvPluginClass*
lilv_plugin_get_class(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary((LilvPlugin*)plugin);
	if (!plugin->plugin_class) {
		// <plugin> a ?class
		SerdRange* c = serd_model_range(plugin->world->model,
		                                plugin->plugin_uri,
		                                plugin->world->uris.rdf_a,
		                                NULL,
		                                NULL);
		FOREACH_MATCH(s, c) {
			const SerdNode* class_node = serd_statement_get_object(s);
			if (serd_node_get_type(class_node) != SERD_URI) {
				continue;
			}

			LilvNode* klass = serd_node_copy(class_node);
			if (!lilv_node_equals(klass, plugin->world->lv2_plugin_class->uri)) {
				const LilvPluginClass* pclass = lilv_plugin_classes_get_by_uri(
					plugin->world->plugin_classes, klass);

				if (pclass) {
					((LilvPlugin*)plugin)->plugin_class = pclass;
					lilv_node_free(klass);
					break;
				}
			}

			lilv_node_free(klass);
		}
		serd_range_free(c);

		if (plugin->plugin_class == NULL) {
			((LilvPlugin*)plugin)->plugin_class =
				plugin->world->lv2_plugin_class;
		}
	}
	return plugin->plugin_class;
}

static LilvNodes*
lilv_plugin_get_value_internal(const LilvPlugin* plugin,
                               const SerdNode*   predicate)
{
	lilv_plugin_load_if_necessary(plugin);
	return lilv_world_find_nodes_internal(
		plugin->world, plugin->plugin_uri, predicate, NULL);
}

LILV_API bool
lilv_plugin_verify(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);
	if (plugin->parse_errors) {
		return false;
	}

	LilvNodes* results =
		lilv_plugin_get_value(plugin, plugin->world->uris.rdf_a);
	if (!results) {
		return false;
	}

	lilv_nodes_free(results);
	results = lilv_plugin_get_value_internal(plugin,
	                                         plugin->world->uris.doap_name);
	if (!results) {
		return false;
	}

	lilv_nodes_free(results);
	LilvNode* lv2_port = lilv_new_uri(plugin->world, LV2_CORE__port);
	results = lilv_plugin_get_value(plugin, lv2_port);
	lilv_node_free(lv2_port);
	if (!results) {
		return false;
	}

	lilv_nodes_free(results);
	return true;
}

LILV_API LilvNode*
lilv_plugin_get_name(const LilvPlugin* plugin)
{
	LilvNodes* results = lilv_plugin_get_value_internal(
		plugin, plugin->world->uris.doap_name);

	LilvNode* ret = NULL;
	if (results) {
		LilvNode* val = lilv_nodes_get_first(results);
		if (lilv_node_is_string(val)) {
			ret = lilv_node_duplicate(val);
		}
		lilv_nodes_free(results);
	}

	if (!ret) {
		LILV_WARNF("Plugin <%s> has no (mandatory) doap:name\n",
		           lilv_node_as_string(lilv_plugin_get_uri(plugin)));
	}

	return ret;
}

LILV_API LilvNodes*
lilv_plugin_get_value(const LilvPlugin* plugin,
                      const LilvNode*   predicate)
{
	lilv_plugin_load_if_necessary(plugin);
	return lilv_world_find_nodes(plugin->world, plugin->plugin_uri, predicate, NULL);
}

LILV_API uint32_t
lilv_plugin_get_num_ports(const LilvPlugin* plugin)
{
	lilv_plugin_load_ports_if_necessary(plugin);
	return plugin->num_ports;
}

LILV_API void
lilv_plugin_get_port_ranges_float(const LilvPlugin* plugin,
                                  float*            min_values,
                                  float*            max_values,
                                  float*            def_values)
{
	lilv_plugin_load_ports_if_necessary(plugin);
	LilvNode*  min    = NULL;
	LilvNode*  max    = NULL;
	LilvNode*  def    = NULL;
	LilvNode** minptr = min_values ? &min : NULL;
	LilvNode** maxptr = max_values ? &max : NULL;
	LilvNode** defptr = def_values ? &def : NULL;

	for (uint32_t i = 0; i < plugin->num_ports; ++i) {
		lilv_port_get_range(plugin, plugin->ports[i], defptr, minptr, maxptr);

		if (min_values) {
			if (lilv_node_is_float(min) || lilv_node_is_int(min)) {
				min_values[i] = lilv_node_as_float(min);
			} else {
				min_values[i] = NAN;
			}
		}

		if (max_values) {
			if (lilv_node_is_float(max) || lilv_node_is_int(max)) {
				max_values[i] = lilv_node_as_float(max);
			} else {
				max_values[i] = NAN;
			}
		}

		if (def_values) {
			if (lilv_node_is_float(def) || lilv_node_is_int(def)) {
				def_values[i] = lilv_node_as_float(def);
			} else {
				def_values[i] = NAN;
			}
		}

		lilv_node_free(def);
		lilv_node_free(min);
		lilv_node_free(max);
	}
}

LILV_API uint32_t
lilv_plugin_get_num_ports_of_class_va(const LilvPlugin* plugin,
                                      const LilvNode*   class_1,
                                      va_list           args)
{
	lilv_plugin_load_ports_if_necessary(plugin);

	uint32_t count = 0;

	// Build array of classes from args so we can walk it several times
	size_t           n_classes = 0;
	const LilvNode** classes   = NULL;
	for (LilvNode* c = NULL; (c = va_arg(args, LilvNode*)); ) {
		classes = (const LilvNode**)realloc(
			classes, ++n_classes * sizeof(LilvNode*));
		classes[n_classes - 1] = c;
	}

	// Check each port against every type
	for (unsigned i = 0; i < plugin->num_ports; ++i) {
		LilvPort* port = plugin->ports[i];
		if (port && lilv_port_is_a(plugin, port, class_1)) {
			bool matches = true;
			for (size_t j = 0; j < n_classes; ++j) {
				if (!lilv_port_is_a(plugin, port, classes[j])) {
					matches = false;
					break;
				}
			}

			if (matches) {
				++count;
			}
		}
	}

	free(classes);
	return count;
}

LILV_API uint32_t
lilv_plugin_get_num_ports_of_class(const LilvPlugin* plugin,
                                   const LilvNode*   class_1, ...)
{
	va_list args;
	va_start(args, class_1);

	uint32_t count = lilv_plugin_get_num_ports_of_class_va(plugin, class_1, args);

	va_end(args);
	return count;
}

LILV_API bool
lilv_plugin_has_latency(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);

	SerdRange* ports = serd_model_range(
		plugin->world->model,
		plugin->plugin_uri,
		plugin->world->uris.lv2_port,
		NULL,
		NULL);

	bool ret = false;
	FOREACH_MATCH(s, ports) {
		const SerdNode* port = serd_statement_get_object(s);
		if (serd_model_ask(plugin->world->model,
		                   port,
		                   plugin->world->uris.lv2_portProperty,
		                   plugin->world->uris.lv2_reportsLatency,
		                   NULL) ||
		    serd_model_ask(plugin->world->model,
		                   port,
		                   plugin->world->uris.lv2_designation,
		                   plugin->world->uris.lv2_latency,
		                   NULL)) {
			ret = true;
			break;
		}
	}
	serd_range_free(ports);

	return ret;
}

static const LilvPort*
lilv_plugin_get_port_by_property(const LilvPlugin* plugin,
                                 const SerdNode*   port_property)
{
	lilv_plugin_load_ports_if_necessary(plugin);
	for (uint32_t i = 0; i < plugin->num_ports; ++i) {
		LilvPort* port = plugin->ports[i];
		if (serd_model_ask(plugin->world->model,
		                   port->node,
		                   plugin->world->uris.lv2_portProperty,
		                   port_property,
		                   NULL)) {
			return port;
		}
	}

	return NULL;
}

LILV_API const LilvPort*
lilv_plugin_get_port_by_designation(const LilvPlugin* plugin,
                                    const LilvNode*   port_class,
                                    const LilvNode*   designation)
{
	LilvWorld* world = plugin->world;
	lilv_plugin_load_ports_if_necessary(plugin);
	for (uint32_t i = 0; i < plugin->num_ports; ++i) {
		LilvPort*  port            = plugin->ports[i];
		const bool has_designation = serd_model_ask(world->model,
		                                            port->node,
		                                            world->uris.lv2_designation,
		                                            designation,
		                                            NULL);

		if (has_designation &&
		    (!port_class || lilv_port_is_a(plugin, port, port_class))) {
			return port;
		}
	}

	return NULL;
}

LILV_API uint32_t
lilv_plugin_get_latency_port_index(const LilvPlugin* plugin)
{
	LilvNode* lv2_OutputPort =
		lilv_new_uri(plugin->world, LV2_CORE__OutputPort);
	LilvNode* lv2_latency =
		lilv_new_uri(plugin->world, LV2_CORE__latency);

	const LilvPort* prop_port = lilv_plugin_get_port_by_property(
		plugin, plugin->world->uris.lv2_reportsLatency);
	const LilvPort* des_port = lilv_plugin_get_port_by_designation(
		plugin, lv2_OutputPort, lv2_latency);

	lilv_node_free(lv2_latency);
	lilv_node_free(lv2_OutputPort);

	if (prop_port) {
		return prop_port->index;
	} else if (des_port) {
		return des_port->index;
	} else {
		return (uint32_t)-1;
	}
}

LILV_API bool
lilv_plugin_has_feature(const LilvPlugin* plugin,
                        const LilvNode*   feature)
{
	lilv_plugin_load_if_necessary(plugin);
	const SerdNode* predicates[] = { plugin->world->uris.lv2_requiredFeature,
	                                 plugin->world->uris.lv2_optionalFeature,
	                                 NULL };

	for (const SerdNode** pred = predicates; *pred; ++pred) {
		if (serd_model_ask(plugin->world->model,
		                   plugin->plugin_uri,
		                   *pred,
		                   feature,
		                   NULL)) {
			return true;
		}
	}
	return false;
}

LILV_API LilvNodes*
lilv_plugin_get_supported_features(const LilvPlugin* plugin)
{
	LilvNodes* optional = lilv_plugin_get_optional_features(plugin);
	LilvNodes* required = lilv_plugin_get_required_features(plugin);
	LilvNodes* result   = lilv_nodes_merge(optional, required);
	lilv_nodes_free(optional);
	lilv_nodes_free(required);
	return result;
}

LILV_API LilvNodes*
lilv_plugin_get_optional_features(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);
	return lilv_world_find_nodes_internal(plugin->world,
	                                      plugin->plugin_uri,
	                                      plugin->world->uris.lv2_optionalFeature,
	                                      NULL);
}

LILV_API LilvNodes*
lilv_plugin_get_required_features(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);
	return lilv_world_find_nodes_internal(plugin->world,
	                                      plugin->plugin_uri,
	                                      plugin->world->uris.lv2_requiredFeature,
	                                      NULL);
}

LILV_API bool
lilv_plugin_has_extension_data(const LilvPlugin* plugin,
                               const LilvNode*   uri)
{
	if (!lilv_node_is_uri(uri)) {
		LILV_ERRORF("Extension data `%s' is not a URI\n",
		            serd_node_get_string(uri));
		return false;
	}

	lilv_plugin_load_if_necessary(plugin);
	return serd_model_ask(plugin->world->model,
	                      plugin->plugin_uri,
	                      plugin->world->uris.lv2_extensionData,
	                      uri,
	                      NULL);
}

LILV_API LilvNodes*
lilv_plugin_get_extension_data(const LilvPlugin* plugin)
{
	return lilv_plugin_get_value_internal(plugin, plugin->world->uris.lv2_extensionData);
}

LILV_API const LilvPort*
lilv_plugin_get_port_by_index(const LilvPlugin* plugin,
                              uint32_t          index)
{
	lilv_plugin_load_ports_if_necessary(plugin);
	if (index < plugin->num_ports) {
		return plugin->ports[index];
	} else {
		return NULL;
	}
}

LILV_API const LilvPort*
lilv_plugin_get_port_by_symbol(const LilvPlugin* plugin,
                               const LilvNode*   symbol)
{
	lilv_plugin_load_ports_if_necessary(plugin);
	for (uint32_t i = 0; i < plugin->num_ports; ++i) {
		LilvPort* port = plugin->ports[i];
		if (lilv_node_equals(port->symbol, symbol)) {
			return port;
		}
	}

	return NULL;
}

LILV_API LilvNode*
lilv_plugin_get_project(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);

	return serd_node_copy(serd_model_get(plugin->world->model,
	                                     plugin->plugin_uri,
	                                     plugin->world->uris.lv2_project,
	                                     NULL,
	                                     NULL));
}

static SerdNode*
lilv_plugin_get_author(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);

	SerdNode* doap_maintainer = serd_new_uri(NS_DOAP "maintainer");

	const SerdNode* maintainer = serd_model_get(
		plugin->world->model,
		plugin->plugin_uri,
		doap_maintainer,
		NULL,
		NULL);

	if (!maintainer) {
		LilvNode* project = lilv_plugin_get_project(plugin);
		if (!project) {
			serd_node_free(doap_maintainer);
			return NULL;
		}

		maintainer = serd_model_get(
			plugin->world->model,
			project,
			doap_maintainer,
			NULL,
			NULL);

		lilv_node_free(project);
	}

	serd_node_free(doap_maintainer);

	return maintainer ? serd_node_copy(maintainer) : NULL;
}

static LilvNode*
lilv_plugin_get_author_property(const LilvPlugin* plugin, const char* uri)
{
	SerdNode* author = lilv_plugin_get_author(plugin);
	if (author) {
		SerdNode* pred = serd_new_uri(uri);
		LilvNode* ret  = lilv_plugin_get_one(plugin, author, pred);
		serd_node_free(pred);
		serd_node_free(author);
		return ret;
	}
	return NULL;
}

LILV_API LilvNode*
lilv_plugin_get_author_name(const LilvPlugin* plugin)
{
	return lilv_plugin_get_author_property(plugin, NS_FOAF "name");
}

LILV_API LilvNode*
lilv_plugin_get_author_email(const LilvPlugin* plugin)
{
	return lilv_plugin_get_author_property(plugin, NS_FOAF "mbox");
}

LILV_API LilvNode*
lilv_plugin_get_author_homepage(const LilvPlugin* plugin)
{
	return lilv_plugin_get_author_property(plugin, NS_FOAF "homepage");
}

LILV_API bool
lilv_plugin_is_replaced(const LilvPlugin* plugin)
{
	return plugin->replaced;
}

LILV_API LilvUIs*
lilv_plugin_get_uis(const LilvPlugin* plugin)
{
	lilv_plugin_load_if_necessary(plugin);

	SerdNode* ui_ui_node = serd_new_uri(LV2_UI__ui);
	SerdNode* ui_binary_node = serd_new_uri(LV2_UI__binary);

	LilvUIs*   result = lilv_uis_new();
	SerdRange* uis    = serd_model_range(plugin->world->model,
	                                     plugin->plugin_uri,
	                                     ui_ui_node,
	                                     NULL,
	                                     NULL);

	FOREACH_MATCH(s, uis) {
		const SerdNode* ui = serd_statement_get_object(s);

		LilvNode* type   = lilv_plugin_get_unique(plugin, ui, plugin->world->uris.rdf_a);
		LilvNode* binary = lilv_plugin_get_one(plugin, ui, plugin->world->uris.lv2_binary);
		if (!binary) {
			binary = lilv_plugin_get_unique(plugin, ui, ui_binary_node);
		}

		if (serd_node_get_type(ui) != SERD_URI
		    || !lilv_node_is_uri(type)
		    || !lilv_node_is_uri(binary)) {
			lilv_node_free(binary);
			lilv_node_free(type);
			LILV_ERRORF("Corrupt UI <%s>\n", serd_node_get_string(ui));
			continue;
		}

		LilvUI* lilv_ui = lilv_ui_new(
			plugin->world,
			serd_node_copy(ui),
			type,
			binary);

		zix_tree_insert((ZixTree*)result, lilv_ui, NULL);
	}
	serd_range_free(uis);

	serd_node_free(ui_binary_node);
	serd_node_free(ui_ui_node);

	if (lilv_uis_size(result) > 0) {
		return result;
	} else {
		lilv_uis_free(result);
		return NULL;
	}
}

LILV_API LilvNodes*
lilv_plugin_get_related(const LilvPlugin* plugin, const LilvNode* type)
{
	lilv_plugin_load_if_necessary(plugin);

	LilvWorld* const world   = plugin->world;
	LilvNodes* const related = lilv_world_find_nodes_internal(
		world,
		NULL,
		world->uris.lv2_appliesTo,
		lilv_plugin_get_uri(plugin));

	if (!type) {
		return related;
	}

	LilvNodes* matches = lilv_nodes_new();
	LILV_FOREACH(nodes, i, related) {
		LilvNode* node  = (LilvNode*)lilv_collection_get((ZixTree*)related, i);
		if (serd_model_ask(
			    world->model, node, world->uris.rdf_a, type, NULL)) {
			zix_tree_insert((ZixTree*)matches,
			                serd_node_copy(node),
			                NULL);
		}
	}

	lilv_nodes_free(related);
	return matches;
}

static SerdEnv*
new_lv2_env(const SerdNode* base)
{
	SerdEnv* env = serd_env_new(base);

	serd_env_set_prefix_from_strings(env, "doap", NS_DOAP);
	serd_env_set_prefix_from_strings(env, "foaf", NS_FOAF);
	serd_env_set_prefix_from_strings(env, "lv2",  NS_LV2);
	serd_env_set_prefix_from_strings(env, "owl",  NS_OWL);
	serd_env_set_prefix_from_strings(env, "rdf",  NS_RDF);
	serd_env_set_prefix_from_strings(env, "rdfs", NS_RDFS);
	serd_env_set_prefix_from_strings(env, "xsd",  NS_XSD);

	return env;
}

static void
maybe_write_prefixes(const SerdSink* sink, SerdEnv* env, FILE* file)
{
	fseek(file, 0, SEEK_END);
	if (ftell(file) == 0) {
		serd_env_write_prefixes(env, sink);
	} else {
		fprintf(file, "\n");
	}
}

LILV_API void
lilv_plugin_write_description(LilvWorld*        world,
                              const LilvPlugin* plugin,
                              const LilvNode*   base_uri,
                              FILE*             plugin_file)
{
	const LilvNode* subject   = lilv_plugin_get_uri(plugin);
	const uint32_t  num_ports = lilv_plugin_get_num_ports(plugin);
	const SerdNode* base      = base_uri;
	SerdEnv*        env       = new_lv2_env(base);

	SerdWriter* writer = serd_writer_new(
		world->world,
		SERD_TURTLE,
		0,
		env,
		(SerdWriteFunc)fwrite,
		plugin_file);

	const SerdSink* iface = serd_writer_get_sink(writer);

	// Write prefixes if this is a new file
	maybe_write_prefixes(iface, env, plugin_file);

	// Write plugin description
	SerdRange* plug_range = serd_model_range(
		world->model, subject, NULL, NULL, NULL);
	serd_range_serialise(plug_range, iface, 0);

	// Write port descriptions
	for (uint32_t i = 0; i < num_ports; ++i) {
		const LilvPort* port = plugin->ports[i];
		SerdRange* port_range = serd_model_range(
			world->model, port->node, NULL, NULL, NULL);
		serd_range_serialise(port_range, iface, 0);
	}

	serd_writer_free(writer);
	serd_env_free(env);
}

LILV_API void
lilv_plugin_write_manifest_entry(LilvWorld*        world,
                                 const LilvPlugin* plugin,
                                 const LilvNode*   base_uri,
                                 FILE*             manifest_file,
                                 const char*       plugin_file_path)
{
	const LilvNode* subject = lilv_plugin_get_uri(plugin);
	const SerdNode* base    = base_uri;
	SerdEnv*        env     = new_lv2_env(base);

	SerdWriter* writer = serd_writer_new(
		world->world,
		SERD_TURTLE,
		0,
		env,
		(SerdWriteFunc)fwrite,
		manifest_file);

	const SerdSink* iface = serd_writer_get_sink(writer);

	// Write prefixes if this is a new file
	maybe_write_prefixes(iface, env, manifest_file);

	// Write manifest entry
	serd_sink_write(iface,
	                0,
	                subject,
	                plugin->world->uris.rdf_a,
	                plugin->world->uris.lv2_Plugin,
	                NULL);

	const SerdNode* file_node = serd_new_uri(plugin_file_path);
	serd_sink_write(serd_writer_get_sink(writer),
	                0,
	                subject,
	                plugin->world->uris.rdfs_seeAlso,
	                file_node,
	                NULL);

	serd_writer_free(writer);
	serd_env_free(env);
}
