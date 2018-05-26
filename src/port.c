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

#include "lv2/atom/atom.h"
#include "lv2/core/lv2.h"
#include "lv2/event/event.h"

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "zix/tree.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

LilvPort*
lilv_port_new(LilvWorld*      world,
              const SerdNode* node,
              uint32_t        index,
              const char*     symbol)
{
	LilvPort* port = (LilvPort*)malloc(sizeof(LilvPort));
	port->node    = serd_node_copy(node);
	port->index   = index;
	port->symbol  = serd_new_string(symbol);
	port->classes = lilv_nodes_new();
	return port;
}

void
lilv_port_free(const LilvPlugin* plugin, LilvPort* port)
{
	if (port) {
		lilv_node_free(port->node);
		lilv_nodes_free(port->classes);
		lilv_node_free(port->symbol);
		free(port);
	}
}

LILV_API bool
lilv_port_is_a(const LilvPlugin* plugin,
               const LilvPort*   port,
               const LilvNode*   port_class)
{
	LILV_FOREACH(nodes, i, port->classes) {
		if (lilv_node_equals(lilv_nodes_get(port->classes, i), port_class)) {
			return true;
		}
	}

	return false;
}

LILV_API bool
lilv_port_has_property(const LilvPlugin* plugin,
                       const LilvPort*   port,
                       const LilvNode*   property)
{
	return serd_model_ask(plugin->world->model,
	                      port->node,
	                      plugin->world->uris.lv2_portProperty,
	                      property,
	                      NULL);
}

LILV_API bool
lilv_port_supports_event(const LilvPlugin* plugin,
                         const LilvPort*   port,
                         const LilvNode*   event_type)
{
	const char* predicates[] = { LV2_EVENT__supportsEvent,
	                             LV2_ATOM__supports,
	                             NULL };

	for (const char** pred = predicates; *pred; ++pred) {
		if (serd_model_ask(plugin->world->model,
		                   port->node,
		                   serd_new_uri(*pred),
		                   event_type,
		                   NULL)) {
			return true;
		}
	}
	return false;
}

static LilvNodes*
lilv_port_get_value_by_node(const LilvPlugin* plugin,
                            const LilvPort*   port,
                            const SerdNode*   predicate)
{
	return lilv_world_find_nodes_internal(
		plugin->world, port->node, predicate, NULL);
}

LILV_API const LilvNode*
lilv_port_get_node(const LilvPlugin* plugin,
                   const LilvPort*   port)
{
	return port->node;
}

LILV_API LilvNodes*
lilv_port_get_value(const LilvPlugin* plugin,
                    const LilvPort*   port,
                    const LilvNode*   predicate)
{
	if (!lilv_node_is_uri(predicate)) {
		LILV_ERRORF("Predicate `%s' is not a URI\n",
		            serd_node_get_string(predicate));
		return NULL;
	}

	return lilv_port_get_value_by_node(plugin, port, predicate);
}

LILV_API LilvNode*
lilv_port_get(const LilvPlugin* plugin,
              const LilvPort*   port,
              const LilvNode*   predicate)
{
	LilvNodes* values = lilv_port_get_value(plugin, port, predicate);

	LilvNode* value = lilv_node_duplicate(
		values ? lilv_nodes_get_first(values) : NULL);

	lilv_nodes_free(values);
	return value;
}

LILV_API uint32_t
lilv_port_get_index(const LilvPlugin* plugin,
                    const LilvPort*   port)
{
	return port->index;
}

LILV_API const LilvNode*
lilv_port_get_symbol(const LilvPlugin* plugin,
                     const LilvPort*   port)
{
	return port->symbol;
}

LILV_API LilvNode*
lilv_port_get_name(const LilvPlugin* plugin,
                   const LilvPort*   port)
{
	LilvNodes* results = lilv_port_get_value_by_node(
		plugin, port, plugin->world->uris.lv2_name);

	LilvNode* ret = NULL;
	if (results) {
		LilvNode* val = lilv_nodes_get_first(results);
		if (lilv_node_is_string(val)) {
			ret = lilv_node_duplicate(val);
		}
		lilv_nodes_free(results);
	}

	if (!ret) {
		LILV_WARNF("Plugin <%s> port has no (mandatory) doap:name\n",
		           lilv_node_as_string(lilv_plugin_get_uri(plugin)));
	}

	return ret;
}

LILV_API const LilvNodes*
lilv_port_get_classes(const LilvPlugin* plugin,
                      const LilvPort*   port)
{
	return port->classes;
}

LILV_API void
lilv_port_get_range(const LilvPlugin* plugin,
                    const LilvPort*   port,
                    LilvNode**        def,
                    LilvNode**        min,
                    LilvNode**        max)
{
	if (def) {
		LilvNodes* defaults = lilv_port_get_value_by_node(
			plugin, port, plugin->world->uris.lv2_default);
		*def = defaults
			? lilv_node_duplicate(lilv_nodes_get_first(defaults))
			: NULL;
		lilv_nodes_free(defaults);
	}
	if (min) {
		LilvNodes* minimums = lilv_port_get_value_by_node(
			plugin, port, plugin->world->uris.lv2_minimum);
		*min = minimums
			? lilv_node_duplicate(lilv_nodes_get_first(minimums))
			: NULL;
		lilv_nodes_free(minimums);
	}
	if (max) {
		LilvNodes* maximums = lilv_port_get_value_by_node(
			plugin, port, plugin->world->uris.lv2_maximum);
		*max = maximums
			? lilv_node_duplicate(lilv_nodes_get_first(maximums))
			: NULL;
		lilv_nodes_free(maximums);
	}
}

LILV_API LilvScalePoints*
lilv_port_get_scale_points(const LilvPlugin* plugin,
                           const LilvPort*   port)
{
	SerdRange* points = serd_model_range(plugin->world->model,
	                                     port->node,
	                                     serd_new_uri(LV2_CORE__scalePoint),
	                                     NULL,
	                                     NULL);

	LilvScalePoints* ret = NULL;
	if (!serd_range_empty(points)) {
		ret = lilv_scale_points_new();
	}

	FOREACH_MATCH(s, points) {
		const SerdNode* point = serd_statement_get_object(s);

		LilvNode* value = lilv_plugin_get_unique(plugin,
		                                         point,
		                                         plugin->world->uris.rdf_value);

		LilvNode* label = lilv_plugin_get_unique(plugin,
		                                         point,
		                                         plugin->world->uris.rdfs_label);

		if (value && label) {
			zix_tree_insert(
				(ZixTree*)ret, lilv_scale_point_new(value, label), NULL);
		}
	}
	serd_range_free(points);

	assert(!ret || lilv_nodes_size(ret) > 0);
	return ret;
}

LILV_API LilvNodes*
lilv_port_get_properties(const LilvPlugin* plugin,
                         const LilvPort*   port)
{
	LilvNode*  pred = serd_node_copy(plugin->world->uris.lv2_portProperty);
	LilvNodes* ret  = lilv_port_get_value(plugin, port, pred);
	lilv_node_free(pred);
	return ret;
}
