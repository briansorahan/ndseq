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
#include <jack/ringbuffer.h>

#define MODE_LIVE_TRIG (1)
#define MODE_SEQUENCER (2)

// Kills the program.
void die(const char *msg);

// Debug print midi data.
void print_midi_event(const char *source, jack_midi_event_t e);

int initialize_ports();
int connect_ports();
int initialize_seq(void *lpout);

unsigned char cell(int step); // Returns the MIDI Note value associated with step.
unsigned char color(int g, int r);

int handle_clk_event(jack_midi_event_t event, void *ndout, void *lpout);
int handle_launchpad_event(jack_midi_event_t event, void *ndout, void *lpout);
int handle_norddrum_event(jack_midi_event_t event, void *ndout, void *lpout);

int handle_grid_button(jack_midi_event_t midi_event, void *ndout, void *lpout);
int handle_live_trig(jack_midi_event_t midi_event, void *ndout, void *lpout);
int handle_letter_button(jack_midi_event_t midi_event, void *ndout, void *lpout);
int handle_scene_button(jack_midi_event_t midi_event, void *ndout, void *lpout);
int handle_track_button(jack_midi_event_t midi_event, void *ndout, void *lpout);

int switch_mode(jack_midi_event_t midi_event, void *ndout, void *lpout);
int set_grid_leds(void *lpout);
int set_track_leds(void *lpout);
int toggle_seq_step(jack_midi_event_t midi_event, void *lpout);
	
int start(void *ndout, void *lpout); // start function takes buffers for MIDI output data.
int play(int step, void *ndout, void *lpout); // play function takes buffers for MIDI output data.
int tick(void *ndout, void *lpout); // tick function takes buffers for MIDI output data.

int process(jack_nframes_t nframes, void *arg); // Process callback.

uint64_t beat_clock; // MIDI beat clock counter.

int curr; // Current step.
int prev; // Previous step.

int mode; // UI mode (live trig or sequencer).

int get_step_from(jack_midi_event_t midi_event); // Determine sequencer step based on a Launchpad grid MIDI event.
unsigned char get_cell_from(int step); // Determine MIDI note number for the given sequencer step.

int reset_launchpad(void *lpout); // Takes a JACK port buffer.
int update_launchpad(void *lpout); // Takes a JACK port buffer.

unsigned char ctrldata[6][64]; // Sequence data: 6 tracks x 64 steps.
unsigned char trigdata[6][64]; // Sequence data: 6 tracks x 64 steps.
int curr_track; // Last track that was selected.

jack_client_t *client;
jack_port_t *mclk_input; // receive jack_midi_clock data
jack_port_t *launchpad_input; // receive Launchpad MIDI events
jack_port_t *launchpad_output; // send MIDI data to the Launchpad
jack_port_t *norddrum_input; // receive MIDI data from the Nord Drum
jack_port_t *norddrum_output; // send MIDI data to the Nord Drum
jack_ringbuffer_t *norddrum_events;
jack_status_t status;

int main() {
	int rc = 0;

	norddrum_events = jack_ringbuffer_create(64 * sizeof(jack_midi_event_t)); // Arbitrary size.

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
	// Buffer for data we will send to the nord drum.
	void *lpout = jack_port_get_buffer(launchpad_output, 1024);
	
	// Reset all the buttons on the launchpad.
	rc = reset_launchpad(lpout);
	if (rc != 0) {
		die("failed to reset launchpad");
	}
	// Initialize the state of the sequencer.
	rc = initialize_seq(lpout);
	if (rc != 0) {
		die("failed to initialize sequencer");
	}
	// Wait.
	while (1) {
		sleep(1);
	}
	// TODO: cleanup in a signal handler.
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
	// Register nord drum input port (receive data from nord drum).
	norddrum_input = jack_port_register(client, "nord drum input", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	if (norddrum_input == NULL) {
		fprintf(stderr, "failed to register nord drum input port");
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
	
	const char *lpin;
	const char *lpout;
	const char *mclkout;
	const char *scarin;
	const char *scarout;

	// Discover jack inputs.
	const char **jack_inputs = jack_get_ports(client, "", "", JackPortIsInput);
	if (jack_inputs == NULL) {
		fprintf(stderr, "jack_inputs is NULL");
		return 1;
	}
	while (jack_inputs != NULL && *jack_inputs != NULL) {
		char *lpss = strstr(*jack_inputs, "Launchpad Mini");
		char *scss = strstr(*jack_inputs, "Scarlett 6i6"); // TODO: this should be a command-line flag.

		if (lpss != NULL) {
			lpin = *jack_inputs;
		} else if (scss != NULL) {
			scarin = *jack_inputs;
		}
		jack_inputs++;
	}

	// Discover jack outputs.
	const char **jack_outputs = jack_get_ports(client, "", "", JackPortIsOutput);
	if (jack_outputs == NULL) {
		fprintf(stderr, "jack_outputs is NULL");
		return 1;
	}
	while (jack_outputs != NULL && *jack_outputs != NULL) {
		char *lpss = strstr(*jack_outputs, "Launchpad Mini");
		char *mclkss = strstr(*jack_outputs, "jack_midi_clock");
		char *scss = strstr(*jack_outputs, "Scarlett 6i6"); // TODO: this should be a command-line flag.

 		if (lpss != NULL) {
			lpout = *jack_outputs;
		} else if (mclkss != NULL) {
			mclkout = *jack_outputs;
		} else if (scss != NULL) {
			scarout = *jack_outputs;
		}
		jack_outputs++;
	}

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
	// Send MIDI to the nord drum.
	rc = jack_connect(client, jack_port_name(norddrum_output), scarin);
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to scarlett 6i6 input: %s\n", scarin);
		return rc;
	}
	// Receive MIDI from the nord drum.
	rc = jack_connect(client, scarout, jack_port_name(norddrum_input));
	if (rc != 0) {
		fprintf(stderr, "Failed to connect to scarlett 6i6 output: %s\n", scarout);
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
	void *clkin = jack_port_get_buffer(mclk_input, nframes);
	void *lpin = jack_port_get_buffer(launchpad_input, nframes);
	void *ndin = jack_port_get_buffer(norddrum_input, nframes);
	
	// Buffer for data we will send to the nord drum.
	void *lpout = jack_port_get_buffer(launchpad_output, nframes);
	void *ndout = jack_port_get_buffer(norddrum_output, nframes);

	// Clear the output buffer.
	jack_midi_clear_buffer(lpout);
	jack_midi_clear_buffer(ndout);

	// Process the input events.
	jack_nframes_t nclk = jack_midi_get_event_count(clkin);
	jack_nframes_t nlp = jack_midi_get_event_count(lpin);
	jack_nframes_t nnd = jack_midi_get_event_count(ndin);

	if (nclk == 0 && nlp == 0 && nnd == 0) {
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
	if (norddrum_events == NULL) {
		fprintf(stderr, "allocating array of nord drum MIDI events");
		return 1;
	}
	// Process the nord drum events.
	// This will store MIDI CC data for the current step.
	// If we handle the nord drum before the clk events this means that
	// when recording controller data we should try to tweak the controller just ahead of the trigs.
	/* for (jack_nframes_t i = 0; i < nnd; i++) { */
	/* 	jack_midi_event_t *midi_event = malloc(sizeof(jack_midi_event_t)); */

	/* 	rc = jack_midi_event_get(midi_event, ndin, (uint32_t) i); */
	/* 	if (rc != 0) { */
	/* 		fprintf(stderr, "error getting nord drum MIDI event\n"); */
	/* 		return rc; */
	/* 	} */
	/* 	size_t written = jack_ringbuffer_write(norddrum_events, (void *) midi_event, sizeof(jack_midi_event_t)); */
		
	/* 	if (written < sizeof(jack_midi_event_t)) { */
	/* 		fprintf(stderr, "wrote less bytes than expected to norddrum_events ringbuffer\n"); */
	/* 		return 1; */
	/* 	} */
	/* } */
	// Process the clk events.
	// This will move the sequencer's internal state forward!
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

	// Handy for looking at raw MIDI data.
	/* print_midi_event("launchpad", midi_event); */
	
	// We always expect at least 3 bytes.
	if (midi_event.size < 3) {
		fprintf(stderr, "expected at least 3 bytes in MIDI message\n");
		return 1;
	}
	// "Scene Launch" button.
	if (midi_event.buffer[0] == 0xB0) {
		rc = handle_scene_button(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error switch modes\n");
		}
		return rc;
	}
	// "Letter" button.
	if ((midi_event.buffer[1] & 0x08) == 8) {
		rc = handle_letter_button(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "handle letter button\n");
		}
		return rc;
	}
	/* printf(">>> grid button\n"); */
	rc = handle_grid_button(midi_event, ndout, lpout);
	if (rc != 0) {
		fprintf(stderr, "handle grid button\n");
		return rc;
	}
	return 0;
}

int handle_grid_button(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;
	
	switch (mode) {
	case MODE_SEQUENCER:
		/* printf(">>> sequencer mode\n"); */
		
		// Ignore button up events.
		if (midi_event.buffer[0] == 0x80) {
			return 0;
		}
		// Toggle the sequencer step for the current track.
		rc = toggle_seq_step(midi_event, lpout);
		if (rc != 0) {
			fprintf(stderr, "toggling sequencer step\n");
			return rc;
		}
		break;
	case MODE_LIVE_TRIG:
		rc = handle_live_trig(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "handle_grid_button handling live trig\n");
			return rc;
		}
		break;
	}
}

int handle_live_trig(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;

	/* printf(">>> handle_live_trig track = %d, velocity = %d\n", (midi_event.buffer[1] % 8)+1, (112 - (midi_event.buffer[1] & 0xF0)) + 15); */
	
	unsigned char ndevent[3] = {midi_event.buffer[0] + (midi_event.buffer[1] % 8), 60, (112 - (midi_event.buffer[1] & 0xF0)) + 15};
	unsigned char lpevent[3] = {midi_event.buffer[0], midi_event.buffer[1], color(3, 0)};

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

int handle_letter_button(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	// TODO: what should A-H do?
	return 0;
}

int handle_scene_button(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;

	switch (midi_event.buffer[1] % 8) {
	case 6:
		// Switch modes on button down only.
		if (midi_event.buffer[2] == 0) {
			return 0;
		}
		rc = switch_mode(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "handle_scene_button switching mode\n");
			return rc;
		}
		break;
	case 7:
		// TODO: what will this button do?
		break;
	default:
		// Switch tracks (doesn't do anything in live trig mode, but perhaps it should).
		rc = handle_track_button(midi_event, ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "calling handle_track_button\n");
			return rc;
		}
	}
	return 0;
}

int handle_track_button(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;

	// Update internal state of the sequencer.
	curr_track = midi_event.buffer[1] - 104;
	
	// Update the track buttons.
	for (int i = 0; i < 6; i++) {
		unsigned char e[3] = {0xB0, 104+i, 0};
		if (i == curr_track) {
			e[2] = color(3, 0);
		}
		rc = jack_midi_event_write(lpout, 0, e, 3);
		if (rc != 0) {
			fprintf(stderr, "handle_track_button updating track button\n");
			return rc;
		}
	}
	// Update the grid with the current track's sequencer data.
	for (int i = 0; i < 64; i++) {
		unsigned char e[3] = {0x90, get_cell_from(i), 0};
		if (trigdata[curr_track][i]) {
			e[2] = color(3, 0);
		}
		rc = jack_midi_event_write(lpout, 0, e, 3);
		if (rc != 0) {
			fprintf(stderr, "handle_track_button updating grid button\n");
			return rc;
		}
	}
	return 0;
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
		/* printf("clock start 0xFB\n"); */
	case 0xFA: // start
		/* printf("clock start 0xFA\n"); */
		rc = start(ndout, lpout);
		if (rc != 0) {
			fprintf(stderr, "error starting sequencer\n");
			return rc;
		}
		break;
	case 0xFC: // stop
		/* printf("clock stop\n"); */
		break;
		/* case 0xF2: */
		/* 	if (midi_event.size == 3 && midi_event.buffer[2] == 0) { */
		/* 		printf("clock start 0xF2\n"); */
		/* 	} */
		/* 	break; */
	/* default: */
	/* 	printf("clk event:"); */
	/* 	for (size_t j = 0; j < midi_event.size; j++) { */
	/* 		printf(" %X", midi_event.buffer[j]); */
	/* 	} */
	/* 	printf("\n"); */
	}
	// Process queued nord drum events.
	while (jack_ringbuffer_read_space(norddrum_events) >= sizeof(jack_midi_event_t)) {
		jack_midi_event_t *ndevent = malloc(sizeof(jack_midi_event_t));
		size_t bytes_read = jack_ringbuffer_read(norddrum_events, (char *) ndevent, sizeof(jack_midi_event_t));
		if (bytes_read < sizeof(jack_midi_event_t)) {
			fprintf(stderr, "handle_clk_event reading nord drum MIDI event: expected to read %ld bytes, actually read %ld\n", sizeof(jack_midi_event_t), bytes_read);
			return 1;
		}
		free(ndevent);
	}
	return rc;
}

int handle_norddrum_event(jack_midi_event_t event, void *ndout, void *lpout) {
	print_midi_event("nord drum", event);
	return 0;
}

int start(void *ndout, void *lpout) {
	curr = 0;
	return play(curr, ndout, lpout);
}

// nudge_seq updates just the internal state of the sequencer.
// It does not do any MIDI I/O!
void nudge_seq() {
	prev = curr;
	curr++;

	if (curr >= 64) {
		curr = 0;
	}
}

// play a sequencer step.
// This function is only called in response to clock events.
int play(int step, void *ndout, void *lpout) {
	int rc = 0;

	// Play the tracks for the given step.
	for (int i = 0; i < 6; i++) {
		unsigned char ndevent[3] = {0x90+i, 60, 127};
		if (trigdata[i][curr]) {
			rc = jack_midi_event_write(ndout, 0, ndevent, 3);
			if (rc != 0) {
				fprintf(stderr, "writing MIDI data to nord drum\n");
				return rc;
			}
		}
	}
	// If we are in live trig mode then clock events have no effect on the grid.
	// Note that we still need to advance the sequencer though.
	if (mode == MODE_LIVE_TRIG) {
		nudge_seq();
		return 0;
	}
	unsigned char lpevent[3] = {0x90, cell(curr), color(1, 1)};

	rc = jack_midi_event_write(lpout, 0, lpevent, 3);
	if (rc != 0) {
		fprintf(stderr, "play: writing MIDI data to launchpad\n");
		return rc;
	}
	if (curr == 0 && 0 == prev) {
		// First time we've ever started.
		// Assume all the sequencer data is 0, so we don't need to turn any buttons off.
		for (int i = 0; i < 64; i++) {
			unsigned char e[3] = {0x80, cell(i), 0};
			rc = jack_midi_event_write(lpout, 0, e, 8);
			if (rc != 0) {
				fprintf(stderr, "play: error turning off launchpad button\n");
				return rc;
			}
		}
	} else {
		unsigned char prev_color = 0;
		if (trigdata[curr_track][prev]) {
			prev_color = color(3, 0);
		}
		// Turn off prev.
		/* printf(">>> cell(prev) = %d\n", cell(prev)); */
		unsigned char e[3] = {0x90, cell(prev), prev_color};
		rc = jack_midi_event_write(lpout, 0, e, 8);
		if (rc != 0) {
			fprintf(stderr, "error turning off launchpad button\n");
			return rc;
		}
	}
	nudge_seq();
	
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

int reset_launchpad(void *lpout) {
	int rc = 0;
	
	unsigned char lpevent[3] = {176, 0, 0};
	
	rc = jack_midi_event_write(lpout, 0, lpevent, 3);
	if (rc != 0) {
		fprintf(stderr, "resetting launchpad\n");
		return rc;
	}
	return 0;
}

int update_launchpad(void *lpout) {
	int rc = 0;

	unsigned char lpevent[3] = {176, 110, 0};
	
	// Set the mode LED.
	switch (mode) {
	case MODE_LIVE_TRIG:
		// Turn off the grid buttons.
		for (int i = 0; i < 64; i++) {
			unsigned char e[3] = {0x90, get_cell_from(i), 0};
			rc = jack_midi_event_write(lpout, 0, e, 3);
			if (rc != 0) {
				fprintf(stderr, "update_launchpad turning grid button off\n");
				return rc;
			}
		}
		// Turn off the track buttons.
		for (int i = 0; i < 6; i++) {
			unsigned char e[3] = {0xB0, 104+i, 0};
			rc = jack_midi_event_write(lpout, 0, e, 3);
			if (rc != 0) {
				fprintf(stderr, "update_launchpad turning track button off\n");
				return rc;
			}
		}
		lpevent[2] = color(3, 0); // g, r
		break;
	case MODE_SEQUENCER:
		// If we're in sequencer mode then scene buttons 1-6 indicate the currently selected sequencer track.
		rc = set_track_leds(lpout);
		if (rc != 0) {
			fprintf(stderr, "update_launchpad set track LED's\n");
			return rc;
		}
		// Set the grid based on the sequencer data of the current track.
		rc = set_grid_leds(lpout);
		if (rc != 0) {
			fprintf(stderr, "update_launchpad setting grid\n");
			return rc;
		}
		lpevent[2] = color(3, 3); // g, r
		break;
	}
	rc = jack_midi_event_write(lpout, 0, lpevent, 3);
	if (rc != 0) {
		fprintf(stderr, "updating launchpad button\n");
		return rc;
	}
	return 0;
}

// Initializes the sequencer.
int initialize_seq(void *lpout) {
	int rc = 0;

	// Initialize sequencer data to be all zeroes.
	for (int i = 0; i < 6; i++) {
		for (int j = 0; j < 64; j++) {
			trigdata[i][j] = 0;
		}
	}

	// Default to having the first track selected.
	curr_track = 0;
	
	// Button 7 toggles between live trig and sequencer mode. Default is sequencer.
	mode = MODE_SEQUENCER;
	
	rc = update_launchpad(lpout);
	if (rc != 0) {
		fprintf(stderr, "error updating launchpad\n");
		return rc;
	}
	return 0;
}

// get the sequencer step from a MIDI event.
// This event should be a grid button on the launchpad.
int get_step_from(jack_midi_event_t midi_event) {
	return (midi_event.buffer[1] % 8) + ((midi_event.buffer[1] & 0xF0) / 2);
}

// get_cell_from gets the MIDI Note value that indicates a cell in the grid based on a step value.
unsigned char get_cell_from(int step) {
	return (16 * (step / 8)) + (step % 8);
}

// toggle a sequencer step based on the push of a grid button.
// Note that we assume this is a "button down" event.
int toggle_seq_step(jack_midi_event_t midi_event, void *lpout) {
	int rc = 0;
	int step = get_step_from(midi_event);
	int val = trigdata[curr_track][step];

	// Set the internal state of the sequencer.
	trigdata[curr_track][step] = !val;

	unsigned char e[3] = {midi_event.buffer[0], midi_event.buffer[1], color(0, 0)};

	if (trigdata[curr_track][step]) {
		e[2] = color(3, 0);
	}
	rc = jack_midi_event_write(lpout, 0, e, 3);
	if (rc != 0) {
		fprintf(stderr, "toggling sequencer step\n");
		return rc;
	}
	return 0;
}

int set_grid_leds(void *lpout) {
	int rc = 0;
	
	for (int i = 0; i < 64; i++) {
		unsigned char e[3] = {0x90, get_cell_from(i), 0};
		if (trigdata[curr_track][i]) {
			e[2] = color(3, 0);
		}
		rc = jack_midi_event_write(lpout, 0, e, 3);
		if (rc != 0) {
			fprintf(stderr, "set_grid_leds sending MIDI data to launchpad\n");
			return rc;
		}
	}
	return 0;
}

// Sets the track LED's based on the internal sequencer data (curr_track).
int set_track_leds(void *lpout) {
	int rc = 0;
	
	for (int i = 0; i < 6; i++) {
		unsigned char lpevent[3] = {0xB0, i+104, 0x00};
	
		if (i == curr_track) {
			lpevent[2] = color(3, 0);
		}
		rc = jack_midi_event_write(lpout, 0, lpevent, 3);
		if (rc != 0) {
			fprintf(stderr, "setting track LED %d\n", i);
			return rc;
		}
	}
	return 0;
}

// Switches launchpad "modes".
int switch_mode(jack_midi_event_t midi_event, void *ndout, void *lpout) {
	int rc = 0;
	
	switch (mode) {
	case MODE_LIVE_TRIG:
		mode = MODE_SEQUENCER;
		break;
	case MODE_SEQUENCER:
		mode = MODE_LIVE_TRIG;
		break;
	}
	rc = update_launchpad(lpout);
	if (rc != 0) {
		fprintf(stderr, "switch_mode updating launchpad\n");
		return rc;
	}
	return 0;
}

inline unsigned char cell(int step) {
	return (unsigned char) (16 * (step / 8)) + (step % 8);
}

inline unsigned char color(int g, int r) {
	return (unsigned char) (g * 16) + r;
}

void print_midi_event(const char *source, jack_midi_event_t e) {
	printf("%s:", source);
	for (size_t j = 0; j < e.size; j++) {
		printf(" %X", e.buffer[j]);
	}
	printf("\n");
}
