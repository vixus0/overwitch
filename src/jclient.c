/*
 *   jclient.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Overwitch.
 *
 *   Overwitch is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Overwitch is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Overwitch. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <jack/jack.h>
#include <jack/intclient.h>
#include <jack/thread.h>
#include <jack/midiport.h>

#include "utils.h"
#include "jclient.h"

#define MSG_ERROR_PORT_REGISTER "Error while registering JACK port"

#define MAX_MIDI_BUF_LEN OB_MIDI_BUF_LEN

#define MAX_LATENCY (8192 * 2)	//This is twice the maximum JACK latency.

void
squeue_init (struct squeue *queue, uint32_t max_len)
{
  queue->max_len = max_len;
  queue->data = malloc (max_len);
  queue->len = 0;
}

void
squeue_destroy (struct squeue *queue)
{
  free (queue->data);
}

inline int
squeue_write (struct squeue *queue, void *data, uint32_t len)
{
  if (queue->len + len >= queue->max_len)
    {
      error_print ("o2j: Not enough space in queue. Resetting...");
      queue->len = 0;
      return 1;
    }

  memcpy (queue->data + queue->len, data, len);
  queue->len += len;
  return 0;
}

inline void
squeue_read (struct squeue *queue, void *data)
{
  memcpy (data, queue->data, queue->len);
  queue->len = 0;
}

void
squeue_consume (struct squeue *queue, uint32_t consumed)
{
  queue->len -= consumed;
  memcpy (queue->data, queue->data + consumed, queue->len);
}

size_t
jclient_buffer_read (void *buffer, char *src, size_t size)
{
  if (src)
    {
      return jack_ringbuffer_read (buffer, src, size);
    }
  else
    {
      jack_ringbuffer_read_advance (buffer, size);
      return 0;
    }
}

static int
jclient_thread_xrun_cb (void *cb_data)
{
  struct ow_resampler *resampler = cb_data;
  error_print ("JACK xrun");
  ow_resampler_inc_xruns (resampler);
  return 0;
}

static void
jclient_thread_latency_cb (jack_latency_callback_mode_t mode, void *cb_data)
{
  jack_latency_range_t range;
  struct jclient *jclient = cb_data;
  size_t latency, min_latency, max_latency;
  struct ow_engine *engine = ow_resampler_get_engine (jclient->resampler);
  const struct ow_device_desc *desc = ow_engine_get_device_desc (engine);

  debug_print (2, "JACK latency request");

  if (mode == JackPlaybackLatency)
    {
      debug_print (2, "Recalculating input to output latency...");
      for (int i = 0; i < desc->outputs; i++)
	{
	  jack_port_get_latency_range (jclient->input_ports[0], mode, &range);
	  ow_resampler_get_o2h_latency (jclient->resampler, &latency,
					&min_latency, &max_latency);
	  range.min += min_latency;
	  range.max += max_latency;
	  jack_port_set_latency_range (jclient->output_ports[i], mode,
				       &range);
	}
    }
  else if (mode == JackCaptureLatency)
    {
      debug_print (2, "Recalculating output to input latency...");
      for (int i = 0; i < desc->inputs; i++)
	{
	  jack_port_get_latency_range (jclient->output_ports[0], mode,
				       &range);
	  ow_resampler_get_h2o_latency (jclient->resampler, &latency,
					&min_latency, &max_latency);
	  range.min += min_latency;
	  range.max += max_latency;
	  jack_port_set_latency_range (jclient->input_ports[i], mode, &range);
	}
    }
}

static void
jclient_port_connect_cb (jack_port_id_t a, jack_port_id_t b, int connect,
			 void *cb_data)
{
  int total_connections = 0;
  struct jclient *jclient = cb_data;
  struct ow_engine *engine = ow_resampler_get_engine (jclient->resampler);
  const struct ow_device_desc *desc = ow_engine_get_device_desc (engine);

  for (int i = 0; i < desc->inputs; i++)
    {
      total_connections += jack_port_connected (jclient->input_ports[i]);
    }

  ow_engine_set_option (engine, OW_ENGINE_OPTION_P2O_AUDIO,
			total_connections != 0);

  for (int i = 0; i < desc->outputs; i++)
    {
      total_connections += jack_port_connected (jclient->output_ports[i]);
    }

  if (!total_connections)
    {
      ow_resampler_clear_buffers (jclient->resampler);
    }
}

static void
jclient_jack_shutdown_cb (jack_status_t code, const char *reason,
			  void *cb_data)
{
  struct jclient *jclient = cb_data;
  debug_print (1, "JACK is shutting down: %s", reason);
  jclient_stop (jclient);
}

static void
jclient_jack_freewheel (int starting, void *cb_data)
{
  debug_print (1, "JACK in freewheel mode: %d", starting);
}

static int
jclient_jack_graph_order_cb (void *cb_data)
{
  debug_print (1, "JACK calling graph order...");
  return 0;
}

static void
jclient_jack_client_registration_cb (const char *name, int op, void *cb_data)
{
  debug_print (1, "JACK client %s is being %s...", name,
	       op ? "registered" : "unregistered");
}

static int
jclient_set_buffer_size_cb (jack_nframes_t nframes, void *cb_data)
{
  struct jclient *jclient = cb_data;
  debug_print (1, "JACK buffer size: %d", nframes);
  jclient->bufsize = nframes;
  ow_resampler_set_buffer_size (jclient->resampler, nframes);
  return 0;
}

static int
jclient_set_sample_rate_cb (jack_nframes_t nframes, void *cb_data)
{
  struct jclient *jclient = cb_data;
  debug_print (1, "JACK sample rate: %d", nframes);
  ow_resampler_set_samplerate (jclient->resampler, nframes);
  return 0;
}

static inline void
jclient_o2j_midi (struct jclient *jclient, jack_nframes_t nframes)
{
  void *midi_port_buf;
  jack_midi_data_t *jmidi;
  struct ow_midi_event event;
  jack_nframes_t last_frame, jack_frame;
  int send = 0;
  uint32_t len, lost_count;
  int64_t frame;

  midi_port_buf = jack_port_get_buffer (jclient->midi_output_port, nframes);
  jack_midi_clear_buffer (midi_port_buf);

  last_frame = jack_last_frame_time (jclient->client);

  while (jack_ringbuffer_read_space (jclient->context.o2h_midi) >=
	 sizeof (struct ow_midi_event))
    {
      jack_ringbuffer_peek (jclient->context.o2h_midi, (void *) &event,
			    sizeof (struct ow_midi_event));

      // We add 1 JACK cycle because it's the maximum delay we want to achieve
      // as everyting generated during the previous cycle will always be played.
      // If we tried to adjust it automatically we'd get 1 cycle delay.
      jack_frame = jack_time_to_frames (jclient->client, event.time) +
	nframes;

      debug_print (3, "o2j: last frame: %u", last_frame);
      debug_print (3, "o2j: JACK frame: %u", jack_frame);

      if (jack_frame < last_frame)
	{
	  frame = 0;
	  debug_print (2, "o2j: Processing missed event @ %lu us...", frame);
	}
      else
	{
	  frame = jack_frame - last_frame;
	  if (frame >= nframes)
	    {
	      debug_print (2,
			   "o2j: Skipping until the next cycle (event frames %lu)...",
			   frame);
	      break;
	    }
	}

      debug_print (2, "o2j: Event frames: %lu", frame);

      jack_ringbuffer_read_advance (jclient->context.o2h_midi,
				    sizeof (struct ow_midi_event));
      switch (event.packet.header)
	{
	case 0x04:
	  len = 3;
	  send = 0;
	  if (jclient->o2j_midi_skipping)
	    {
	      continue;
	    }
	  break;
	case 0x05:
	  len = 1;
	  send = 1;
	  if (jclient->o2j_midi_skipping)
	    {
	      jclient->o2j_midi_skipping = 0;
	      continue;
	    }
	  break;
	case 0x06:
	  len = 2;
	  send = 1;
	  if (jclient->o2j_midi_skipping)
	    {
	      jclient->o2j_midi_skipping = 0;
	      continue;
	    }
	  break;
	case 0x07:
	  len = 3;
	  send = 1;
	  if (jclient->o2j_midi_skipping)
	    {
	      jclient->o2j_midi_skipping = 0;
	      continue;
	    }
	  break;
	case 0x0c:		//Program Change
	case 0x0d:		//Channel Pressure (After-touch)
	  len = 2;
	  send = 1;
	  jclient->o2j_midi_skipping = 0;
	  break;
	case 0x08:		//Note Off
	case 0x09:		//Note On
	case 0x0a:		//Polyphonic Key Pressure
	case 0x0b:		//Control Change
	case 0x0e:		//Pitch Bend Change
	  len = 3;
	  send = 1;
	  jclient->o2j_midi_skipping = 0;
	  break;
	case 0x0f:		//Single Byte SysEx
	  len = 1;
	  send = 1;
	  jclient->o2j_midi_skipping = 0;
	  break;
	default:
	  error_print ("o2j: Message %02X not implemented",
		       event.packet.header);
	  jclient->o2j_midi_queue.len = 0;
	  jclient->o2j_midi_skipping = 0;
	  continue;
	}

      debug_print (3,
		   "o2j MIDI packet: %02x %02x %02x %02x @ %lu us",
		   event.packet.header, event.packet.data[0],
		   event.packet.data[1], event.packet.data[2], event.time);

      if (squeue_write (&jclient->o2j_midi_queue, event.packet.data, len))
	{
	  jclient->o2j_midi_skipping = 1;	//No space. We skip the current message being sent.
	  continue;
	}

      if (send)
	{
	  jmidi = jack_midi_event_reserve (midi_port_buf, frame,
					   jclient->o2j_midi_queue.len);
	  if (jmidi)
	    {
	      debug_print (2, "o2j: Processing MIDI event @ %lu (%d B)",
			   frame, jclient->o2j_midi_queue.len);
	      squeue_read (&jclient->o2j_midi_queue, jmidi);
	    }
	  else
	    {
	      error_print ("o2j: JACK could not reserve event");
	      jclient->o2j_midi_queue.len = 0;
	    }
	}

      lost_count = jack_midi_get_lost_event_count (midi_port_buf);
      if (lost_count > jclient->o2j_last_lost_count)
	{
	  jclient->o2j_last_lost_count = lost_count;
	  error_print ("Lost event count: %d", jclient->o2j_last_lost_count);
	}
    }
}

static inline void
jclient_j2o_midi_queue_event (struct jclient *jclient,
			      struct ow_midi_event *event)
{
  if (jack_ringbuffer_write_space (jclient->context.h2o_midi) >=
      sizeof (struct ow_midi_event))
    {
      debug_print (3,
		   "j2o: MIDI packet: %02x %02x %02x %02x @ %lu us",
		   event->packet.header, event->packet.data[0],
		   event->packet.data[1], event->packet.data[2], event->time);

      jack_ringbuffer_write (jclient->context.h2o_midi,
			     (void *) event, sizeof (struct ow_midi_event));
    }
  else
    {
      error_print ("j2o: MIDI ring buffer overflow. Discarding data...");
    }
}

static inline void
jclient_copy_event_bytes (struct ow_midi_event *oevent,
			  jack_midi_data_t *buffer, size_t len)
{
  memcpy (oevent->packet.data, buffer, len);
  memset (oevent->packet.data + len, 0, OB_MIDI_EVENT_BYTES - len);
}

static inline void
jclient_j2o_midi_msg (struct jclient *jclient, jack_midi_event_t *jevent,
		      jack_time_t time)
{
  struct ow_midi_event oevent;
  jack_midi_data_t status_byte = jevent->buffer[0];
  jack_midi_data_t type = status_byte & 0xf0;

  oevent.packet.header = 0;

  debug_print (2, "j2o: Sending MIDI message...");

  if (jevent->size == 1)
    {
      if (status_byte >= 0xf8 && status_byte <= 0xfc)
	{
	  oevent.packet.header = 0x0f;	//Single Byte SysEx
	}
    }
  else if (jevent->size == 2)
    {
      switch (type)
	{
	case 0xc0:		//Program Change
	  oevent.packet.header = 0x0c;
	  break;
	case 0xd0:		//Channel Pressure (After-touch)
	  oevent.packet.header = 0x0d;
	  break;
	}
    }
  else				// jevent->size == 3
    {
      switch (type)
	{
	case 0x80:		//Note Off
	  oevent.packet.header = 0x08;
	  break;
	case 0x90:		//Note On
	  oevent.packet.header = 0x09;
	  break;
	case 0xa0:		//Polyphonic Key Pressure
	  oevent.packet.header = 0x0a;
	  break;
	case 0xb0:		//Control Change
	  oevent.packet.header = 0x0b;
	  break;
	case 0xe0:		//Pitch Bend Change
	  oevent.packet.header = 0x0e;
	  break;
	}
    }

  if (oevent.packet.header)
    {
      oevent.time = time;
      jclient_copy_event_bytes (&oevent, jevent->buffer, jevent->size);
      jclient_j2o_midi_queue_event (jclient, &oevent);
    }
  else
    {
      error_print ("j2o: Message %02x not implemented", type);
    }
}

//Multiple byte SysEx

static inline void
jclient_j2o_midi_sysex (struct jclient *jclient, jack_midi_event_t *jevent,
			jack_time_t time)
{
  int consumed;
  uint8_t *b;

  if (squeue_write (&jclient->j2o_midi_queue, jevent->buffer, jevent->size))
    {
      return;
    }

  debug_print (2, "j2o: Sending MIDI SysEx packets...");

  b = jclient->j2o_midi_queue.data;
  consumed = 0;
  while (consumed < jclient->j2o_midi_queue.len)
    {
      struct ow_midi_event oevent;
      size_t plen = 0;
      int end = 0;
      uint8_t *start = b;
      oevent.packet.header = 0x04;
      oevent.time = time;

      for (int i = 0; i < OB_MIDI_EVENT_BYTES &&
	   consumed + i < jclient->j2o_midi_queue.len; i++, b++, plen++)
	{
	  if (*b == 0xf7)
	    {
	      switch (i)
		{
		case 0:
		  oevent.packet.header = 0x05;
		  break;
		case 1:
		  oevent.packet.header = 0x06;
		  break;
		default:	// 2
		  oevent.packet.header = 0x07;
		}

	      end = 1;
	      plen = i + 1;
	      jclient->j2o_ongoing_sysex = 0;

	      debug_print (2,
			   "j2o: MIDI packet (%ld): %02x %02x %02x %02x @ %lu us",
			   plen, oevent.packet.header, *start, *(start + 1),
			   *(start + 2), oevent.time);

	      break;
	    }
	}

      if (!end && plen < OB_MIDI_EVENT_BYTES)
	{
	  break;
	}

      jclient_copy_event_bytes (&oevent, start, plen);
      jclient_j2o_midi_queue_event (jclient, &oevent);
      consumed += plen;
    }

  squeue_consume (&jclient->j2o_midi_queue, consumed);

  debug_print (2, "j2o: SysEx message pending bytes: %d",
	       jclient->j2o_midi_queue.len);
}

static inline void
jclient_j2o_midi (struct jclient *jclient, jack_nframes_t nframes,
		  jack_nframes_t current_frames)
{
  jack_midi_event_t jevent;
  void *midi_port_buf;
  jack_nframes_t event_count;
  jack_time_t time = jack_frames_to_time (jclient->client, current_frames);

  midi_port_buf = jack_port_get_buffer (jclient->midi_input_port, nframes);
  event_count = jack_midi_get_event_count (midi_port_buf);

  for (int i = 0; i < event_count; i++)
    {
      if (!jack_midi_event_get (&jevent, midi_port_buf, i))
	{
	  debug_print (2, "j2o: Processing MIDI event @ %u (%zu B)",
		       jevent.time, jevent.size);
	  if (jevent.buffer[0] == 0xf0 || jclient->j2o_ongoing_sysex)
	    {
	      jclient->j2o_ongoing_sysex = 1;
	      jclient_j2o_midi_sysex (jclient, &jevent, time);
	    }
	  else
	    {
	      jclient_j2o_midi_msg (jclient, &jevent, time);
	    }
	}
    }
}

inline void
jclient_copy_o2j_audio (float *f, jack_nframes_t nframes,
			jack_default_audio_sample_t *buffer[],
			const struct ow_device_desc *desc)
{
  for (int i = 0; i < nframes; i++)
    {
      for (int j = 0; j < desc->outputs; j++)
	{
	  buffer[j][i] = *f;
	  f++;
	}
    }
}

inline void
jclient_copy_j2o_audio (float *f, jack_nframes_t nframes,
			jack_default_audio_sample_t *buffer[],
			const struct ow_device_desc *desc)
{
  for (int i = 0; i < nframes; i++)
    {
      for (int j = 0; j < desc->inputs; j++)
	{
	  *f = buffer[j][i];
	  f++;
	}
    }
}

static void
jclient_audio_running (void *data)
{
  jack_recompute_total_latencies (data);
}

static inline int
jclient_process_cb (jack_nframes_t nframes, void *arg)
{
  float *f;
  jack_default_audio_sample_t *buffer[OB_MAX_TRACKS];
  struct jclient *jclient = arg;
  jack_nframes_t current_frames;
  jack_time_t current_usecs;
  jack_time_t next_usecs;
  float period_usecs;
  struct ow_engine *engine = ow_resampler_get_engine (jclient->resampler);
  const struct ow_device_desc *desc = ow_engine_get_device_desc (engine);

  if (jack_get_cycle_times (jclient->client, &current_frames, &current_usecs,
			    &next_usecs, &period_usecs))
    {
      error_print ("Error while getting JACK time");
    }

  //MIDI runs independently of audio status
  jclient_o2j_midi (jclient, nframes);
  jclient_j2o_midi (jclient, nframes, current_frames);

  if (ow_resampler_compute_ratios (jclient->resampler, current_usecs,
				   jclient_audio_running, jclient->client))
    {
      return 0;
    }

  //o2h

  for (int i = 0; i < desc->outputs; i++)
    {
      buffer[i] = jack_port_get_buffer (jclient->output_ports[i], nframes);
    }

  f = ow_resampler_get_o2h_audio_buffer (jclient->resampler);
  ow_resampler_read_audio (jclient->resampler);
  jclient_copy_o2j_audio (f, nframes, buffer, desc);

  //h2o

  if (ow_engine_is_option (engine, OW_ENGINE_OPTION_P2O_AUDIO))
    {
      for (int i = 0; i < desc->inputs; i++)
	{
	  buffer[i] = jack_port_get_buffer (jclient->input_ports[i], nframes);
	}

      f = ow_resampler_get_h2o_audio_buffer (jclient->resampler);
      jclient_copy_j2o_audio (f, nframes, buffer, desc);
      ow_resampler_write_audio (jclient->resampler);
    }

  return 0;
}

static void
set_rt_priority (pthread_t thread, int priority)
{
  int err = jack_acquire_real_time_scheduling (thread, priority);
  if (err)
    {
      error_print ("Could not set real time priority");
    }
}

void
jclient_stop (struct jclient *jclient)
{
  debug_print (1, "Stopping client...");
  if (jclient->client)
    {
      ow_resampler_report_status (jclient->resampler);
      ow_resampler_stop (jclient->resampler);
    }
}

int
jclient_init (struct jclient *jclient)
{
  struct ow_resampler *resampler;
  struct ow_engine *engine;
  ow_err_t err = ow_resampler_init_from_bus_address (&resampler, jclient->bus,
						     jclient->address,
						     jclient->blocks_per_transfer,
						     jclient->xfr_timeout,
						     jclient->quality);
  jclient->running = 0;

  if (err)
    {
      error_print ("Overwitch error: %s", ow_get_err_str (err));
      return -1;
    }

  jclient->resampler = resampler;
  engine = ow_resampler_get_engine (jclient->resampler);
  jclient->name = ow_engine_get_overbridge_name (engine);

  return 0;
}

void
jclient_destroy (struct jclient *jclient)
{
  ow_resampler_destroy (jclient->resampler);
}

int
jclient_run (struct jclient *jclient)
{
  jack_status_t status;
  ow_err_t err = OW_OK;
  char *client_name;
  struct ow_engine *engine;
  const struct ow_device_desc *desc;

  jclient->output_ports = NULL;
  jclient->input_ports = NULL;
  jclient->j2o_ongoing_sysex = 0;
  jclient->context.h2o_audio = NULL;
  jclient->context.o2h_audio = NULL;
  jclient->context.h2o_midi = NULL;
  jclient->context.o2h_midi = NULL;

  engine = ow_resampler_get_engine (jclient->resampler);
  desc = ow_engine_get_device_desc (engine);

  jclient->client = jack_client_open (jclient->name, JackNoStartServer,
				      &status, NULL);
  if (jclient->client == NULL)
    {
      if (status & JackServerFailed)
	{
	  error_print ("Unable to connect to JACK server");
	}
      else
	{
	  error_print ("Unable to open client. Error 0x%2.0x", status);
	}
      return OW_GENERIC_ERROR;
    }

  if (status & JackServerStarted)
    {
      debug_print (1, "JACK server started");
    }

  if (status & JackNameNotUnique)
    {
      client_name = jack_get_client_name (jclient->client);
      debug_print (0, "Name client in use. Using %s...", client_name);
    }

  if (jack_set_process_callback (jclient->client, jclient_process_cb,
				 jclient))
    {
      goto cleanup_jack;
    }

  if (jack_set_xrun_callback (jclient->client, jclient_thread_xrun_cb,
			      jclient->resampler))
    {
      goto cleanup_jack;
    }

  if (jack_set_latency_callback (jclient->client, jclient_thread_latency_cb,
				 jclient))
    {
      goto cleanup_jack;
    }

  if (jack_set_port_connect_callback (jclient->client,
				      jclient_port_connect_cb, jclient))
    {
      error_print
	("Cannot set port connect callback so j2o audio will not be possible");
    }

  jack_on_info_shutdown (jclient->client, jclient_jack_shutdown_cb, jclient);

  if (jack_set_freewheel_callback (jclient->client, jclient_jack_freewheel,
				   jclient))
    {
      error_print ("Cannot set JACK freewheel callback");
    }

  if (jack_set_graph_order_callback (jclient->client,
				     jclient_jack_graph_order_cb, jclient))
    {
      error_print ("Cannot set JACK graph order callback");
    }

  if (jack_set_client_registration_callback (jclient->client,
					     jclient_jack_client_registration_cb,
					     jclient))
    {
      error_print ("Cannot set JACK client registration callback");
    }

  if (jack_set_buffer_size_callback (jclient->client,
				     jclient_set_buffer_size_cb, jclient))
    {
      goto cleanup_jack;
    }

  if (jack_set_sample_rate_callback (jclient->client,
				     jclient_set_sample_rate_cb, jclient))
    {
      goto cleanup_jack;
    }

  if (jclient->priority < 0)
    {
      jclient->priority = jack_client_real_time_priority (jclient->client);
    }
  debug_print (1, "Using RT priority %d...", jclient->priority);

  debug_print (1, "Registering ports...");
  jclient->output_ports = malloc (sizeof (jack_port_t *) * desc->outputs);
  for (int i = 0; i < desc->outputs; i++)
    {
      const char *name = desc->output_track_names[i];
      debug_print (2, "Registering output port %s...", name);
      jclient->output_ports[i] = jack_port_register (jclient->client,
						     name,
						     JACK_DEFAULT_AUDIO_TYPE,
						     JackPortIsOutput |
						     JackPortIsTerminal, 0);

      if (jclient->output_ports[i] == NULL)
	{
	  error_print (MSG_ERROR_PORT_REGISTER);
	  goto cleanup_jack;
	}
    }

  jclient->input_ports = malloc (sizeof (jack_port_t *) * desc->inputs);
  for (int i = 0; i < desc->inputs; i++)
    {
      const char *name = desc->input_track_names[i];
      debug_print (2, "Registering input port %s...", name);
      jclient->input_ports[i] = jack_port_register (jclient->client,
						    name,
						    JACK_DEFAULT_AUDIO_TYPE,
						    JackPortIsInput |
						    JackPortIsTerminal, 0);

      if (jclient->input_ports[i] == NULL)
	{
	  error_print (MSG_ERROR_PORT_REGISTER);
	  goto cleanup_jack;
	}
    }

  jclient->midi_output_port = jack_port_register (jclient->client, "MIDI out",
						  JACK_DEFAULT_MIDI_TYPE,
						  JackPortIsOutput, 0);

  if (jclient->midi_output_port == NULL)
    {
      error_print (MSG_ERROR_PORT_REGISTER);
      goto cleanup_jack;
    }

  jclient->midi_input_port = jack_port_register (jclient->client, "MIDI in",
						 JACK_DEFAULT_MIDI_TYPE,
						 JackPortIsInput, 0);

  if (jclient->midi_input_port == NULL)
    {
      error_print (MSG_ERROR_PORT_REGISTER);
      goto cleanup_jack;
    }

  jclient->context.o2h_audio = jack_ringbuffer_create (MAX_LATENCY *
						       ow_resampler_get_o2h_frame_size
						       (jclient->resampler));
  jack_ringbuffer_mlock (jclient->context.o2h_audio);

  jclient->context.h2o_audio = jack_ringbuffer_create (MAX_LATENCY *
						       ow_resampler_get_h2o_frame_size
						       (jclient->resampler));
  jack_ringbuffer_mlock (jclient->context.h2o_audio);

  jclient->context.o2h_midi = jack_ringbuffer_create (OB_MIDI_BUF_LEN);
  jack_ringbuffer_mlock (jclient->context.o2h_midi);

  jclient->context.h2o_midi = jack_ringbuffer_create (OB_MIDI_BUF_LEN);
  jack_ringbuffer_mlock (jclient->context.h2o_midi);

  jclient->context.read_space =
    (ow_buffer_rw_space_t) jack_ringbuffer_read_space;
  jclient->context.write_space =
    (ow_buffer_rw_space_t) jack_ringbuffer_write_space;
  jclient->context.read = jclient_buffer_read;
  jclient->context.write = (ow_buffer_write_t) jack_ringbuffer_write;
  jclient->context.get_time = jack_get_time;

  jclient->context.set_rt_priority = set_rt_priority;
  jclient->context.priority = jclient->priority;

  jclient->context.options = OW_ENGINE_OPTION_O2P_AUDIO |
    OW_ENGINE_OPTION_O2P_MIDI | OW_ENGINE_OPTION_P2O_MIDI;

  squeue_init (&jclient->o2j_midi_queue, MAX_MIDI_BUF_LEN);
  squeue_init (&jclient->j2o_midi_queue, MAX_MIDI_BUF_LEN);

  jclient->o2j_midi_skipping = 0;
  jclient->o2j_last_lost_count = 0;

  err = ow_resampler_start (jclient->resampler, &jclient->context);
  if (err)
    {
      goto cleanup_jack;
    }

  if (jack_activate (jclient->client))
    {
      error_print ("Cannot activate client");
      err = -1;
      goto cleanup_jack;
    }

  ow_resampler_wait (jclient->resampler);

  debug_print (1, "Exiting...");
  jack_deactivate (jclient->client);

cleanup_jack:
  jack_ringbuffer_free (jclient->context.h2o_audio);
  jack_ringbuffer_free (jclient->context.o2h_audio);
  jack_ringbuffer_free (jclient->context.h2o_midi);
  jack_ringbuffer_free (jclient->context.o2h_midi);
  squeue_destroy (&jclient->o2j_midi_queue);
  squeue_destroy (&jclient->j2o_midi_queue);
  jack_client_close (jclient->client);
  free (jclient->output_ports);
  free (jclient->input_ports);
  return err;
}

static void *
jclient_thread_runner (void *data)
{
  struct jclient *jclient = data;
  jclient_run (jclient);
  return NULL;
}

int
jclient_start (struct jclient *jclient)
{
  int err = pthread_create (&jclient->thread, NULL, jclient_thread_runner,
			    jclient);
  jclient->running = err ? 0 : 1;
  return err;
}

void
jclient_wait (struct jclient *jclient)
{
  if (jclient->running)
    {
      pthread_join (jclient->thread, NULL);
      jclient->running = 0;
    }
}
