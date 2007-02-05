/* jack_host - SLV2 Jack Host
 * Copyright (C) 2007 Dave Robillard <drobilla.net>
 *  
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <slv2/slv2.h>
#include <jack/jack.h>

#define WITH_MIDI 1
#define MIDI_BUFFER_SIZE 1024

#ifdef WITH_MIDI
#include <jack/midiport.h>
#include "lv2-miditype.h"
#include "lv2-midifunctions.h"
#endif // WITH_MIDI

struct Port {
	enum Direction { INPUT, OUTPUT}         direction;
	enum Type      { UNKNOWN, FLOAT, MIDI } type;

	jack_port_t* jack_port;   /**< For audio and MIDI ports, otherwise NULL */
	float        control;     /**< For control ports, otherwise 0.0f */
	LV2_MIDI*    midi_buffer; /**< For midi ports, otherwise NULL */
};


/** This program's data */
struct JackHost {
	jack_client_t* jack_client; /**< Jack client */
	SLV2Plugin*    plugin;      /**< Plugin "class" (actually just a few strings) */
	SLV2Instance*  instance;    /**< Plugin "instance" (loaded shared lib) */
	uint32_t       num_ports;   /**< Size of the two following arrays: */
	struct Port*   ports;       /** Port array of size num_ports */
};


void die(const char* msg);
void create_port(struct JackHost* host, uint32_t port_index);
int  jack_process_cb(jack_nframes_t nframes, void* data);
void list_plugins(SLV2List list);


int
main(int argc, char** argv)
{
	struct JackHost host;
	host.jack_client = NULL;
	host.num_ports   = 0;
	host.ports       = NULL;
	
	/* Find all installed plugins */
	SLV2List plugins = slv2_list_new();
	slv2_list_load_all(plugins);
	//slv2_list_load_bundle(plugins, "http://www.scs.carleton.ca/~drobilla/files/Amp-swh.lv2");

	/* Find the plugin to run */
	const char* plugin_uri = (argc == 2) ? argv[1] : NULL;
	
	if (!plugin_uri) {
		fprintf(stderr, "\nYou must specify a plugin URI to load.\n");
		fprintf(stderr, "\nKnown plugins:\n\n");
		list_plugins(plugins);
		return EXIT_FAILURE;
	}

	printf("URI:\t%s\n", plugin_uri);
	host.plugin = slv2_list_get_plugin_by_uri(plugins, plugin_uri);
	
	if (!host.plugin) {
		fprintf(stderr, "Failed to find plugin %s.\n", plugin_uri);
		slv2_list_free(plugins);
		return EXIT_FAILURE;
	}

	/* Get the plugin's name */
	char* name = slv2_plugin_get_name(host.plugin);
	printf("Name:\t%s\n", name);
	
	/* Connect to JACK (with plugin name as client name) */
	host.jack_client = jack_client_open(name, JackNullOption, NULL);
	free(name);
	if (!host.jack_client)
		die("Failed to connect to JACK.");
	else
		printf("Connected to JACK.\n");
	
	/* Instantiate the plugin */
	host.instance = slv2_plugin_instantiate(
		host.plugin, jack_get_sample_rate(host.jack_client), NULL);
	if (!host.instance)
		die("Failed to instantiate plugin.\n");
	else
		printf("Succesfully instantiated plugin.\n");

	jack_set_process_callback(host.jack_client, &jack_process_cb, (void*)(&host));
	
	/* Create ports */
	host.num_ports  = slv2_plugin_get_num_ports(host.plugin);
	host.ports = calloc((size_t)host.num_ports, sizeof(struct Port));
	
	for (uint32_t i=0; i < host.num_ports; ++i)
		create_port(&host, i);
	
	/* Activate plugin and JACK */
	slv2_instance_activate(host.instance);
	jack_activate(host.jack_client);
	
	/* Run */
	printf("Press enter to quit: ");
	getc(stdin);
	printf("\n");
	
	/* Deactivate plugin and JACK */
	slv2_instance_free(host.instance);
	slv2_list_free(plugins);
	
	printf("Shutting down JACK.\n");
	for (unsigned long i=0; i < host.num_ports; ++i) {
		if (host.ports[i].jack_port != NULL) {
			jack_port_unregister(host.jack_client, host.ports[i].jack_port);
			host.ports[i].jack_port = NULL;
		}
		if (host.ports[i].midi_buffer != NULL) {
			lv2midi_free(host.ports[i].midi_buffer);
		}
	}
	jack_client_close(host.jack_client);
	
	return 0;
}


/** Abort and exit on error */
void
die(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}


/** Creates a port and connects the plugin instance to it's data location.
 *
 * For audio ports, creates a jack port and connects plugin port to buffer.
 *
 * For control ports, sets controls array to default value and connects plugin
 * port to that element.
 */
void
create_port(struct JackHost* host,
            uint32_t         port_index)
{
	//struct Port* port = (Port*)malloc(sizeof(Port));
	struct Port* const port = &host->ports[port_index];

	port->type        = UNKNOWN;
	port->jack_port   = NULL;
	port->control     = 0.0f;
	port->midi_buffer = NULL;
	
	slv2_instance_connect_port(host->instance, port_index, NULL);

	char* type_str = slv2_port_get_data_type(host->plugin, port_index);
	if (!strcmp(type_str, SLV2_DATA_TYPE_FLOAT))
		port->type = FLOAT;
	else if (!strcmp(type_str, SLV2_DATA_TYPE_MIDI))
		port->type = MIDI;
	
	/* Get the port symbol (label) for console printing */
	char* symbol = slv2_port_get_symbol(host->plugin, port_index);
	
	/* Get the 'class' (not data type) of the port (control input, audio output, etc) */
	enum SLV2PortClass class = slv2_port_get_class(host->plugin, port_index);

	if (port->type == FLOAT) {
		
		/* Connect the port based on it's 'class' */
		switch (class) {
		case SLV2_CONTROL_RATE_INPUT:
			port->direction = INPUT;
			port->control = slv2_port_get_default_value(host->plugin, port_index);
			slv2_instance_connect_port(host->instance, port_index, &port->control);
			printf("Set %s to %f\n", symbol, host->ports[port_index].control);
			break;
		case SLV2_CONTROL_RATE_OUTPUT:
			port->direction = OUTPUT;
			slv2_instance_connect_port(host->instance, port_index, &port->control);
			break;
		case SLV2_AUDIO_RATE_INPUT:
			port->direction = INPUT;
			port->jack_port = jack_port_register(host->jack_client,
				symbol, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
			break;
		case SLV2_AUDIO_RATE_OUTPUT:
			port->direction = OUTPUT;
			port->jack_port = jack_port_register(host->jack_client,
				symbol, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
			break;
		default:
			fprintf(stderr, "ERROR: Unknown port class\n");
		}
	
	} else if (port->type == MIDI) {

		if (class == SLV2_CONTROL_RATE_INPUT) {
			port->direction = INPUT;
			port->jack_port = jack_port_register(host->jack_client,
				symbol, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
			port->midi_buffer = lv2midi_new(MIDI_BUFFER_SIZE);
			slv2_instance_connect_port(host->instance, port_index, port->midi_buffer);
		} else if (class == SLV2_CONTROL_RATE_OUTPUT) {
			port->direction = OUTPUT;
			port->jack_port = jack_port_register(host->jack_client,
				symbol, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
			port->midi_buffer = lv2midi_new(MIDI_BUFFER_SIZE);
			slv2_instance_connect_port(host->instance, port_index, port->midi_buffer);
		} else {
			fprintf(stderr, "ERROR: Audio rate MIDI port??  Ignoring.\n");
		}

	} else {

		fprintf(stderr, "Unrecognized data type %s for port %s, ignored.\n",
				type_str, symbol);
		fprintf(stderr, "                       %s\n",
				SLV2_DATA_TYPE_MIDI);

	}

	free(symbol);
	free(type_str);
}


/** Jack process callback. */
int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	struct JackHost* const host = (struct JackHost*)data;

	/* Connect inputs */
	for (uint32_t p=0; p < host->num_ports; ++p) {
		if (!host->ports[p].jack_port)
			continue;

		if (host->ports[p].type == FLOAT) {
			slv2_instance_connect_port(host->instance, p,
					jack_port_get_buffer(host->ports[p].jack_port, nframes));
		} else if (host->ports[p].type == MIDI) {
			
			void* jack_buffer = jack_port_get_buffer(host->ports[p].jack_port, nframes);

			LV2_MIDIState state;
			lv2midi_reset_state(&state, host->ports[p].midi_buffer, nframes);
			lv2midi_reset_buffer(state.midi);
			
			if (host->ports[p].direction == INPUT) {
				jack_midi_event_t ev;

				const jack_nframes_t event_count
					= jack_midi_get_event_count(jack_buffer, nframes);

				for (jack_nframes_t e=0; e < event_count; ++e) {

					jack_midi_event_get(&ev, jack_buffer, e, nframes);

					state.midi = host->ports[p].midi_buffer;
					lv2midi_put_event(&state, (double)ev.time, ev.size, ev.buffer);
				}
			}
		}
	}
	

	/* Run plugin for this cycle */
	slv2_instance_run(host->instance, nframes);


	/* Deliver output */
	for (uint32_t p=0; p < host->num_ports; ++p) {
		if (host->ports[p].jack_port
				&& host->ports[p].type == MIDI
				&& host->ports[p].direction == OUTPUT) {

			void* jack_buffer = jack_port_get_buffer(host->ports[p].jack_port, nframes);

			jack_midi_clear_buffer(jack_buffer, nframes);

			LV2_MIDIState state;
			lv2midi_reset_state(&state, host->ports[p].midi_buffer, nframes);
			
			double         timestamp = 0.0f;
			uint32_t       size      = 0;
			unsigned char* data      = NULL;

			const uint32_t event_count = state.midi->event_count;

			for (uint32_t i=0; i < event_count; ++i) {
				lv2midi_get_event(&state, &timestamp, &size, &data);

				jack_midi_event_write(jack_buffer,
						(jack_nframes_t)timestamp, data, size, nframes);

				lv2midi_increment(&state);
			}

		}
	}

	return 0;
}


void
list_plugins(SLV2List list)
{
	for (size_t i=0; i < slv2_list_get_length(list); ++i) {
		const SLV2Plugin* const p = slv2_list_get_plugin_by_index(list, i);
		printf("%s\n", slv2_plugin_get_uri(p));
	}
}