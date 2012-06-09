/*---------------------------------------------------------------------------*\

  FILE........: codec2.c
  AUTHOR......: David Rowe
  DATE CREATED: 21/8/2010

  Codec2 fully quantised encoder and decoder functions.  If you want use 
  codec2, the codec2_xxx functions are for you.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2010 David Rowe

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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "defines.h"
#include "sine.h"
#include "nlp.h"
#include "dump.h"
#include "lpc.h"
#include "quantise.h"
#include "phase.h"
#include "interp.h"
#include "postfilter.h"
#include "codec2.h"
#include "lsp.h"
#include "codec2_internal.h"

/*---------------------------------------------------------------------------*\
                                                       
                             FUNCTION HEADERS

\*---------------------------------------------------------------------------*/

void analyse_one_frame(struct CODEC2 *c2, MODEL *model, short speech[]);
void synthesise_one_frame(struct CODEC2 *c2, short speech[], MODEL *model,
			  float ak[]);
void codec2_encode_2400(struct CODEC2 *c2, unsigned char * bits, short speech[]);
void codec2_decode_2400(struct CODEC2 *c2, short speech[], const unsigned char * bits);
void codec2_encode_1400(struct CODEC2 *c2, unsigned char * bits, short speech[]);
void codec2_decode_1400(struct CODEC2 *c2, short speech[], const unsigned char * bits);
void codec2_encode_1200(struct CODEC2 *c2, unsigned char * bits, short speech[]);
void codec2_decode_1200(struct CODEC2 *c2, short speech[], const unsigned char * bits);

/*---------------------------------------------------------------------------*\
                                                       
                                FUNCTIONS

\*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_create	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 21/8/2010 

  Create and initialise an instance of the codec.  Returns a pointer
  to the codec states or NULL on failure.  One set of states is
  sufficient for a full duuplex codec (i.e. an encoder and decoder).
  You don't need separate states for encoders and decoders.  See
  c2enc.c and c2dec.c for examples.

\*---------------------------------------------------------------------------*/

struct CODEC2 * CODEC2_WIN32SUPPORT codec2_create(int mode)
{
    struct CODEC2 *c2;
    int            i,l;

    c2 = (struct CODEC2*)malloc(sizeof(struct CODEC2));
    if (c2 == NULL)
	return NULL;
    
    assert(
	   (mode == CODEC2_MODE_2400) || 
	   (mode == CODEC2_MODE_1400) || 
	   (mode == CODEC2_MODE_1200)
	   );
    c2->mode = mode;
    for(i=0; i<M; i++)
	c2->Sn[i] = 1.0;
    c2->hpf_states[0] = c2->hpf_states[1] = 0.0;
    for(i=0; i<2*N; i++)
	c2->Sn_[i] = 0;
    make_analysis_window(c2->w,c2->W);
    make_synthesis_window(c2->Pn);
    quantise_init();
    c2->prev_Wo_enc = 0.0;
    c2->bg_est = 0.0;
    c2->ex_phase = 0.0;

    for(l=1; l<MAX_AMP; l++)
	c2->prev_model_dec.A[l] = 0.0;
    c2->prev_model_dec.Wo = TWO_PI/P_MAX;
    c2->prev_model_dec.L = PI/c2->prev_model_dec.Wo;
    c2->prev_model_dec.voiced = 0;

    for(i=0; i<LPC_ORD; i++) {
      c2->prev_lsps_dec[i] = i*PI/(LPC_ORD+1);
    }
    c2->prev_e_dec = 1;

    c2->nlp = nlp_create();
    if (c2->nlp == NULL) {
	free (c2);
	return NULL;
    }

    c2->xq_enc[0] = c2->xq_enc[1] = 0.0;
    c2->xq_dec[0] = c2->xq_dec[1] = 0.0;

    return c2;
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_destroy	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 21/8/2010 

  Destroy an instance of the codec.

\*---------------------------------------------------------------------------*/

void CODEC2_WIN32SUPPORT codec2_destroy(struct CODEC2 *c2)
{
    assert(c2 != NULL);
    nlp_destroy(c2->nlp);
    free(c2);
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_bits_per_frame     
  AUTHOR......: David Rowe			      
  DATE CREATED: Nov 14 2011

  Returns the number of bits per frame.

\*---------------------------------------------------------------------------*/

int CODEC2_WIN32SUPPORT codec2_bits_per_frame(struct CODEC2 *c2) {
    if (c2->mode == CODEC2_MODE_2400)
	return 48;
    if  (c2->mode == CODEC2_MODE_1400)
	return 56;
    if  (c2->mode == CODEC2_MODE_1200)
	return 48;

    return 0; /* shouldn't get here */
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_samples_per_frame     
  AUTHOR......: David Rowe			      
  DATE CREATED: Nov 14 2011

  Returns the number of bits per frame.

\*---------------------------------------------------------------------------*/

int CODEC2_WIN32SUPPORT codec2_samples_per_frame(struct CODEC2 *c2) {
    if (c2->mode == CODEC2_MODE_2400)
	return 160;
    if  (c2->mode == CODEC2_MODE_1400)
	return 320;
    if  (c2->mode == CODEC2_MODE_1200)
	return 320;

    return 0; /* shouldnt get here */
}

void CODEC2_WIN32SUPPORT codec2_encode(struct CODEC2 *c2, unsigned char *bits, short speech[])
{
    assert(c2 != NULL);
    assert(
	   (c2->mode == CODEC2_MODE_2400) || 
	   (c2->mode == CODEC2_MODE_1400) || 
	   (c2->mode == CODEC2_MODE_1200)
	   );

    if (c2->mode == CODEC2_MODE_2400)
	codec2_encode_2400(c2, bits, speech);
    if (c2->mode == CODEC2_MODE_1400)
	codec2_encode_1400(c2, bits, speech);
    if (c2->mode == CODEC2_MODE_1200)
	codec2_encode_1200(c2, bits, speech);
}

void CODEC2_WIN32SUPPORT codec2_decode(struct CODEC2 *c2, short speech[], const unsigned char *bits)
{
    assert(c2 != NULL);
    assert(
	   (c2->mode == CODEC2_MODE_2400) || 
	   (c2->mode == CODEC2_MODE_1400) || 
	   (c2->mode == CODEC2_MODE_1200)
	   );

    if (c2->mode == CODEC2_MODE_2400)
	codec2_decode_2400(c2, speech, bits);
    if (c2->mode == CODEC2_MODE_1400)
 	codec2_decode_1400(c2, speech, bits);
    if (c2->mode == CODEC2_MODE_1200)
 	codec2_decode_1200(c2, speech, bits);
}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_encode_2400	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 21/8/2010 

  Encodes 160 speech samples (20ms of speech) into 48 bits.  

  The codec2 algorithm actually operates internally on 10ms (80
  sample) frames, so we run the encoding algorithm twice.  On the
  first frame we just send the voicing bit.  One the second frame we
  send all model parameters.

  The bit allocation is:

    Parameter                      bits/frame
    --------------------------------------
    Harmonic magnitudes (LSPs)     36
    Joint VQ of Energy and Wo       8
    Voicing (10ms update)           2
    Spare                           2
    TOTAL                          48
 
\*---------------------------------------------------------------------------*/

void codec2_encode_2400(struct CODEC2 *c2, unsigned char * bits, short speech[])
{
    MODEL   model;
    float   ak[LPC_ORD+1];
    float   lsps[LPC_ORD];
    float   e;
    int     WoE_index;
    int     lsp_indexes[LPC_ORD];
    int     i;
    int     spare = 0;
    unsigned int nbit = 0;

    assert(c2 != NULL);

    memset(bits, '\0', ((codec2_bits_per_frame(c2) + 7) / 8));

    /* first 10ms analysis frame - we just want voicing */

    analyse_one_frame(c2, &model, speech);
    pack(bits, &nbit, model.voiced, 1);

    /* second 10ms analysis frame */

    analyse_one_frame(c2, &model, &speech[N]);
    pack(bits, &nbit, model.voiced, 1);
    
    e = speech_to_uq_lsps(lsps, ak, c2->Sn, c2->w, LPC_ORD);
    WoE_index = encode_WoE(&model, e, c2->xq_enc);
    pack(bits, &nbit, WoE_index, WO_E_BITS);

    encode_lsps_scalar(lsp_indexes, lsps, LPC_ORD);
    for(i=0; i<LSP_SCALAR_INDEXES; i++) {
	pack(bits, &nbit, lsp_indexes[i], lsp_bits(i));
    }
    pack(bits, &nbit, spare, 2);

    assert(nbit == (unsigned)codec2_bits_per_frame(c2));
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_decode_2400	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 21/8/2010 

  Decodes frames of 48 bits into 160 samples (20ms) of speech.

\*---------------------------------------------------------------------------*/

void codec2_decode_2400(struct CODEC2 *c2, short speech[], const unsigned char * bits)
{
    MODEL   model[2];
    int     lsp_indexes[LPC_ORD];
    float   lsps[2][LPC_ORD];
    int     WoE_index;
    float   e[2];
    float   snr;
    float   ak[2][LPC_ORD+1];
    int     i,j;
    unsigned int nbit = 0;

    assert(c2 != NULL);
    
    /* only need to zero these out due to (unused) snr calculation */

    for(i=0; i<2; i++)
	for(j=1; j<=MAX_AMP; j++)
	    model[i].A[j] = 0.0;

    /* unpack bits from channel ------------------------------------*/

    /* this will partially fill the model params for the 2 x 10ms
       frames */

    model[0].voiced = unpack(bits, &nbit, 1);

    model[1].voiced = unpack(bits, &nbit, 1);
    WoE_index = unpack(bits, &nbit, WO_E_BITS);
    decode_WoE(&model[1], &e[1], c2->xq_dec, WoE_index);

    for(i=0; i<LSP_SCALAR_INDEXES; i++) {
	lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
    }
    decode_lsps_scalar(&lsps[1][0], lsp_indexes, LPC_ORD);
    check_lsp_order(&lsps[1][0], LPC_ORD);
    bw_expand_lsps(&lsps[1][0], LPC_ORD);
 
    /* interpolate ------------------------------------------------*/

    /* Wo and energy are sampled every 20ms, so we interpolate just 1
       10ms frame between 20ms samples */

    interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
    e[0] = interp_energy(c2->prev_e_dec, e[1]);
 
    /* LSPs are sampled every 40ms so we interpolate the frame in
       between, then recover spectral amplitudes */

    interpolate_lsp_ver2(&lsps[0][0], c2->prev_lsps_dec, &lsps[1][0], 0.5);
    for(i=0; i<2; i++) {
	lsp_to_lpc(&lsps[i][0], &ak[i][0], LPC_ORD);
	aks_to_M2(&ak[i][0], LPC_ORD, &model[i], e[i], &snr, 1); 
	apply_lpc_correction(&model[i]);
    }

    /* synthesise ------------------------------------------------*/

    for(i=0; i<2; i++)
	synthesise_one_frame(c2, &speech[N*i], &model[i], &ak[i][0]);

    /* update memories for next frame ----------------------------*/

    c2->prev_model_dec = model[1];
    c2->prev_e_dec = e[1];
    for(i=0; i<LPC_ORD; i++)
	c2->prev_lsps_dec[i] = lsps[1][i];
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_encode_1400	     
  AUTHOR......: David Rowe			      
  DATE CREATED: May 11 2012

  Encodes 320 speech samples (40ms of speech) into 56 bits.

  The codec2 algorithm actually operates internally on 10ms (80
  sample) frames, so we run the encoding algorithm for times:

  frame 0: voicing bit
  frame 1: voicing bit, joint VQ of Wo and E
  frame 2: voicing bit
  frame 3: voicing bit, joint VQ of Wo and E, scalar LSPs

  The bit allocation is:

    Parameter                      frame 2  frame 4   Total
    -------------------------------------------------------
    Harmonic magnitudes (LSPs)      0       36        36
    Energy+Wo                       8        8        16
    Voicing (10ms update)           2        2         4
    TOTAL                          10       46        56
 
\*---------------------------------------------------------------------------*/

void codec2_encode_1400(struct CODEC2 *c2, unsigned char * bits, short speech[])
{
    MODEL   model;
    float   lsps[LPC_ORD];
    float   ak[LPC_ORD+1];
    float   e;
    int     lsp_indexes[LPC_ORD];
    int     WoE_index;
    int     i;
    unsigned int nbit = 0;

    assert(c2 != NULL);

    memset(bits, '\0',  ((codec2_bits_per_frame(c2) + 7) / 8));

    /* frame 1: - voicing ---------------------------------------------*/

    analyse_one_frame(c2, &model, speech);
    pack(bits, &nbit, model.voiced, 1);
 
    /* frame 2: - voicing, joint Wo & E -------------------------------*/

    analyse_one_frame(c2, &model, &speech[N]);
    pack(bits, &nbit, model.voiced, 1);

    /* need to run this just to get LPC energy */
    e = speech_to_uq_lsps(lsps, ak, c2->Sn, c2->w, LPC_ORD);

    WoE_index = encode_WoE(&model, e, c2->xq_enc);
    pack(bits, &nbit, WoE_index, WO_E_BITS);
 
    /* frame 3: - voicing ---------------------------------------------*/

    analyse_one_frame(c2, &model, &speech[2*N]);
    pack(bits, &nbit, model.voiced, 1);

    /* frame 4: - voicing, joint Wo & E, scalar LSPs ------------------*/

    analyse_one_frame(c2, &model, &speech[3*N]);
    pack(bits, &nbit, model.voiced, 1);
 
    e = speech_to_uq_lsps(lsps, ak, c2->Sn, c2->w, LPC_ORD);
    WoE_index = encode_WoE(&model, e, c2->xq_enc);
    pack(bits, &nbit, WoE_index, WO_E_BITS);
 
    encode_lsps_scalar(lsp_indexes, lsps, LPC_ORD);
    for(i=0; i<LSP_SCALAR_INDEXES; i++) {
	pack(bits, &nbit, lsp_indexes[i], lsp_bits(i));
    }
 
    assert(nbit == (unsigned)codec2_bits_per_frame(c2));
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_decode_1400	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 11 May 2012

  Decodes frames of 56 bits into 320 samples (40ms) of speech.

\*---------------------------------------------------------------------------*/

void codec2_decode_1400(struct CODEC2 *c2, short speech[], const unsigned char * bits)
{
    MODEL   model[4];
    int     lsp_indexes[LPC_ORD];
    float   lsps[4][LPC_ORD];
    int     WoE_index;
    float   e[4];
    float   snr;
    float   ak[4][LPC_ORD+1];
    int     i,j;
    unsigned int nbit = 0;
    float   weight;

    assert(c2 != NULL);

    /* only need to zero these out due to (unused) snr calculation */

    for(i=0; i<4; i++)
	for(j=1; j<=MAX_AMP; j++)
	    model[i].A[j] = 0.0;

    /* unpack bits from channel ------------------------------------*/

    /* this will partially fill the model params for the 4 x 10ms
       frames */

    model[0].voiced = unpack(bits, &nbit, 1);

    model[1].voiced = unpack(bits, &nbit, 1);
    WoE_index = unpack(bits, &nbit, WO_E_BITS);
    decode_WoE(&model[1], &e[1], c2->xq_dec, WoE_index);

    model[2].voiced = unpack(bits, &nbit, 1);

    model[3].voiced = unpack(bits, &nbit, 1);
    WoE_index = unpack(bits, &nbit, WO_E_BITS);
    decode_WoE(&model[3], &e[3], c2->xq_dec, WoE_index);
 
    for(i=0; i<LSP_SCALAR_INDEXES; i++) {
	lsp_indexes[i] = unpack(bits, &nbit, lsp_bits(i));
    }
    decode_lsps_scalar(&lsps[3][0], lsp_indexes, LPC_ORD);
    check_lsp_order(&lsps[3][0], LPC_ORD);
    bw_expand_lsps(&lsps[3][0], LPC_ORD);
 
    /* interpolate ------------------------------------------------*/

    /* Wo and energy are sampled every 20ms, so we interpolate just 1
       10ms frame between 20ms samples */

    interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
    e[0] = interp_energy(c2->prev_e_dec, e[1]);
    interp_Wo(&model[2], &model[1], &model[3]);
    e[2] = interp_energy(e[1], e[3]);
 
    /* LSPs are sampled every 40ms so we interpolate the 3 frames in
       between, then recover spectral amplitudes */

    for(i=0, weight=0.25; i<3; i++, weight += 0.25) {
	interpolate_lsp_ver2(&lsps[i][0], c2->prev_lsps_dec, &lsps[3][0], weight);
    }
    for(i=0; i<4; i++) {
	lsp_to_lpc(&lsps[i][0], &ak[i][0], LPC_ORD);
	aks_to_M2(&ak[i][0], LPC_ORD, &model[i], e[i], &snr, 1); 
	apply_lpc_correction(&model[i]);
    }

    /* synthesise ------------------------------------------------*/

    for(i=0; i<4; i++)
	synthesise_one_frame(c2, &speech[N*i], &model[i], &ak[i][0]);

    /* update memories for next frame ----------------------------*/

    c2->prev_model_dec = model[3];
    c2->prev_e_dec = e[3];
    for(i=0; i<LPC_ORD; i++)
	c2->prev_lsps_dec[i] = lsps[3][i];

}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_encode_1200	     
  AUTHOR......: David Rowe			      
  DATE CREATED: Nov 14 2011 

  Encodes 320 speech samples (40ms of speech) into 48 bits.  

  The codec2 algorithm actually operates internally on 10ms (80
  sample) frames, so we run the encoding algorithm four times:

  frame 0: voicing bit
  frame 1: voicing bit, joint VQ of Wo and E
  frame 2: voicing bit
  frame 3: voicing bit, joint VQ of Wo and E, VQ LSPs

  The bit allocation is:

    Parameter                      frame 2  frame 4   Total
    -------------------------------------------------------
    Harmonic magnitudes (LSPs)      0       27        27
    Energy+Wo                       8        8        16
    Voicing (10ms update)           2        2         4
    Spare                           0        1         1
    TOTAL                          10       38        48
 
\*---------------------------------------------------------------------------*/

void codec2_encode_1200(struct CODEC2 *c2, unsigned char * bits, short speech[])
{
    MODEL   model;
    float   lsps[LPC_ORD];
    float   lsps_[LPC_ORD];
    float   ak[LPC_ORD+1];
    float   e;
    int     lsp_indexes[LPC_ORD];
    int     WoE_index;
    int     i;
    int     spare = 0;
    unsigned int nbit = 0;

    assert(c2 != NULL);

    memset(bits, '\0',  ((codec2_bits_per_frame(c2) + 7) / 8));

    /* frame 1: - voicing ---------------------------------------------*/

    analyse_one_frame(c2, &model, speech);
    pack(bits, &nbit, model.voiced, 1);
 
    /* frame 2: - voicing, joint Wo & E -------------------------------*/

    analyse_one_frame(c2, &model, &speech[N]);
    pack(bits, &nbit, model.voiced, 1);

    /* need to run this just to get LPC energy */
    e = speech_to_uq_lsps(lsps, ak, c2->Sn, c2->w, LPC_ORD);

    WoE_index = encode_WoE(&model, e, c2->xq_enc);
    pack(bits, &nbit, WoE_index, WO_E_BITS);
 
    /* frame 3: - voicing ---------------------------------------------*/

    analyse_one_frame(c2, &model, &speech[2*N]);
    pack(bits, &nbit, model.voiced, 1);

    /* frame 4: - voicing, joint Wo & E, scalar LSPs ------------------*/

    analyse_one_frame(c2, &model, &speech[3*N]);
    pack(bits, &nbit, model.voiced, 1);
 
    e = speech_to_uq_lsps(lsps, ak, c2->Sn, c2->w, LPC_ORD);
    WoE_index = encode_WoE(&model, e, c2->xq_enc);
    pack(bits, &nbit, WoE_index, WO_E_BITS);
 
    encode_lsps_vq(lsp_indexes, lsps, lsps_, LPC_ORD);
    for(i=0; i<LSP_PRED_VQ_INDEXES; i++) {
	pack(bits, &nbit, lsp_indexes[i], lsp_pred_vq_bits(i));
    }
    pack(bits, &nbit, spare, 1);
 
    assert(nbit == (unsigned)codec2_bits_per_frame(c2));
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: codec2_decode_1200	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 14 Feb 2012

  Decodes frames of 48 bits into 320 samples (40ms) of speech.

\*---------------------------------------------------------------------------*/

void codec2_decode_1200(struct CODEC2 *c2, short speech[], const unsigned char * bits)
{
    MODEL   model[4];
    int     lsp_indexes[LPC_ORD];
    float   lsps[4][LPC_ORD];
    int     WoE_index;
    float   e[4];
    float   snr;
    float   ak[4][LPC_ORD+1];
    int     i,j;
    unsigned int nbit = 0;
    float   weight;

    assert(c2 != NULL);

    /* only need to zero these out due to (unused) snr calculation */

    for(i=0; i<4; i++)
	for(j=1; j<=MAX_AMP; j++)
	    model[i].A[j] = 0.0;

    /* unpack bits from channel ------------------------------------*/

    /* this will partially fill the model params for the 4 x 10ms
       frames */

    model[0].voiced = unpack(bits, &nbit, 1);

    model[1].voiced = unpack(bits, &nbit, 1);
    WoE_index = unpack(bits, &nbit, WO_E_BITS);
    decode_WoE(&model[1], &e[1], c2->xq_dec, WoE_index);

    model[2].voiced = unpack(bits, &nbit, 1);

    model[3].voiced = unpack(bits, &nbit, 1);
    WoE_index = unpack(bits, &nbit, WO_E_BITS);
    decode_WoE(&model[3], &e[3], c2->xq_dec, WoE_index);
 
    for(i=0; i<LSP_PRED_VQ_INDEXES; i++) {
	lsp_indexes[i] = unpack(bits, &nbit, lsp_pred_vq_bits(i));
    }
    decode_lsps_vq(lsp_indexes, &lsps[3][0], LPC_ORD);
    check_lsp_order(&lsps[3][0], LPC_ORD);
    bw_expand_lsps(&lsps[3][0], LPC_ORD);
 
    /* interpolate ------------------------------------------------*/

    /* Wo and energy are sampled every 20ms, so we interpolate just 1
       10ms frame between 20ms samples */

    interp_Wo(&model[0], &c2->prev_model_dec, &model[1]);
    e[0] = interp_energy(c2->prev_e_dec, e[1]);
    interp_Wo(&model[2], &model[1], &model[3]);
    e[2] = interp_energy(e[1], e[3]);
 
    /* LSPs are sampled every 40ms so we interpolate the 3 frames in
       between, then recover spectral amplitudes */

    for(i=0, weight=0.25; i<3; i++, weight += 0.25) {
	interpolate_lsp_ver2(&lsps[i][0], c2->prev_lsps_dec, &lsps[3][0], weight);
    }
    for(i=0; i<4; i++) {
	lsp_to_lpc(&lsps[i][0], &ak[i][0], LPC_ORD);
	aks_to_M2(&ak[i][0], LPC_ORD, &model[i], e[i], &snr, 1); 
	apply_lpc_correction(&model[i]);
    }

    /* synthesise ------------------------------------------------*/

    for(i=0; i<4; i++)
	synthesise_one_frame(c2, &speech[N*i], &model[i], &ak[i][0]);

    /* update memories for next frame ----------------------------*/

    c2->prev_model_dec = model[3];
    c2->prev_e_dec = e[3];
    for(i=0; i<LPC_ORD; i++)
	c2->prev_lsps_dec[i] = lsps[3][i];
}


/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: synthesise_one_frame()	     
  AUTHOR......: David Rowe			      
  DATE CREATED: 23/8/2010 

  Synthesise 80 speech samples (10ms) from model parameters.

\*---------------------------------------------------------------------------*/

void synthesise_one_frame(struct CODEC2 *c2, short speech[], MODEL *model, float ak[])
{
    int     i;

    phase_synth_zero_order(model, ak, &c2->ex_phase, LPC_ORD);
    postfilter(model, &c2->bg_est);
    synthesise(c2->Sn_, model, c2->Pn, 1);

    for(i=0; i<N; i++) {
	if (c2->Sn_[i] > 32767.0)
	    speech[i] = 32767;
	else if (c2->Sn_[i] < -32767.0)
	    speech[i] = -32767;
	else
	    speech[i] = c2->Sn_[i];
    }

}

/*---------------------------------------------------------------------------*\
                                                       
  FUNCTION....: analyse_one_frame()   
  AUTHOR......: David Rowe			      
  DATE CREATED: 23/8/2010 

  Extract sinusoidal model parameters from 80 speech samples (10ms of
  speech).
 
\*---------------------------------------------------------------------------*/

void analyse_one_frame(struct CODEC2 *c2, MODEL *model, short speech[])
{
    COMP    Sw[FFT_ENC];
    COMP    Sw_[FFT_ENC];
    COMP    Ew[FFT_ENC];
    float   pitch, snr;
    int     i;

    /* Read input speech */

    for(i=0; i<M-N; i++)
      c2->Sn[i] = c2->Sn[i+N];
    for(i=0; i<N; i++)
      c2->Sn[i+M-N] = speech[i];

    dft_speech(Sw, c2->Sn, c2->w);

    /* Estimate pitch */

    nlp(c2->nlp,c2->Sn,N,M,P_MIN,P_MAX,&pitch,Sw, &c2->prev_Wo_enc);
    model->Wo = TWO_PI/pitch;
    model->L = PI/model->Wo;

    /* estimate model parameters */

    two_stage_pitch_refinement(model, Sw);
    estimate_amplitudes(model, Sw, c2->W);
    snr = est_voicing_mbe(model, Sw, c2->W, Sw_, Ew, c2->prev_Wo_enc);
    //fprintf(stderr,"snr %3.2f  v: %d  Wo: %f prev_Wo: %f\n", 
    //	   snr, model->voiced, model->Wo, c2->prev_Wo_enc);
    c2->prev_Wo_enc = model->Wo;
}
