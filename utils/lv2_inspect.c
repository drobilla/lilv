/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

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

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lilv/lilv.h"

#include "lilv-config.h"

LilvValue* event_class   = NULL;
LilvValue* control_class = NULL;
LilvValue* in_group_pred = NULL;
LilvValue* role_pred     = NULL;
LilvValue* preset_pred   = NULL;
LilvValue* title_pred    = NULL;

void
print_group(const LilvPlugin* p,
            const LilvValue*  group,
            LilvValue*        type,
            LilvValue*        symbol)
{
	printf("\n\tGroup %s:\n", lilv_value_as_string(group));
	printf("\t\tType: %s\n", lilv_value_as_string(type));
	printf("\t\tSymbol: %s\n", lilv_value_as_string(symbol));
}

void
print_port(const LilvPlugin* p,
           uint32_t          index,
           float*            mins,
           float*            maxes,
           float*            defaults)
{
	const LilvPort* port = lilv_plugin_get_port_by_index(p, index);

	printf("\n\tPort %d:\n", index);

	if (!port) {
		printf("\t\tERROR: Illegal/nonexistent port\n");
		return;
	}

	bool first = true;

	const LilvValues* classes = lilv_port_get_classes(p, port);
	printf("\t\tType:       ");
	LILV_FOREACH(values, i, classes) {
		const LilvValue* value = lilv_values_get(classes, i);
		if (!first) {
			printf("\n\t\t            ");
		}
		printf("%s", lilv_value_as_uri(value));
		first = false;
	}

	if (lilv_port_is_a(p, port, event_class)) {
		LilvValues* supported = lilv_port_get_value_by_qname(p, port,
			"lv2ev:supportsEvent");
		if (lilv_values_size(supported) > 0) {
			printf("\n\t\tSupported events:\n");
			LILV_FOREACH(values, i, supported) {
				const LilvValue* value = lilv_values_get(supported, i);
				printf("\t\t\t%s\n", lilv_value_as_uri(value));
			}
		}
		lilv_values_free(supported);
	}

	LilvScalePoints* points = lilv_port_get_scale_points(p, port);
	if (points)
		printf("\n\t\tScale Points:\n");
	LILV_FOREACH(scale_points, i, points) {
		const LilvScalePoint* p = lilv_scale_points_get(points, i);
		printf("\t\t\t%s = \"%s\"\n",
				lilv_value_as_string(lilv_scale_point_get_value(p)),
				lilv_value_as_string(lilv_scale_point_get_label(p)));
	}
	lilv_scale_points_free(points);

	const LilvValue* sym = lilv_port_get_symbol(p, port);
	printf("\n\t\tSymbol:     %s\n", lilv_value_as_string(sym));

	LilvValue* name = lilv_port_get_name(p, port);
	printf("\t\tName:       %s\n", lilv_value_as_string(name));
	lilv_value_free(name);

	LilvValues* groups = lilv_port_get_value(p, port, in_group_pred);
	if (lilv_values_size(groups) > 0)
		printf("\t\tGroup:      %s\n",
		       lilv_value_as_string(
			       lilv_values_get(groups, lilv_values_begin(groups))));
	lilv_values_free(groups);

	LilvValues* roles = lilv_port_get_value(p, port, role_pred);
	if (lilv_values_size(roles) > 0)
		printf("\t\tRole:       %s\n",
		       lilv_value_as_string(
			       lilv_values_get(roles, lilv_values_begin(roles))));
	lilv_values_free(roles);

	if (lilv_port_is_a(p, port, control_class)) {
		if (!isnan(mins[index]))
			printf("\t\tMinimum:    %f\n", mins[index]);
		if (!isnan(mins[index]))
			printf("\t\tMaximum:    %f\n", maxes[index]);
		if (!isnan(mins[index]))
			printf("\t\tDefault:    %f\n", defaults[index]);
	}

	LilvValues* properties = lilv_port_get_properties(p, port);
	if (lilv_values_size(properties) > 0)
		printf("\t\tProperties: ");
	first = true;
	LILV_FOREACH(values, i, properties) {
		if (!first) {
			printf("\t\t            ");
		}
		printf("%s\n", lilv_value_as_uri(lilv_values_get(properties, i)));
		first = false;
	}
	if (lilv_values_size(properties) > 0)
		printf("\n");
	lilv_values_free(properties);
}

void
print_plugin(const LilvPlugin* p)
{
	LilvValue* val = NULL;

	printf("%s\n\n", lilv_value_as_uri(lilv_plugin_get_uri(p)));

	val = lilv_plugin_get_name(p);
	if (val) {
		printf("\tName:              %s\n", lilv_value_as_string(val));
		lilv_value_free(val);
	}

	const LilvPluginClass* pclass      = lilv_plugin_get_class(p);
	const LilvValue*       class_label = lilv_plugin_class_get_label(pclass);
	if (class_label) {
		printf("\tClass:             %s\n", lilv_value_as_string(class_label));
	}

	val = lilv_plugin_get_author_name(p);
	if (val) {
		printf("\tAuthor:            %s\n", lilv_value_as_string(val));
		lilv_value_free(val);
	}

	val = lilv_plugin_get_author_email(p);
	if (val) {
		printf("\tAuthor Email:      %s\n", lilv_value_as_uri(val));
		lilv_value_free(val);
	}

	val = lilv_plugin_get_author_homepage(p);
	if (val) {
		printf("\tAuthor Homepage:   %s\n", lilv_value_as_uri(val));
		lilv_value_free(val);
	}

	if (lilv_plugin_has_latency(p)) {
		uint32_t latency_port = lilv_plugin_get_latency_port_index(p);
		printf("\tHas latency:       yes, reported by port %d\n", latency_port);
	} else {
		printf("\tHas latency:       no\n");
	}

	printf("\tBundle:            %s\n",
	       lilv_value_as_uri(lilv_plugin_get_bundle_uri(p)));

	const LilvValue* binary_uri = lilv_plugin_get_library_uri(p);
	if (binary_uri) {
		printf("\tBinary:            %s\n",
		       lilv_value_as_uri(lilv_plugin_get_library_uri(p)));
	}

	LilvUIs* uis = lilv_plugin_get_uis(p);
	if (lilv_values_size(uis) > 0) {
		printf("\tUI:                ");
		LILV_FOREACH(uis, i, uis) {
			const LilvUI* ui = lilv_uis_get(uis, i);
			printf("%s\n", lilv_value_as_uri(lilv_ui_get_uri(ui)));

			const char* binary = lilv_value_as_uri(lilv_ui_get_binary_uri(ui));

			const LilvValues* types = lilv_ui_get_classes(ui);
			LILV_FOREACH(values, i, types) {
				printf("\t                       Class:  %s\n",
				       lilv_value_as_uri(lilv_values_get(types, i)));
			}

			if (binary)
				printf("\t                       Binary: %s\n", binary);

			printf("\t                       Bundle: %s\n",
			       lilv_value_as_uri(lilv_ui_get_bundle_uri(ui)));
		}
	}
	lilv_uis_free(uis);

	printf("\tData URIs:         ");
	const LilvValues* data_uris = lilv_plugin_get_data_uris(p);
	bool first = true;
	LILV_FOREACH(values, i, data_uris) {
		if (!first) {
			printf("\n\t                   ");
		}
		printf("%s", lilv_value_as_uri(lilv_values_get(data_uris, i)));
		first = false;
	}
	printf("\n");

	/* Required Features */

	LilvValues* features = lilv_plugin_get_required_features(p);
	if (features)
		printf("\tRequired Features: ");
	first = true;
	LILV_FOREACH(values, i, features) {
		if (!first) {
			printf("\n\t                   ");
		}
		printf("%s", lilv_value_as_uri(lilv_values_get(features, i)));
		first = false;
	}
	if (features)
		printf("\n");
	lilv_values_free(features);

	/* Optional Features */

	features = lilv_plugin_get_optional_features(p);
	if (features)
		printf("\tOptional Features: ");
	first = true;
	LILV_FOREACH(values, i, features) {
		if (!first) {
			printf("\n\t                   ");
		}
		printf("%s", lilv_value_as_uri(lilv_values_get(features, i)));
		first = false;
	}
	if (features)
		printf("\n");
	lilv_values_free(features);

	/* Presets */

	LilvValues* presets = lilv_plugin_get_value(p, preset_pred);
	if (presets)
		printf("\tPresets: \n");
	LILV_FOREACH(values, i, presets) {
		LilvValues* titles = lilv_plugin_get_value_for_subject(
			p, lilv_values_get(presets, i), title_pred);
		if (titles) {
			const LilvValue* title = lilv_values_get(titles, lilv_values_begin(titles));
			printf("\t         %s\n", lilv_value_as_string(title));
		}
	}

	/* Ports */

	const uint32_t num_ports = lilv_plugin_get_num_ports(p);
	float* mins     = calloc(num_ports, sizeof(float));
	float* maxes    = calloc(num_ports, sizeof(float));
	float* defaults = calloc(num_ports, sizeof(float));
	lilv_plugin_get_port_ranges_float(p, mins, maxes, defaults);

	for (uint32_t i = 0; i < num_ports; ++i)
		print_port(p, i, mins, maxes, defaults);

	free(mins);
	free(maxes);
	free(defaults);
}

void
print_version()
{
	printf(
		"lv2_inspect (lilv) " LILV_VERSION "\n"
		"Copyright 2007-2011 David Robillard <http://drobilla.net>\n"
		"License: <http://www.opensource.org/licenses/isc-license>\n"
		"This is free software: you are free to change and redistribute it.\n"
		"There is NO WARRANTY, to the extent permitted by law.\n");
}

void
print_usage()
{
	printf("Usage: lv2_inspect PLUGIN_URI\n");
	printf("Show information about an installed LV2 plugin.\n");
}

int
main(int argc, char** argv)
{
	int ret = 0;
	setlocale (LC_ALL, "");

	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);

#define NS_DC   "http://dublincore.org/documents/dcmi-namespace/"
#define NS_PG   "http://lv2plug.in/ns/ext/port-groups#"
#define NS_PSET "http://lv2plug.in/ns/ext/presets#"

	control_class = lilv_value_new_uri(world, LILV_PORT_CLASS_CONTROL);
	event_class   = lilv_value_new_uri(world, LILV_PORT_CLASS_EVENT);
	in_group_pred = lilv_value_new_uri(world, NS_PG "inGroup");
	preset_pred   = lilv_value_new_uri(world, NS_PSET "hasPreset");
	role_pred     = lilv_value_new_uri(world, NS_PG "role");
	title_pred    = lilv_value_new_uri(world, NS_DC "title");

	if (argc != 2) {
		print_usage();
		ret = 1;
		goto done;
	}

	if (!strcmp(argv[1], "--version")) {
		print_version();
		ret = 0;
		goto done;
	} else if (!strcmp(argv[1], "--help")) {
		print_usage();
		ret = 0;
		goto done;
	} else if (argv[1][0] == '-') {
		print_usage();
		ret = 2;
		goto done;
	}

	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
	LilvValue*         uri     = lilv_value_new_uri(world, argv[1]);

	const LilvPlugin* p = lilv_plugins_get_by_uri(plugins, uri);

	if (p) {
		print_plugin(p);
	} else {
		fprintf(stderr, "Plugin not found.\n");
	}

	ret = (p != NULL ? 0 : -1);

	lilv_value_free(uri);

done:
	lilv_value_free(title_pred);
	lilv_value_free(role_pred);
	lilv_value_free(preset_pred);
	lilv_value_free(in_group_pred);
	lilv_value_free(event_class);
	lilv_value_free(control_class);
	lilv_world_free(world);
	return ret;
}

