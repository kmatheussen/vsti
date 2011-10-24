/*
 *   VST instrument internal jack client
 *    
 *   Copyright (C) Kjetil S. Matheussen 2004 (k.s.matheussen@notam02.no)
 *   Alsa-seq midi-code made by looking at the jack-rack source made by Bob Ham.
 *    
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */



#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <memory.h>
#include <fcntl.h>
#include <stdbool.h>

#include <alsa/asoundlib.h>

#include <jack/jack.h>

#include <vstlib.h>

#define AEFFECTX_H_LINUXWORKAROUND
#include "vst/aeffectx.h"

struct Data{
  struct AEffect *effect;
  jack_client_t *client;
  int num_inputs;
  int num_outputs;
  jack_port_t **input_ports;
  jack_port_t **output_ports;
  pthread_t midithread;
  bool toquit;
  snd_seq_t *seq;
};


static snd_seq_t *create_alsa_seq(const char *client_name,bool isinput){
  snd_seq_t * seq;
  int err;
  
  err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
  if (err){
    printf( "Could not open ALSA sequencer, aborting\n\n%s\n\n"
	    "Make sure you have configure ALSA properly and that\n"
	    "/proc/asound/seq/clients exists and contains relevant\n"
	    "devices.\n", snd_strerror (err));
    return NULL;
  }
  
  snd_seq_set_client_name (seq, client_name);
  
  err = snd_seq_create_simple_port (seq, isinput==true?"Input":"Output",
                                    (isinput==true?SND_SEQ_PORT_CAP_WRITE:SND_SEQ_PORT_CAP_READ)|SND_SEQ_PORT_CAP_DUPLEX|
                                    SND_SEQ_PORT_CAP_SUBS_READ|SND_SEQ_PORT_CAP_SUBS_WRITE,
                                    SND_SEQ_PORT_TYPE_APPLICATION|SND_SEQ_PORT_TYPE_SPECIFIC);

  if (err){
    printf ( "Could not create ALSA port, aborting\n\n%s\n", snd_strerror (err));
    snd_seq_close(seq);
    return NULL;
  }

  return seq;
}


static void sendmidi(AEffect *effect,int val1, int val2, int val3){
  struct VstMidiEvent das_event;
  struct VstMidiEvent *pevent=&das_event;

  struct VstEvents events;

  //  printf("note: %d\n",note);

  pevent->type = kVstMidiType;
  pevent->byteSize = 24;
  pevent->deltaFrames = 0;
  pevent->flags = 0;
  pevent->detune = 0;
  pevent->noteLength = 0;
  pevent->noteOffset = 0;
  pevent->reserved1 = 0;
  pevent->reserved2 = 0;
  pevent->noteOffVelocity = 0;
  pevent->midiData[0] = val1;
  pevent->midiData[1] = val2;
  pevent->midiData[2] = val3;
  pevent->midiData[3] = 0;

  events.numEvents = 1;
  events.reserved  = 0;
  events.events[0]=(VstEvent*)pevent;

  //printf("Sending: %x %x %x\n",val1,val2,val3);
  
  effect->dispatcher(
  		     effect,
  		     effProcessEvents, 0, 0, &events, 0.0f
  		     );
}




#if 0
// To receive from OSS.
static void *midireceiver(void *arg){
  struct Data *data=(struct Data*)arg;
  int fd=open("/dev/midi1",O_RDONLY|O_NDELAY);
  char dasbyte;
  int byte;
  int ret;
  int val1=-1,val2=0;
  bool lastwasval2=false;

  if(fd==-1){
    close(fd);
    fprintf(stderr,"vstclient: Could not open midi\n");
    return NULL;
  }

  while(1){
    ret=read(fd,&dasbyte,1);
    if(data->toquit==true) break;
    byte=dasbyte&0xff;

    if(ret>0){
      if(byte>=0xf0){
	if(byte>0xf7){
	  sendmidi(byte,0,0);
	}else{
	  val1=-1;
	}
      }else{
	if( (byte&0xf0) >=0x80){
	  val1=byte;
	}else{
	  if(val1!=-1){
	    if( (val1&0xf0) ==0xc0 || (val1&0xc0) ==0xd0){
	      sendmidi(val1,byte,0);
	    }else{
	      if(lastwasval2==true){
		sendmidi(val1,val2,byte);
		lastwasval2=false;
	      }else{
		val2=byte;
		lastwasval2=true;
	      }
	    }
	  }
	}      
      }
    }
  }

  return NULL;
}
#endif


static void *midireceiver(void *arg){
  struct Data *data=(struct Data*)arg;
  AEffect *effect=data->effect;
  snd_seq_event_t *event;

  for(;;){
    snd_seq_event_input(data->seq, &event);
    if(data->toquit==true) break;
    switch(event->type){
    case SND_SEQ_EVENT_NOTEON:
      sendmidi(effect,0x90+event->data.note.channel,event->data.note.note,event->data.note.velocity);
      //printf("Noteon, channel: %d note: %d vol: %d\n",event->data.note.channel,event->data.note.note,event->data.note.velocity);
      break;
    case SND_SEQ_EVENT_NOTEOFF:
      sendmidi(effect,0x90+event->data.note.channel,event->data.note.note,0);
      //printf("Noteoff, channel: %d note: %d vol: %d\n",event->data.note.channel,event->data.note.note,event->data.note.velocity);
      break;
    case SND_SEQ_EVENT_KEYPRESS:
      //printf("Keypress, channel: %d note: %d vol: %d\n",event->data.note.channel,event->data.note.note,event->data.note.velocity);
      sendmidi(effect,0xa0+event->data.note.channel,event->data.note.note,event->data.note.velocity);
      break;
    case SND_SEQ_EVENT_CONTROLLER:
      sendmidi(effect,0xb0+event->data.control.channel,event->data.control.param,event->data.control.value);
      //printf("Control: %d %d %d\n",event->data.control.channel,event->data.control.param,event->data.control.value);
      break;
    case SND_SEQ_EVENT_PITCHBEND:
      {
	int val=event->data.control.value + 0x2000;
	sendmidi(effect,0xe0+event->data.control.channel,val&127,val>>7);
      }
      //printf("Pitch: %d %d %d\n",event->data.control.channel,event->data.control.param,event->data.control.value);
      break;
    case SND_SEQ_EVENT_CHANPRESS:
      //printf("chanpress: %d %d %d\n",event->data.control.channel,event->data.control.param,event->data.control.value);
      sendmidi(effect,0xc0+event->data.control.channel,event->data.control.value,0);
      break;
    case SND_SEQ_EVENT_PGMCHANGE:
      //printf("pgmchange: %d %d %d\n",event->data.control.channel,event->data.control.param,event->data.control.value);
      sendmidi(effect,0xc0+event->data.control.channel,event->data.control.value,0);
      break;
    default:
      //printf("Unknown type: %d\n",event->type);
      break;
    }
  }

  return NULL;
}


static void stop_midireceiver(struct Data *data){
  int err; 
  snd_seq_event_t event;

  snd_seq_t *seq2=create_alsa_seq("alsagakkquit",true);

  data->toquit=true;
  snd_seq_connect_to(seq2,0,snd_seq_client_id(data->seq),0);
  snd_seq_ev_clear      (&event);
  snd_seq_ev_set_direct (&event);
  snd_seq_ev_set_subs   (&event);
  snd_seq_ev_set_source (&event, 0);
  snd_seq_ev_set_controller (&event,1,0x80,50);

  err = snd_seq_event_output (seq2, &event);
  if (err < 0){
    fprintf (stderr, "jackvst: %s: error sending midi event: %s\n",
	     __FUNCTION__, snd_strerror (err));
  }
  snd_seq_drain_output (seq2);
  snd_seq_close(seq2);
  pthread_join(data->midithread,NULL);
  snd_seq_close(data->seq);
}


static int buffersizecallback(jack_nframes_t nframes, void *arg){
  struct Data *data=(struct Data*)arg;
  data->effect->dispatcher(data->effect,
			   effSetBlockSize,
			   0, nframes, NULL, 0);
  return 0;
}

static int process (jack_nframes_t nframes, void *arg){
  struct Data *data=(struct Data*)arg;
  float *ins[data->num_inputs];
  float *outs[data->num_outputs];
  int lokke;

  for(lokke=0;lokke<data->num_inputs;lokke++)
    ins[lokke]=(float*)jack_port_get_buffer(data->input_ports[lokke],nframes);
  for(lokke=0;lokke<data->num_outputs;lokke++)
    outs[lokke]=(float*)jack_port_get_buffer(data->output_ports[lokke],nframes);

  data->effect->processReplacing(data->effect,ins,outs,nframes);
  return 0;
}


int jack_initialize (jack_client_t *client, const char *string){
  struct Data *data;
  AEffect *effect;
  snd_seq_t *seq;

  int lokke;

  char pluginname[500];
  char *seqname;

  bool dontconnectjackports=false;

  sprintf(pluginname,"%s",string);
  seqname=&pluginname[0];

  if(strstr(string,":")){
    seqname=strstr(pluginname,":")+1;
    strstr(pluginname,":")[0]=0;
    if(strstr(seqname,":")){
      strstr(seqname,":")[0]=0;
      dontconnectjackports=true;
    }
  }
  
  printf("plugname: %s, seqname: %s, dontconnect: %s\n",pluginname,seqname,dontconnectjackports==true?"true":"false");
     
  //printf("rate: %f, blocksize: %d\n",(float)jack_get_sample_rate(client),jack_get_buffer_size(client));
  
  /*****************************
       VST Init
  *****************************/

  effect=VSTLIB_new(pluginname);
  if(effect==NULL) return 1;

  effect->dispatcher (effect,
		      effOpen,
		      0, 0, NULL, 0);

  effect->dispatcher(
		     effect,
		     effSetSampleRate,
		     0,0,NULL,(float)jack_get_sample_rate(client));

  effect->dispatcher(effect,
		     effSetBlockSize,
		     0, jack_get_buffer_size(client), NULL, 0);

  effect->dispatcher(effect,
		     effMainsChanged,
		     0, 1, NULL, 0);

  effect->dispatcher (effect,
		      effEditOpen,
		      0, 0, NULL, 0);
  

  /*****************************
       MIDI init
  *****************************/

  seq=create_alsa_seq(seqname,true);
  if(seq==NULL){
    VSTLIB_delete(effect);
    return 2;
  }

  data=calloc(1,sizeof(struct Data));
  data->seq=seq;
  data->client=client;
  data->effect=effect;
  data->toquit=false;

  pthread_create(&data->midithread,NULL,midireceiver,data);
  {
    struct sched_param rt_param={0};
    int err;
    rt_param.sched_priority = 1; // This is not an important thread.
    err=pthread_setschedparam(data->midithread,SCHED_FIFO, &rt_param);
    if(err){
      fprintf(stderr,"Could not set realtime priority for midi thread.\n");
    }
  }


  /*****************************
       Jack init
  *****************************/

  jack_set_process_callback(client,process,data);
  jack_set_buffer_size_callback(client,buffersizecallback,data);

  data->num_inputs=data->effect->numInputs;
  data->num_outputs=data->effect->numOutputs;

  data->input_ports=calloc(sizeof(jack_port_t*),data->num_inputs);
  data->output_ports=calloc(sizeof(jack_port_t*),data->num_outputs);

  for(lokke=0;lokke<data->num_inputs;lokke++){
    char temp[500];
    sprintf(temp,"in%d",lokke);
    data->input_ports[lokke]=jack_port_register(client,temp, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
  }
  for(lokke=0;lokke<data->num_outputs;lokke++){
    char temp[500];
    sprintf(temp,"out%d",lokke);
    data->output_ports[lokke]=jack_port_register(client,temp, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  }

  
  jack_activate (client);


  if(dontconnectjackports==false){
    for(lokke=0;lokke<data->num_inputs;lokke++){
      char temp[500];
      sprintf(temp,"alsa_pcm:capture_%d",lokke+1);
      jack_connect(client,temp,jack_port_name(data->input_ports[lokke]));
    }
    for(lokke=0;lokke<data->num_outputs;lokke++){
      char temp[500];
      sprintf(temp,"alsa_pcm:playback_%d",lokke+1);
      jack_connect(client,jack_port_name(data->output_ports[lokke]),temp);
    }
  }


  return 0;
}


void jack_finish (void *arg){
  struct Data *data=(struct Data*)arg;
  if(data!=NULL){
    stop_midireceiver(data);
    data->effect->dispatcher(data->effect,
			     effMainsChanged,
			     0, 0, NULL, 0);
    VSTLIB_delete(data->effect);
    free(data->input_ports);
    free(data->output_ports);
    free(data);
  }
}

