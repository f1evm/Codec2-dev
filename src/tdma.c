/*---------------------------------------------------------------------------*\

  FILE........: tdma.c
  AUTHOR......: Brady O'Brien
  DATE CREATED: 18 September 2016

  Skeletion of the TDMA FSK modem

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2017 Brady O'Brien

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/


#include "fsk.h"
#include "freedv_vhf_framing.h"
#include "tdma.h"
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>


static const uint8_t TDMA_UW_V[] =    {0,1,1,0,0,1,1,1,
                                       1,0,1,0,1,1,0,1};

struct TDMA_MODEM * tdma_create(struct TDMA_MODE_SETTINGS mode){
    struct TDMA_MODEM * tdma;
    
    u32 Rs = mode.sym_rate;
    u32 Fs = mode.samp_rate;
    u32 slot_size = mode.slot_size;
    //u32 frame_size = mode.frame_size;
    u32 n_slots = mode.n_slots;
    u32 M = mode.fsk_m;
    u32 P = Fs/Rs;
    u32 Ts = Fs/Rs;
    COMP * samp_buffer = NULL;
    
    size_t i;

    assert( (Fs%Rs)==0 );
    assert( M==2 || M==4);

    /* allocate the modem */
    tdma = (struct TDMA_MODEM *) malloc(sizeof(struct TDMA_MODEM));
    if(tdma == NULL) goto cleanup_bad_alloc;

    /* Symbols over which pilot modem operates */
    u32 pilot_nsyms = slot_size/2;

    /* Set up pilot modem */
    struct FSK * pilot = fsk_create_hbr(Fs,Rs,P,M,Rs,Rs);
    if(pilot == NULL) goto cleanup_bad_alloc;
    fsk_enable_burst_mode(pilot,pilot_nsyms);
    tdma->fsk_pilot = pilot;
    
    tdma->settings = mode;
    tdma->state = no_sync;
    tdma->sample_sync_offset = 0;
    tdma->slot_cur = 0;

    /* Allocate buffer for incoming samples */
    /* TODO: We may only need a single slot's worth of samps -- look into this */
    samp_buffer = (COMP *) malloc(sizeof(COMP)*slot_size*Ts*n_slots);
    if(samp_buffer == NULL) goto cleanup_bad_alloc;

    tdma->sample_buffer = samp_buffer;
    for(i=0; i<slot_size*Ts*n_slots; i++){
        tdma->sample_buffer[i].real = 0;
        tdma->sample_buffer[i].imag = 0;
    }

    struct TDMA_SLOT * slot;
    struct TDMA_SLOT * last_slot;
    struct FSK * slot_fsk;
    last_slot = NULL;
    for(i=0; i<n_slots; i++){
        slot = (struct TDMA_SLOT *) malloc(sizeof(struct TDMA_SLOT));
        if(slot == NULL) goto cleanup_bad_alloc;
        slot->fsk = NULL;
        tdma->slots = slot;
        slot->next_slot = last_slot;
        slot->slot_local_frame_offset = 0;
        slot->state = rx_no_sync;
        slot_fsk = fsk_create_hbr(Fs,Rs,P,M,Rs,Rs);
        
        if(slot_fsk == NULL) goto cleanup_bad_alloc;

        fsk_enable_burst_mode(slot_fsk, slot_size);
        
        slot->fsk = slot_fsk;
        last_slot = slot;
    }

    return tdma;

    /* Clean up after a failed malloc */
    /* TODO: Run with valgrind/asan, make sure I'm getting everything */
    cleanup_bad_alloc:
    fprintf(stderr,"Cleaning up\n");
    if(tdma == NULL) return NULL;

    struct TDMA_SLOT * cleanup_slot = tdma->slots;
    struct TDMA_SLOT * cleanup_slot_next;
    while(cleanup_slot != NULL){
        cleanup_slot_next = cleanup_slot->next_slot;
        if(cleanup_slot->fsk != NULL) fsk_destroy(cleanup_slot->fsk);
        if(cleanup_slot != NULL) free(cleanup_slot);
        cleanup_slot = cleanup_slot_next;
    }
    if(pilot != NULL) fsk_destroy(pilot);
    if(samp_buffer != NULL) free(samp_buffer);
    free(tdma);
    return NULL;
}

void tdma_print_stuff(struct TDMA_MODEM * tdma){
    printf("symrate: %d\n",tdma->settings.sym_rate);
    printf("fsk_m: %d\n",tdma->settings.fsk_m);
    printf("samprate: %d\n",tdma->settings.samp_rate);
    printf("slotsize: %d\n",tdma->settings.slot_size);
    printf("framesize: %d\n",tdma->settings.frame_size);
    printf("nslots: %d\n",tdma->settings.n_slots);
    printf("frametype: %d\n",tdma->settings.frame_type);
    printf("sync_offset: %ld\n",tdma->sample_sync_offset);
}

void tdma_destroy(tdma_t * tdma){
    /* TODO: Free slot modems (need to create them first) */
    fsk_destroy(tdma->fsk_pilot);
    free(tdma->sample_buffer);
    free(tdma);
}

u32 tdma_get_N(tdma_t * tdma){
    u32 slot_size = tdma->settings.slot_size;
    u32 Fs = tdma->settings.samp_rate;
    u32 Rs = tdma->settings.sym_rate;
    return slot_size * (Fs/Rs);
}

static slot_t * tdma_get_slot(tdma_t * tdma, u32 slot_idx){
    /* Don't try and index beyond the end */
    if(slot_idx >= tdma->settings.n_slots) return NULL;

    size_t i;
    slot_t * cur = tdma->slots;
    for(i = 0; i < slot_idx; i++){
        /* Don't break */
        if(cur == NULL) return NULL;
        cur = cur->next_slot;
    }
    return cur;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void tdma_demod_end_slot(tdma_t * tdma,u32 slot_idx){

    struct TDMA_MODE_SETTINGS mode = tdma->settings;
    u32 Rs = mode.sym_rate;
    u32 Fs = mode.samp_rate;
    u32 slot_size = mode.slot_size;
    u32 frame_size = mode.frame_size;
    u32 n_slots = mode.n_slots;
    u32 M = mode.fsk_m;
    u32 Ts = Fs/Rs;
    u32 bits_per_sym = M==2?1:2;
    u32 slot_samps = slot_size*Ts;
    size_t nbits = slot_size*bits_per_sym;
    u32 frame_bits = frame_size*bits_per_sym;

    u8 bit_buf[nbits];
    /* Samples that belong to this frame */
    COMP frame_samps[slot_size*Ts];
    COMP * sample_buffer = tdma->sample_buffer;
    slot_t * slot = tdma_get_slot(tdma,slot_idx);
    //slot_t * slot = tdma->slots;
    struct FSK * fsk = slot->fsk;

    int nin = fsk_nin(fsk);

    /* Pull out the frame and demod */
    size_t move_samps = slot_samps*sizeof(COMP);
    uintptr_t move_from = ((uintptr_t)sample_buffer) + (tdma->sample_sync_offset)*sizeof(COMP);
    uintptr_t move_to = (uintptr_t)frame_samps; /* Don't really need this, but it's cleaner than doing it all in memmove */
    memcpy((void*)move_to,(void*)move_from,move_samps);
    /* Demodulate the frame */
    fsk_demod(fsk,bit_buf,frame_samps);

    size_t delta,off;
    off = fvhff_search_uw(bit_buf,nbits,TDMA_UW_V,16,&delta);
    u32 f_start = off- (frame_bits-16)/2;
    /* Calculate offset (in samps) from start of frame */
    /* Note: FSK outputs one bit from the last batch */
    u32 frame_offset = (f_start-bits_per_sym)*Ts;
    
    fprintf(stderr,"slot: %d offset: %d delta: %d f1:%.3f\n",slot_idx,off,delta,fsk->f_est[0]);
    for(int i=0; i<nbits; i++){
        if((i>off && i<=off+16) || i==f_start || i==(f_start+frame_bits)){
            if(bit_buf[i])  fprintf(stderr,"1̲");
            else            fprintf(stderr,"0̲");
        } else fprintf(stderr,"%d",bit_buf[i]);
    }
    fprintf(stderr,"\n");
}

/* We got a new slot's worth of samples. Run the slot modem and try to get slot sync */
/* This will probably also work for the slot_sync state */
void tdma_rx_pilot_sync(tdma_t * tdma){
    struct TDMA_MODE_SETTINGS mode = tdma->settings;
    u32 Rs = mode.sym_rate;
    u32 Fs = mode.samp_rate;
    u32 slot_size = mode.slot_size;
    //u32 frame_size = mode.frame_size;
    u32 n_slots = mode.n_slots;
    u32 M = mode.fsk_m;
    u32 Ts = Fs/Rs;
    u32 bits_per_sym = M==2?1:2;

    tdma_demod_end_slot(tdma,tdma->slot_cur);
    tdma->slot_cur++;
    if(tdma->slot_cur >= n_slots)
        tdma->slot_cur = 0;
}

void tdma_rx_no_sync(tdma_t * tdma, COMP * samps, u64 timestamp){
    struct TDMA_MODE_SETTINGS mode = tdma->settings;
    u32 Rs = mode.sym_rate;
    u32 Fs = mode.samp_rate;
    u32 slot_size = mode.slot_size;
    //u32 frame_size = mode.frame_size;
    u32 n_slots = mode.n_slots;
    u32 M = mode.fsk_m;
    u32 Ts = Fs/Rs;
    u32 bits_per_sym = M==2?1:2;

    //Number of bits per pilot modem chunk (half a slot)
    u32 n_pilot_bits = (slot_size/2)*bits_per_sym;
    //We look at a full slot for the UW
    u8 pilot_bits[n_pilot_bits];


    /*
    Pseudocode:
        copy into samp buffer 
            (? may want to let the downstream stuff do this; We may not even really need a local samp buffer)
            (could/probably should do this in tdma_rx)
        demod a half slot
        look for UW in slot-wide bit buffer
        if UW found
          change to pilot sync state
          set slot offset to match where a frame should be
          go to slot_sync_rx to try and decode found frame
        else  
          set up for next half slot, repeat 
            next half slot overlaps current slot      
   */
}

void tdma_rx(tdma_t * tdma, COMP * samps,u64 timestamp){

    COMP * sample_buffer = tdma->sample_buffer;
    struct TDMA_MODE_SETTINGS mode = tdma->settings;
    u32 Rs = mode.sym_rate;
    u32 Fs = mode.samp_rate;
    u32 slot_size = mode.slot_size;
    //u32 frame_size = mode.frame_size;
    u32 n_slots = mode.n_slots;
    u32 M = mode.fsk_m;
    u32 Ts = Fs/Rs;
    u32 bits_per_sym = M==2?1:2;
    u32 slot_samps = slot_size*Ts;

    /* Copy samples into the local buffer for some reason */
    /* Move the current samps in the buffer back by a slot or so */
    size_t move_samps = slot_samps*sizeof(COMP);
    uintptr_t move_from = ((uintptr_t)sample_buffer) + (n_slots-1)*slot_samps*sizeof(COMP);
    uintptr_t move_to = (uintptr_t)sample_buffer; /* Don't really need this, but it's cleaner than doing it all in memmove */
    memmove((void*)move_to,(void*)move_from,move_samps);

    move_samps = slot_samps*sizeof(COMP);
    move_from = (uintptr_t)samps;
    move_to = ((uintptr_t)sample_buffer) + (n_slots-1)*slot_samps*sizeof(COMP);
    memcpy((void*)move_to,(void*)move_from,move_samps);

    /* Set the timestamp. Not sure if this makes sense */
    tdma->timestamp = timestamp - (slot_samps*(n_slots-1));

    /* Staate machine for TDMA modem */
    switch(tdma->state){
        case no_sync:
            tdma_rx_no_sync(tdma,samps,timestamp);
            break;
        case pilot_sync:
            tdma_rx_pilot_sync(tdma);
            break;
        case slot_sync:
            break;
        case master_sync:
            break;
        default:
            break;
    }
    tdma->state = pilot_sync;
}


#pragma GCC diagnostic pop
