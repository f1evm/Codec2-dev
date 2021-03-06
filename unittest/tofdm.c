/*---------------------------------------------------------------------------*\

  FILE........: tofdm.c
  AUTHORS.....: David Rowe & Steve Sampson
  DATE CREATED: June 2017

  Tests for the C version of the OFDM modem.  This program
  outputs a file of Octave vectors that are loaded and automatically
  tested against the Octave version of the modem by the Octave script
  tofdm.m

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2017 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2, as
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
#include <complex.h>

#define MODEMPROBE_ENABLE

#include "ofdm_internal.h"
#include "codec2_ofdm.h"
#include "octave.h"
#include "test_bits_ofdm.h"
#include "comp_prim.h"
#include "modem_probe.h"

#define OFDM_NC 16

#define NFRAMES 60
//#define SAMPLE_CLOCK_OFFSET_PPM 100
//#define FOFF_HZ 5.0f


#define OFDM_NCX     16                       /* N Carriers */
#define OFDM_TS     0.018f                   /* Symbol time */
#define OFDM_RS     (1.0f / OFDM_TS)         /* Symbol rate */
#define OFDM_FS     8000.0f                  /* Sample rate */
#define OFDM_BPS    2                        /* Bits per symbol */
#define OFDM_TCP    0.002f                   /* ? */
#define OFDM_NS     8                        /* Symbols per frame (number of rows incl pilot) */
#define OFDM_CENTRE 1500.0f                  /* Center frequency */

/* ? */
#define OFDM_FTWINDOWWIDTH       11
/* Bits per frame (duh) */
#define OFDM_BITSPERFRAME        ((OFDM_NS - 1) * (OFDM_NCX * OFDM_BPS))
/* Rows per frame */
#define OFDM_ROWSPERFRAME        (OFDM_BITSPERFRAME / (OFDM_NCX * OFDM_BPS))
/* Samps per frame */
#define OFDM_SAMPLESPERFRAME     (OFDM_NS * (OFDM_M + OFDM_NCP))

#define OFDM_MAX_SAMPLESPERFRAME (OFDM_SAMPLESPERFRAME + (OFDM_M + OFDM_NCP)/4)
#define OFDM_RXBUF               (3 * OFDM_SAMPLESPERFRAME + 3 * (OFDM_M + OFDM_NCP))



/* To prevent C99 warning */

#define OFDM_M      144                      /* Samples per bare symbol (?) */
#define OFDM_NCP    16                       /* Samples per cyclic prefix */


/*---------------------------------------------------------------------------*\

  FUNCTION....: fs_offset()
  AUTHOR......: David Rowe
  DATE CREATED: May 2015

  Simulates small Fs offset between mod and demod.
  (Note: Won't work with float, works OK with double)

\*---------------------------------------------------------------------------*/

static int fs_offset(COMP out[], COMP in[], int n, float sample_rate_ppm) {
    double f;
    int t1, t2;

    double tin = 0.0;
    int tout = 0;

    while (tin < (double) n) {
      t1 = (int) floor(tin);
      t2 = (int) ceil(tin);

      f = (tin - (double) t1);

      out[tout].real = (1.0 - f) * in[t1].real + f * in[t2].real;
      out[tout].imag = (1.0 - f) * in[t1].imag + f * in[t2].imag;

      tout += 1;
      tin  += 1.0 + sample_rate_ppm / 1E6;
    }

    return tout;
}

#ifndef ARM_MATH_CM4
  #define SINF(a) sinf(a)
  #define COSF(a) cosf(a)
#else
  #define SINF(a) arm_sin_f32(a)
  #define COSF(a) arm_cos_f32(a)
#endif

/*---------------------------------------------------------------------------*\

  FUNCTION....: freq_shift()
  AUTHOR......: David Rowe
  DATE CREATED: 26/4/2012

  Frequency shift modem signal.  The use of complex input and output allows
  single sided frequency shifting (no images).

\*---------------------------------------------------------------------------*/

static void freq_shift(COMP rx_fdm_fcorr[], COMP rx_fdm[], float foff, COMP *foff_phase_rect, int nin) {
    float temp = (TAU * foff / OFDM_FS);
    COMP  foff_rect = { COSF(temp), SINF(temp) };
    int   i;

    for (i = 0; i < nin; i++) {
	*foff_phase_rect = cmult(*foff_phase_rect, foff_rect);
	rx_fdm_fcorr[i] = cmult(rx_fdm[i], *foff_phase_rect);
    }

    /* normalise digital oscillator as the magnitude can drift over time */

    float mag = cabsolute(*foff_phase_rect);
    foff_phase_rect->real /= mag;
    foff_phase_rect->imag /= mag;
}

int main(int argc, char *argv[])
{

    
    struct OFDM   *ofdm = ofdm_create(OFDM_CONFIG_700D);
    int            samples_per_frame = ofdm_get_samples_per_frame(ofdm);
    int            max_samples_per_frame = ofdm_get_max_samples_per_frame(ofdm);

    COMP           tx[samples_per_frame];         /* one frame of tx samples */

    int            rx_bits[OFDM_BITSPERFRAME];    /* one frame of rx bits    */

    /* log arrays */

    int            tx_bits_log[OFDM_BITSPERFRAME*NFRAMES];
    COMP           tx_log[samples_per_frame*NFRAMES];
    //COMP           rx_log[samples_per_frame*NFRAMES];
    COMP *         rx_log;
    COMP           rxbuf_in_log[max_samples_per_frame*NFRAMES];
    COMP           rxbuf_log[OFDM_RXBUF*NFRAMES];
    COMP           rx_sym_log[(OFDM_NS + 3)*NFRAMES][OFDM_NC + 2];
    float          phase_est_pilot_log[OFDM_ROWSPERFRAME*NFRAMES][OFDM_NC];
    COMP           rx_np_log[OFDM_ROWSPERFRAME*OFDM_NC*NFRAMES];
    float          rx_amp_log[OFDM_ROWSPERFRAME*OFDM_NC*NFRAMES];
    float          foff_hz_log[NFRAMES];
    int            rx_bits_log[OFDM_BITSPERFRAME*NFRAMES];
    int            timing_est_log[OFDM_BITSPERFRAME*NFRAMES];
    int            sample_point_log[OFDM_BITSPERFRAME*NFRAMES];

    FILE          *fout;
    int            f,i,j;

    int sample_clock_offset_ppm = 100;
    float foff_hz = 0.1;
    int nframes = 30;

    assert(ofdm != NULL);

    /* Main Loop ---------------------------------------------------------------------*/

    for(f=0; f<NFRAMES; f++) {

	/* --------------------------------------------------------*\
	                          Mod
	\*---------------------------------------------------------*/

        /* todo: add a longer sequence of test bits through
           ofdm_get/put test bits functin similat to cohpsk/fdmdv */

        ofdm_mod(ofdm, (COMP*)tx, test_bits_ofdm);

        /* tx vector logging */

	    memcpy(&tx_bits_log[OFDM_BITSPERFRAME*f], test_bits_ofdm, sizeof(int)*OFDM_BITSPERFRAME);
	    memcpy(&tx_log[samples_per_frame*f], tx, sizeof(COMP)*samples_per_frame);
    }

    /* --------------------------------------------------------*\
	                        Channel
    \*---------------------------------------------------------*/

    /*fs_offset(rx_log, tx_log, samples_per_frame*NFRAMES, sample_clock_offset_ppm);

    COMP foff_phase_rect = {1.0f, 0.0f};

    freq_shift(rx_log, rx_log, foff_hz, &foff_phase_rect, samples_per_frame * NFRAMES);*/

    FILE *cplin = fopen("tofdm_rx_vec","r");
    fseek(cplin,0,SEEK_END);
    size_t file_len = ftell(cplin);
    rewind(cplin);
    size_t rx_samps_in = file_len/sizeof(COMP);
    rx_log = malloc(rx_samps_in*sizeof(COMP));
    fread(rx_log,sizeof(COMP),rx_samps_in,cplin);
    fclose(cplin);
    //fprintf(stderr,"Read %d\n",s);


    /* --------------------------------------------------------*\
	                        Demod
    \*---------------------------------------------------------*/

    /* Init rx with ideal timing so we can test with timing estimation disabled */

    int  Nsam = samples_per_frame*NFRAMES;
    Nsam = rx_samps_in;
    int  prx = 0;
    //int  nin = samples_per_frame + 2*(OFDM_M+OFDM_NCP);
    int nin = ofdm_get_nin(ofdm);
    int  lnew;
    COMP rxbuf_in[max_samples_per_frame];

    /*for (i=0; i<nin; i++,prx++) {
         ofdm->rxbuf[OFDM_RXBUF-nin+i] = rx_log[prx].real + I*rx_log[prx].imag;
    }*/

    int nin_tot = 0;

    modem_probe_init("ofdm","ofdm_test.txt");

    /* disable estimators for initial testing */

    ofdm_set_verbose(ofdm, true);
    ofdm_set_timing_enable(ofdm, true);
    ofdm_set_foff_est_enable(ofdm, true);
    ofdm_set_phase_est_enable(ofdm, true);
    //ofdm->foff_est_hz = 10;

    bool cont = true;
    int ptr = 0;
    for(f=0; cont; f++) {
        // /* For initial testng, timing est is off, so nin is always
        //    fixed.  TODO: we need a constant for rxbuf_in[] size that
        //    is the maximum possible nin */

        // nin = ofdm_get_nin(ofdm);
        
        // nin_tot += nin;

        // assert(nin <= max_samples_per_frame);

        // /* Insert samples at end of buffer, set to zero if no samples
        //    available to disable phase estimation on future pilots on
        //    last frame of simulation. */

        // if ((Nsam-prx) < nin) {
        //     lnew = Nsam-prx;
        // } else {
        //     lnew = nin;
        // }
        // //printf("nin: %d prx: %d lnew: %d\n", nin, prx, lnew);
        // for(i=0; i<nin; i++) {
        //     rxbuf_in[i].real = 0.0;
        //     rxbuf_in[i].imag = 0.0;
        // }

        // if (lnew) {
        //     for(i=0; i<lnew; i++, prx++) {
        //         rxbuf_in[i] = rx_log[prx];
        //     }
        // }

        // if(prx > Nsam){
        //     cont = false;
        //     break;
        // }
        //assert(prx <= max_samples_per_frame*NFRAMES);

        //fprintf(stderr,"NIN:%d\n",nin);
        nin = ofdm_get_nin(ofdm);
        if(ptr + nin >= Nsam){
            break;
        }

        memset(rxbuf_in,0,sizeof(COMP) * nin);

        for(i=0; i<nin && ptr < Nsam; i++,ptr++){
            rxbuf_in[i] = rx_log[ptr];
        }
        ofdm_demod_coarse(ofdm, rx_bits, rxbuf_in);


        if(ptr >= Nsam){
            cont = false;
            //break;
        }

        modem_probe_samp_cft("rxbuf_in_log_c",rxbuf_in,nin);

        if(f<NFRAMES){
            for (i = 0; i < (OFDM_NS + 3); i++) {
                for (j = 0; j < (OFDM_NC + 2); j++) {
                    rx_sym_log[(OFDM_NS + 3)*f+i][j].real = crealf(ofdm->rx_sym[i][j]);
                    rx_sym_log[(OFDM_NS + 3)*f+i][j].imag = cimagf(ofdm->rx_sym[i][j]);
                }
            }
        }
        /* note phase/amp ests the same for each col, but check them all anyway */
        if(f < NFRAMES){
            for (i = 0; i < OFDM_ROWSPERFRAME; i++) {
                for (j = 0; j < OFDM_NC; j++) {
                    phase_est_pilot_log[OFDM_ROWSPERFRAME*f+i][j] = ofdm->aphase_est_pilot_log[OFDM_NC*i+j];
                    rx_amp_log[OFDM_ROWSPERFRAME*OFDM_NC*f+OFDM_NC*i+j] = ofdm->rx_amp[OFDM_NC*i+j];
                }
            }
        }
        modem_probe_samp_i("rx_bits_log_c",rx_bits,OFDM_BITSPERFRAME);
    }

    /*---------------------------------------------------------*\
               Dump logs to Octave file for evaluation
                      by tofdm.m Octave script
    \*---------------------------------------------------------*/

    modem_probe_close();


    fout = fopen("tofdm_out.txt","wt");
    assert(fout != NULL);
    fprintf(fout, "# Created by tofdm.c\n");
    octave_save_complex(fout, "W_c", (COMP*)ofdm->W, OFDM_NC + 2, OFDM_M, OFDM_M);
    octave_save_int(fout, "tx_bits_log_c", tx_bits_log, 1, OFDM_BITSPERFRAME*NFRAMES);
    octave_save_complex(fout, "tx_log_c", (COMP*)tx_log, 1, samples_per_frame*NFRAMES,  samples_per_frame*NFRAMES);
    octave_save_complex(fout, "rx_log_c", (COMP*)rx_log, 1, samples_per_frame*NFRAMES,  samples_per_frame*NFRAMES);
    octave_save_complex(fout, "rx_sym_log_c", (COMP*)rx_sym_log, (OFDM_NS + 3)*NFRAMES, OFDM_NC + 2, OFDM_NC + 2);
    octave_save_float(fout, "phase_est_pilot_log_c", (float*)phase_est_pilot_log, OFDM_ROWSPERFRAME*NFRAMES, OFDM_NC, OFDM_NC);
    octave_save_float(fout, "rx_amp_log_c", (float*)rx_amp_log, 1, OFDM_ROWSPERFRAME*OFDM_NC*NFRAMES, OFDM_ROWSPERFRAME*OFDM_NC*NFRAMES);

    fclose(fout);

    ofdm_destroy(ofdm);

    return 0;
}

