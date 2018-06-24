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

#define MODE_INIT      (0)
#define MODE_LIVE_TRIG (1)
#define MODE_SEQUENCER (2)

// Kills the program.
void die(const char *msg);

int initialize_ports();
int connect_ports();
int initialize_seq();

unsigned char cell(int step); // Returns the MIDI Note value associated with step.
unsigned char color(int g, int r);

int handle_launchpad_event(jack_midi_event_t event, void *ndout, void *lpout);
int handle_clk_event(jack_midi_event_t event, void *ndout, void *lpout);

int start(void *ndout, void *lpout); // start function takes buffers for MIDI output data.
int play(int step, void *ndout, void *lpout); // play function takes buffers for MIDI output data.
int tick(void *ndout, void *lpout); // tick function takes buffers for MIDI output data.

// Process callback.
int process(jack_nframes_t nframes, void *arg);

// MIDI beat clock.
static uint64_t beat_clock;

// Current and previous step.
static int curr;
static int prev;

// UI mode (live trig or sequencer).
static int mode;

// Sequence data: 6 tracks x 64 steps.
unsigned char seqdata[6][64];

jack_client_t *client;
jack_port_t *mclk_input; // receive jack_midi_clock data
jack_port_t *launchpad_input;
jack_port_t *launchpad_output;
jack_port_t *norddrum_output;
jack_status_t status;

int main() {
	int rc = 0;

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
	rc = initialize_seq();
	if (rc != 0) {
		die("failed to initialize sequencer");
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

int initialize_ports() {
	int rc = 0;
	
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
	int rc = 0;
	
	const char *lpin = "a2j:Launchpad Mini [28] (playback): Launchpad Mini MIDI 1";
	const char *lpout = "a2j:Launchpad Mini [28] (capture): Launchpad Mini MIDI 1";
	const char *mclkout = "jack_midi_clock:mclk_out";
	const char *scarlett = "a2j:Scarlett 6i6 USB [20] (playback): Scarlett 6i6 USB MIDI 1";

	// Receive MIDI from launchpad.
	rc = jack_connect(client, lpout, jack_port_name(launchpad_input));
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to launchpad output: %s\n", lpout);
		return rc;
	}
	// Send MIDI to launchpad.
	rc = jack_connect(client, jack_port_name(launchpad_output), lpin);
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to launchpad input: %s\n", lpin);
		return rc;
	}
	// Receive MIDI from jack_midi_clock.
	rc = jack_connect(client, mclkout, jack_port_name(mclk_input));
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to mclk output: %s\n", mclkout);
		return rc;
	}
	rc = jack_connect(client, jack_port_name(norddrum_output), scarlett);
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to scarlett 6i6: %s\n", scarlett);
		return rc;
	}
	return 0;
}

void die(const char *msg) {
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

int process(jack_nframes_t nframes, void *arg) {
	int rc = 0;
	
	// Initialize the input buffers.
	void *lpin = jack_port_get_buffer(launchpad_input, nframes);
	void *clkin = jack_port_get_buffer(mclk_input, nframes);
	
	// Buffer for data we will send to the nord drum.
	void *lpout = jack_port_get_buffer(launchpad_output, nframes);
	void *ndout = jack_port_get_buffer(norddrum_output, nframes);

	// Clear the output buffer.
	jack_midi_clear_buffer(lpout);
	jack_midi_clear_buffer(ndout);

	// Process the input events.
	jack_nframes_t nlp = jack_midi_get_event_count(lpin);
	jack_nframes_t nclk = jack_midi_get_event_count(clkin);

	if (nlp == 0 && nclk == 0) {
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
	// Process the launchpad events.
	for (jack_nframes_t i = 0; i < nlp; i++) {
		jack_midi_event_t midi_event;
		
		rc = jack_midi_event_get(&midi_event, lpin, (uint32_t) i);
		if (rc != 0) {
			fprintf(stderr, "error getting launchpad MIDI event\n");
			return rc;
		}
		rc = handle_launchpad_event(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error handling launchpad MIDI event\n");
			return rc;
		}
	}
	// Process the clk events.
	for (jack_nframes_t i = 0; i < nclk; i++) {
		jack_midi_event_t midi_event;
		
		rc = jack_midi_event_get(&midi_event, clkin, (uint32_t) i);
		if (rc != 0) {
			fprintf(stderr, "error getting jack_midi_clock MIDI event\n");
			return rc;
		}
		rc = handle_clk_event(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error handling jack_midi_clock MIDI event\n");
			return rc;
		}
	}
	return 0;
}

int handle_launchpad_event(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;
	
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

	/* printf("nd out:"); */
	/* for (int j = 0; j < 3; j++) { */
	/* 	printf(" %X", ndevent[j]); */
	/* } */
	/* printf("\n"); */
	
	/* printf("lp out:"); */
	/* for (int j = 0; j < 3; j++) { */
	/* 	printf(" %X", lpevent[j]); */
	/* } */
	/* printf("\n"); */
	
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

unsigned char cell(int step) {
	return (unsigned char) (16 * (step / 8)) + (step % 8);
}

unsigned char color(int g, int r) {
	return (unsigned char) (g * 16) + r;
}

int handle_clk_event(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;
	
	if (midi_event.size < 1) {
		fprintf(stderr, "expected at least 1 bytes in MIDI message\n");
		return 1;
	}
	switch (midi_event.buffer[0]) {
	case 0xF8: // tick
		rc = tick(ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error ticking sequencer\n");
			return rc;
		}
		break;
	case 0xFB: // continue
		printf("clock start 0xFB\n");
	case 0xFA: // start
		printf("clock start 0xFA\n");
		
		rc = start(ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error starting sequencer\n");
			return rc;
		}
		break;
	case 0xFC: // stop
		printf("clock stop\n");
		break;
	/* case 0xF2: */
	/* 	if (midi_event.size == 3 && midi_event.buffer[2] == 0) { */
	/* 		printf("clock start 0xF2\n"); */
	/* 	} */
	/* 	break; */
	default:
		printf("clk event:");
		for (size_t j = 0; j < midi_event.size; j++) {
			printf(" %X", midi_event.buffer[j]);
		}
		printf("\n");
	}
	return rc;
}

int start(void *ndout, void *lpout) {
	curr = 0;
	return play(curr, ndout, lpout);
}

int play(int step, void *ndout, void *lpout) {
	int rc = 0;

	// Play the tracks for the given step.
	for (int i = 0; i < 6; i++) {
		unsigned char ndevent[3] = {};
		if (seqdata[i][curr] > 0) {
			rc = jack_midi_event_write(ndout, 0, ndevent, 3);
			if (rc != 0) {
				fprintf(stderr, "writing MIDI data to nord drum");
				return rc;
			}
		}
	}
	unsigned char lpevent[3] = {0x90, cell(curr), color(1, 1)};

	rc = jack_midi_event_write(lpout, 0, lpevent, 3);
	if (rc != 0) {
		return rc;
	}
	if (curr == 0 && 0 == prev) {
		// First time we've ever started.
		for (int i = 0; i < 64; i++) {
			unsigned char e[3] = {0x80, cell(i), 0};
			rc = jack_midi_event_write(lpout, 0, e, 8);
			if (rc != 0) {
				fprintf(stderr, "error turning off launchpad button");
				return rc;
			}
		}
	} else {
		// Turn off prev.
		unsigned char e[3] = {0x80, cell(prev), 0};
		rc = jack_midi_event_write(lpout, 0, e, 8);
		if (rc != 0) {
			fprintf(stderr, "error turning off launchpad button");
			return rc;
		}
	}
	prev = curr;
	curr++;

	if (curr >= 64) {
		curr = 0;
	}
	return 0;
}

int tick(void *ndout, void *lpout) {
	if (beat_clock % 6 == 0) {
		beat_clock++;
		return play(curr, ndout, lpout);
	}
	beat_clock++;
	return 0;
}

int updateLP() {
	int rc = 0;
	
	// Buffer for data we will send to the nord drum.
	/* void *lpout = jack_port_get_buffer(launchpad_output, 1024); */
	
	// Set the mode LED.
	/* switch (mode) { */
	/* case 1: */
	/* 	unsigned char lpevent[3] = {176, 110, color(0, 3)}; */
	/* 	rc = jack_midi_event_write(lpout, 0, lpevent, 3); */
	/* 	break; */
	/* case 2: */
	/* 	unsigned char lpevent[3] = {176, 111, color(0, 3)}; */
	/* 	rc = jack_midi_event_write(lpout, 0, lpevent, 3); */
	/* 	break; */
	/* } */
	return 0;
}

// Initializes the sequencer.
int initialize_seq() {
	int rc = 0;
	
	// Buttons 7 and 8 toggle between live trig and sequencer mode, respectively.
	// Default is live trig.
	mode = MODE_LIVE_TRIG;
	
	rc = updateLP();
	if (rc != 0) {
		fprintf(stderr, "error updating launchpad");
		return rc;
	}
	return 0;
}
