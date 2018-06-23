/*
    Copyright (C) 2018 Brian Sorahan
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/midiport.h>

// Kills the program.
void die(const char *msg);

int initialize_ports();
int connect_ports();

unsigned char color(int g, int r);
int handle_launchpad_event(jack_nframes_t nframes, jack_midi_event_t event, void *ndout, void *lpout);

// Process callback.
int process(jack_nframes_t nframes, void *arg);

jack_client_t *client;
jack_port_t *mclk_input; // receive jack_midi_clock data
jack_port_t *launchpad_input;
jack_port_t *launchpad_output;
jack_port_t *norddrum_output;
jack_status_t status;

int main() {
	int rc;

	// Create the client.
	client = jack_client_open("ndtrig", JackNoStartServer, &status);
	if (client == NULL) {
		die("failed to initialize JACK client");
	}
	rc = initialize_ports();
	if (rc != 0) {
		die("initializing launchpad");
	}
	// Set the process callback.
	rc = jack_set_process_callback(client, process, client);
	if (rc != 0) {
		die("failed to set JACK process callback");
	}
	// Activate the client.
	rc = jack_activate(client);
	if (rc != 0) {
		die("failed to activate JACK client");
	}
	// Connect ports.
	rc = connect_ports();
	if (rc != 0) {
		die("failed to connect ports");
	}
	// Wait.
	while (1) {
		sleep(1);
	}
	// Deactivate the client.
	printf("deactivating client");
	rc = jack_deactivate(client);
	if (rc != 0) {
		die("failed to deactivate JACK client");
	}
	// Close the client.
	printf("closing client");
	rc = jack_client_close(client);
	if (rc != 0) {
		die("failed to close JACK client");
	}
}

void die(const char *msg) {
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

int process(jack_nframes_t nframes, void *arg) {
	int rc;
	
	// Input buffer for launchpad data.
	void *lpin = jack_port_get_buffer(launchpad_input, nframes);
	
	// Buffer for data we will send to the nord drum.
	void *lpout = jack_port_get_buffer(launchpad_output, nframes);
	void *ndout = jack_port_get_buffer(norddrum_output, nframes);

	// Clear the output buffer.
	jack_midi_clear_buffer(lpout);
	jack_midi_clear_buffer(ndout);

	// Process the input events.
	jack_nframes_t n = jack_midi_get_event_count(lpin);

	if (n == 0) {
		// If we didn't get any events then clear the output bus(ses). Is this necessary?
		rc = jack_midi_event_write(ndout, 0, NULL, 0);
		if (rc != 0) {
			fprintf(stderr, "error writing data to nord drum\n");
			return rc;
		}
		rc = jack_midi_event_write(lpout, 0, NULL, 0);
		if (rc != 0) {
			fprintf(stderr, "error writing data to launchpad\n");
			return rc;
		}
	}
	for (jack_nframes_t i = 0; i < n; i++) {
		jack_midi_event_t midi_event;
		
		rc = jack_midi_event_get(&midi_event, lpin, (uint32_t) i);
		if (rc != 0) {
			return rc;
		}
		printf("Handling launchpad event\n");
		rc = handle_launchpad_event(nframes, midi_event, ndout, lpout);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int initialize_ports() {
	int rc;
	
	// Register launchpad input port (receive data from launchpad).
	launchpad_input = jack_port_register(client, "launchpad input", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (launchpad_input == NULL) {
		fprintf(stderr, "failed to register launchpad input port");
		return 1;
	}
	// Register launchpad output port (send data to launchpad).
	launchpad_output = jack_port_register(client, "launchpad output", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
	if (launchpad_output == NULL) {
		fprintf(stderr, "failed to register launchpad output port");
		return 1;
	}
	// Register jack_midi_clock input port (receive data from jack_midi_clock).
	mclk_input = jack_port_register(client, "mclk input", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (mclk_input == NULL) {
		fprintf(stderr, "failed to register jack_midi_clock input port");
		return 1;
	}
	// Register nord drum output port (send data to nord drum).
	norddrum_output = jack_port_register(client, "nord drum output", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
	if (norddrum_output == NULL) {
		fprintf(stderr, "failed to register nord drum output port");
		return 1;
	}
	return 0;
}

int connect_ports() {
	int rc;
	
	const char *lpin = "a2j:Launchpad Mini [28] (playback): Launchpad Mini MIDI 1";
	const char *lpout = "a2j:Launchpad Mini [28] (capture): Launchpad Mini MIDI 1";
	const char *scarlett = "a2j:Scarlett 6i6 USB [20] (playback): Scarlett 6i6 USB MIDI 1";
	
	rc = jack_connect(client, lpout, jack_port_name(launchpad_input));
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to launchpad output: %s\n", lpout);
		return rc;
	}
	rc = jack_connect(client, jack_port_name(launchpad_output), lpin);
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to launchpad input: %s\n", lpin);
		return rc;
	}
	rc = jack_connect(client, jack_port_name(norddrum_output), scarlett);
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to scarlett 6i6: %s\n", scarlett);
		return rc;
	}
	return 0;
}

int handle_launchpad_event(jack_nframes_t nframes, jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc;
	
	// We always expect at least 3 bytes.
	if (midi_event.size < 3) {
		fprintf(stderr, "expected at least 3 bytes in MIDI message\n");
		return 1;
	}
	unsigned char ndevent[3] = {midi_event.buffer[0] + midi_event.buffer[1] - 0x70, 60, 127};
	unsigned char lpevent[3] = {midi_event.buffer[0], midi_event.buffer[1], color(3, 0)};
	
	for (size_t j = 0; j < midi_event.size; j++) {
		if (j == 0) {
			printf("%X", midi_event.buffer[j]);
		} else {
			printf(" %X", midi_event.buffer[j]);
		}
	}
	printf("\n");

	printf("nd out:");
	for (int j = 0; j < 3; j++) {
		printf(" %X", ndevent[j]);
	}
	printf("\n");
	
	printf("lp out:");
	for (int j = 0; j < 3; j++) {
		printf(" %X", lpevent[j]);
	}
	printf("\n");
	
	rc = jack_midi_event_write(ndout, 0, ndevent, 3);
	if (rc != 0) {
		fprintf(stderr, "error writing midi data to nord drum\n");
		return rc;
	}
	rc = jack_midi_event_write(lpout, 0, lpevent, 3);
	if (rc != 0) {
		fprintf(stderr, "error writing midi data to nord drum\n");
		return rc;
	}
	return 0;
}

unsigned char color(int g, int r) {
	return (unsigned char) (g * 16) + r;
	/* switch (g) { */
	/* case 1: */
	/* 	val += 8 */
	/* case 2: */
	/* 	val += 16 */
	/* } */
	/* return val; */
}
