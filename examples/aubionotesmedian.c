/*
   Copyright (C) 2003 Paul Brossier

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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include "aubio.h"
#include "utils.h"

#include <math.h> //required for FLOORF :(

/* settings */
const char * output_filename = NULL;
const char * input_filename  = NULL;
const char * onset_filename  = "/usr/share/sounds/aubio/woodblock.aiff";
int verbose = 0;
int usejack = 0;
int usedoubled = 1;
int usemidi = 1;

/* energy,specdiff,hfc,complexdomain,phase */
aubio_onsetdetection_type type_onset  = hfc;
aubio_onsetdetection_type type_onset2 = complexdomain;
smpl_t threshold                      = 0.3;
smpl_t threshold2                     = -90.;
uint_t buffer_size                    = 1024;
uint_t overlap_size                   = 512;
uint_t channels                       = 1;
uint_t samplerate                     = 44100;

/* global objects */
int frames;
aubio_pvoc_t * pv;
fvec_t * ibuf;
fvec_t * obuf;
cvec_t * fftgrain;
fvec_t * woodblock;
aubio_onsetdetection_t *o;
aubio_onsetdetection_t *o2;
fvec_t *onset;
fvec_t *onset2;
int isonset = 0;
aubio_pickpeak_t * parms;

/* pitch objects */
smpl_t pitch               = 0.;
cvec_t * fftpitch          = NULL;
aubio_filter_t * filter    = NULL;
aubio_pvoc_t * pvi         = NULL;
aubio_pitchmcomb_t * p = NULL;

fvec_t * note_buffer = NULL;
fvec_t * note_buffer2 = NULL;
uint_t medianfiltlen = 6;
smpl_t curlevel = 0;
smpl_t maxonset = 0.;

/* midi objects */
aubio_midi_player_t * mplay; 
aubio_midi_driver_t * mdriver; 
aubio_midi_event_t  * event;

void send_noteon(aubio_midi_driver_t * d, int pitch, int velo);
void send_noteon(aubio_midi_driver_t * d, int pitch, int velo)
{
    /* we should check if we use midi here, not jack */
#if ALSA_SUPPORT
    if (usejack) {
        if (velo==0) {
            aubio_midi_event_set_type(event,NOTE_OFF);
        } else {
            aubio_midi_event_set_type(event,NOTE_ON);
        }
        aubio_midi_event_set_channel(event,0);
        aubio_midi_event_set_pitch(event,pitch);
        aubio_midi_event_set_velocity(event,velo);
        aubio_midi_direct_output(mdriver,event);
    } else 
#endif
    {
        if (velo==0) {
            outmsg("%f\n",frames*overlap_size/(float)samplerate);
        } else {
            outmsg("%d\t%f\t",pitch,frames*overlap_size/(float)samplerate);
        }
    }
}


/** append new note candidate to the note_buffer and return filtered value. we
 * need to copy the input array as vec_median destroy its input data.*/

void note_append(fvec_t * note_buffer, smpl_t curnote); 
void note_append(fvec_t * note_buffer, smpl_t curnote) {
  uint_t i = 0;
  for (i = 0; i < note_buffer->length - 1; i++) { 
      note_buffer->data[0][i] = note_buffer->data[0][i+1];
  }
  note_buffer->data[0][note_buffer->length - 1] = curnote;
  return;
}

uint_t get_note(fvec_t *note_buffer, fvec_t *note_buffer2);
uint_t get_note(fvec_t *note_buffer, fvec_t *note_buffer2){
  uint_t i = 0;
  for (i = 0; i < note_buffer->length; i++) { 
      note_buffer2->data[0][i] = note_buffer->data[0][i];
  }
  return vec_median(note_buffer2);
}


smpl_t curnote = 0.;
smpl_t newnote = 0.;
uint_t isready = 0;

int aubio_process(float **input, float **output, int nframes);
int aubio_process(float **input, float **output, int nframes) {
  unsigned int i;       /*channels*/
  unsigned int j;       /*frames*/
  unsigned int pos = 0; /*frames%dspblocksize*/
  for (j=0;j<nframes;j++) {
    if(usejack) {
      for (i=0;i<channels;i++) {
        /* write input to datanew */
        fvec_write_sample(ibuf, input[i][j], i, pos);
        /* put synthnew in output */
        output[i][j] = fvec_read_sample(obuf, i, pos);
      }
    }
    /*time for fft*/
    if (pos == overlap_size-1) {         
      /* block loop */
      aubio_pvoc_do (pv,ibuf, fftgrain);
      aubio_onsetdetection(o,fftgrain, onset);
      if (usedoubled) {
        aubio_onsetdetection(o2,fftgrain, onset2);
        onset->data[0][0] *= onset2->data[0][0];
      }
      isonset = aubio_peakpick_pimrt(onset,parms);
      
      aubio_filter_do(filter,ibuf);
      aubio_filter_do(filter,ibuf);
      aubio_pvoc_do(pvi,ibuf, fftpitch);
      pitch = aubio_pitchmcomb_detect(p,fftpitch);
      newnote = (FLOOR)(bintomidi(pitch,samplerate,buffer_size*4)+.5);
      note_append(note_buffer, newnote);

      //outmsg("%f    %f\n", pitch, newnote);

      /* curlevel is negatif or 1 if silence */
      curlevel = aubio_level_detection(ibuf, threshold2);
      if (isonset) {
        /* test for silence */
        if (curlevel == 1.) {
          isonset=0;
          isready=0;
          /* send note off */
          send_noteon(mdriver,curnote,0);
        } else {
          isready = 1;
          for (pos = 0; pos < overlap_size; pos++){
            obuf->data[0][pos] = woodblock->data[0][pos];
          }
        }
      } else {
//        if (curlevel != 1) {
//          if (bufpos == note_buffer->length)
//            curnote = aubio_getnote(note_buffer);
//        }
//
        if (isready > 0)
            isready++;
        if (isready == note_buffer->length)
        {
            //outmsg("%d, %f\n", isready, curnote);
            send_noteon(mdriver,curnote,0);
        /* kill old note */
            newnote = get_note(note_buffer, note_buffer2);
            curnote = newnote;
        /* get and send new one */
        /*if (curnote<45){
          send_noteon(mdriver,curnote,0);
        } else {*/
            if (curnote>45){
                send_noteon(mdriver,curnote,127+(int)FLOOR(curlevel));
            }
        }
        for (pos = 0; pos < overlap_size; pos++)
          obuf->data[0][pos] = 0.;
      }
      /* end of block loop */
      //aubio_pvoc_rdo(pv,fftgrain, obuf);
      pos = -1; /* so it will be zero next j loop */
    }
    pos++;
  }
  return 1;
}

int main(int argc, char **argv) {

  aubio_file_t * file = NULL;
  aubio_file_t * fileout = NULL;

  aubio_file_t * onsetfile = new_file_ro(onset_filename);
  /* parse command line arguments */
  parse_args(argc, argv);

  if(!usejack)
  {
    debug("Opening files ...\n");
    file = new_file_ro (input_filename);
    file_info(file);
    channels = aubio_file_channels(file);
    if (output_filename != NULL)
      fileout = new_file_wo(file, output_filename);
  }

  ibuf        = new_fvec(overlap_size, channels);
  obuf        = new_fvec(overlap_size, channels);
  woodblock   = new_fvec(buffer_size,1);
  fftgrain    = new_cvec(buffer_size, channels);

  pvi      = new_aubio_pvoc(buffer_size*4, overlap_size, channels);
  fftpitch = new_cvec(buffer_size*4, channels);
  filter   = new_aubio_adsgn_filter((smpl_t)samplerate);
  p        = new_aubio_pitchmcomb(buffer_size*4,channels);

  note_buffer = new_fvec(medianfiltlen, 1);
  note_buffer2= new_fvec(medianfiltlen, 1);
  /* read the output sound once */
  file_read(onsetfile, overlap_size, woodblock);

  /* phase vocoder */
  debug("Phase voc init ... \n");
  pv = new_aubio_pvoc(buffer_size, overlap_size, channels);

  /* onsets */
  parms = new_aubio_peakpicker(threshold);
  o = new_aubio_onsetdetection(type_onset,buffer_size,channels);
  onset = new_fvec(1, channels);
  if (usedoubled)    {
    o2 = new_aubio_onsetdetection(type_onset2,buffer_size,channels);
    onset2 = new_fvec(1 , channels);
  }

  if(usejack) {
#ifdef JACK_SUPPORT
    aubio_jack_t * jack_setup;
    debug("Midi init ...\n");
    debug("Jack init ...\n");
    jack_setup = new_aubio_jack(channels, channels,
          (aubio_process_func_t)aubio_process);

    mplay = new_aubio_midi_player();

    mdriver = new_aubio_midi_driver("alsa_seq",
        (handle_midi_event_func_t)aubio_midi_send_event, mplay);

    event = new_aubio_midi_event();
    
    debug("Jack activation ...\n");
    aubio_jack_activate(jack_setup);
    debug("Processing (Ctrl+C to quit) ...\n");
    pause();
    send_noteon(mdriver,curnote,0);
    aubio_jack_close(jack_setup);
    del_aubio_midi_driver(mdriver);
#else
    outmsg("Compiled without jack output, exiting.");
#endif

  } else {
    /* phasevoc */
    debug("Processing ...\n");

    frames = 0;

    while (overlap_size == file_read(file, overlap_size, ibuf))
    {
      isonset=0;
      aubio_process(ibuf->data, obuf->data, overlap_size);
      if (output_filename != NULL) {
        file_write(fileout,overlap_size,obuf);
      }
      frames++;
    }
    send_noteon(mdriver,curnote,0);

    debug("Processed %d frames of %d samples.\n", frames, buffer_size);
    del_file(file);

    if (output_filename != NULL)
      del_file(fileout);

  }

  del_aubio_pvoc(pv);
  del_fvec(obuf);
  del_fvec(ibuf);
  del_cvec(fftgrain);
  del_cvec(fftpitch);
  del_aubio_pvoc(pvi);
  del_fvec(onset);
  del_fvec(note_buffer);
  del_fvec(note_buffer2);

  debug("End of program.\n");
  fflush(stderr);
  return 0;
}
