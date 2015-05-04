% test_foff.m
% David Rowe April 2015
%
% Octave script for testing the cohpsk freq offset estimator

graphics_toolkit ("gnuplot");
more off;

cohpsk;
fdmdv;
autotest;

rand('state',1); 
randn('state',1);

% Core function for testing frequency offset estimator.  Performs one test

function sim_out = freq_off_est_test(sim_in)
  global Nfilter;
  global M;

  Rs = 50;
  Nc = 4;
  Nd = 2;
  framesize = 32;
  Fs = 8000;
  Fcentre = 1500;

  afdmdv.Nsym = 2;
  afdmdv.Nt = 3;

  afdmdv.Fs = 8000;
  afdmdv.Nc = Nd*Nc-1;
  afdmdv.Rs = Rs;
  afdmdv.M  = afdmdv.Fs/afdmdv.Rs;
  afdmdv.tx_filter_memory = zeros(afdmdv.Nc+1, Nfilter);
  afdmdv.Nfilter =  Nfilter;
  afdmdv.gt_alpha5_root = gen_rn_coeffs(0.5, 1/Fs, Rs, afdmdv.Nsym, afdmdv.M);
  afdmdv.Fsep = 75;
  afdmdv.phase_tx = ones(afdmdv.Nc+1,1);
  freq_hz = afdmdv.Fsep*( -Nc*Nd/2 - 0.5 + (1:Nc*Nd) );
  afdmdv.freq_pol = 2*pi*freq_hz/Fs;
  afdmdv.freq = exp(j*afdmdv.freq_pol);
  afdmdv.Fcentre = 1500;

  afdmdv.fbb_rect = exp(j*2*pi*Fcentre/Fs);
  afdmdv.fbb_phase_tx = 1;
  afdmdv.fbb_phase_rx = 1;
  afdmdv.phase_rx = ones(afdmdv.Nc+1,1);

  nin = M;

  P = afdmdv.P = 4;
  afdmdv.Nfilter = afdmdv.Nsym*afdmdv.M;
  afdmdv.rx_filter_mem_timing = zeros(afdmdv.Nc+1, afdmdv.Nt*afdmdv.P);
  afdmdv.Nfiltertiming = afdmdv.M + afdmdv.Nfilter + afdmdv.M;
  afdmdv.rx_filter_memory = zeros(afdmdv.Nc+1, afdmdv.Nfilter);

  acohpsk = standard_init();
  acohpsk.framesize        = framesize;
  acohpsk.ldpc_code        = 0;
  acohpsk.ldpc_code_rate   = 1;
  acohpsk.Nc               = Nc;
  acohpsk.Rs               = Rs;
  acohpsk.Ns               = 4;
  acohpsk.Nd               = Nd;
  acohpsk.modulation       = 'qpsk';
  acohpsk.do_write_pilot_file = 0;
  acohpsk = symbol_rate_init(acohpsk);
  acohpsk.Ncm  = 10*acohpsk.Nsymbrowpilot*M;
  acohpsk.coarse_mem  = zeros(1,acohpsk.Ncm);
  acohpsk.Ndft = 2^(ceil(log2(acohpsk.Ncm)));
 
  frames    = sim_in.frames;
  EsNodB    = sim_in.EsNodB;
  foff      = sim_in.foff;
  dfoff     = sim_in.dfoff;
  fading_en = sim_in.fading_en;

  EsNo = 10^(EsNodB/10);
  hf_delay_ms = 2;
  phase_ch = 1;

  rand('state',1); 
  tx_bits_coh = round(rand(1,framesize*10));
  ptx_bits_coh = 1;
  [spread spread_2ms hf_gain] = init_hf_model(Fs, Fs, frames*acohpsk.Nsymbrowpilot*afdmdv.M);

  hf_n = 1;
  nhfdelay = floor(hf_delay_ms*Fs/1000);
  ch_fdm_delay = zeros(1, acohpsk.Nsymbrowpilot*M + nhfdelay);
  
  sync = 0; next_sync = 1;
  sync_start = 1;
  freq_offset_log = [];
  sync_time_log = [];
  ch_fdm_frame_log = [];
  ch_symb_log = [];
  tx_fdm_frame_log = [];

  for f=1:frames

    acohpsk.frame = f;

    %
    % Mod --------------------------------------------------------------------
    %

    tx_bits = tx_bits_coh(ptx_bits_coh:ptx_bits_coh+framesize-1);
    ptx_bits_coh += framesize;
    if ptx_bits_coh > length(tx_bits_coh)
      ptx_bits_coh = 1;
    end 

    [tx_symb tx_bits] = bits_to_qpsk_symbols(acohpsk, tx_bits, [], []);

    tx_fdm_frame = [];
    for r=1:acohpsk.Nsymbrowpilot
      tx_onesymb = tx_symb(r,:);
      [tx_baseband afdmdv] = tx_filter(afdmdv, tx_onesymb);
      [tx_fdm afdmdv] = fdm_upconvert(afdmdv, tx_baseband);
      tx_fdm_frame = [tx_fdm_frame tx_fdm];
    end
    tx_fdm_frame_log = [tx_fdm_frame_log tx_fdm_frame];

    %
    % Channel --------------------------------------------------------------------
    %

    ch_fdm_frame = zeros(1,acohpsk.Nsymbrowpilot*M);
    for i=1:acohpsk.Nsymbrowpilot*M
      foff_rect = exp(j*2*pi*foff/Fs);
      foff += dfoff;
      phase_ch *= foff_rect;
      ch_fdm_frame(i) = tx_fdm_frame(i) * phase_ch;
    end
    phase_ch /= abs(phase_ch);

    if fading_en
      ch_fdm_delay(1:nhfdelay) = ch_fdm_delay(acohpsk.Nsymbrowpilot*M+1:nhfdelay+acohpsk.Nsymbrowpilot*M);
      ch_fdm_delay(nhfdelay+1:nhfdelay+acohpsk.Nsymbrowpilot*M) = ch_fdm_frame;

      for i=1:acohpsk.Nsymbrowpilot*M
        ahf_model = hf_gain*(spread(hf_n)*ch_fdm_frame(i) + spread_2ms(hf_n)*ch_fdm_delay(i));
        ch_fdm_frame(i) = ahf_model;
        hf_n++;
      end
    end

    % each carrier has power = 2, total power 2Nc, total symbol rate NcRs, noise BW B=Fs
    % Es/No = (C/Rs)/(N/B), N = var = 2NcFs/NcRs(Es/No) = 2Fs/Rs(Es/No)

    variance = 2*Fs/(acohpsk.Rs*EsNo);
    uvnoise = sqrt(0.5)*(randn(1,acohpsk.Nsymbrowpilot*M) + j*randn(1,acohpsk.Nsymbrowpilot*M));
    noise = sqrt(variance)*uvnoise;

    ch_fdm_frame += noise;
    ch_fdm_frame_log = [ch_fdm_frame_log ch_fdm_frame];

    %
    % Try to achieve sync --------------------------------------------------------------------
    %

    next_sync = sync;

    if sync == 0
      next_sync = 2;
      acohpsk.f_est = Fcentre;
    end

    [rx_fdm_frame_bb afdmdv.fbb_phase_rx] = freq_shift(ch_fdm_frame, -acohpsk.f_est, Fs, afdmdv.fbb_phase_rx);

    for r=1:acohpsk.Nsymbrowpilot

      % downconvert each FDM carrier to Nc separate baseband signals

      [rx_baseband afdmdv] = fdm_downconvert(afdmdv, rx_fdm_frame_bb(1+(r-1)*M:r*M), nin);
      [rx_filt afdmdv] = rx_filter(afdmdv, rx_baseband, nin);
      [rx_onesym rx_timing env afdmdv] = rx_est_timing(afdmdv, rx_filt, nin);     

      ch_symb(r,:) = rx_onesym;
    end
    ch_symb_log = [ch_symb_log; ch_symb];

    % coarse timing (frame sync) and initial fine freq est ---------------------------------------------
  
    [next_sync acohpsk] = frame_sync_fine_freq_est(acohpsk, ch_symb, sync, next_sync);

    % if we've acheived sync gather stats

    if (next_sync == 4) 
       freq_offset_log = [freq_offset_log acohpsk.f_fine_est+foff];
       sync_time_log = [sync_time_log f-sync_start];
       sync = 0; next_sync = 2; sync_start = f;
    end

    %printf("f: %d sync: %d next_sync: %d\n", f, sync, next_sync);
    [sync acohpsk] = sync_state_machine(acohpsk, sync, next_sync);

  end

  % ftx=fopen("coarse_tx.raw","wb"); fwrite(ftx, 1000*ch_fdm_frame_log, "short"); fclose(ftx);

  sim_out.freq_offset_log = freq_offset_log;
  sim_out.sync_time_log = sync_time_log;
  sim_out.ch_fdm_frame_log = ch_fdm_frame_log;
  sim_out.ch_symb_log = ch_symb_log;
  sim_out.tx_fdm_frame_log = tx_fdm_frame_log;
endfunction


function freq_off_est_test_single
  sim_in.frames    = 100;
  sim_in.EsNodB    = 20;
  sim_in.foff      = -15;
  sim_in.dfoff     = 0;
  sim_in.fading_en = 1;

  sim_out = freq_off_est_test(sim_in);

  figure(1)
  subplot(211)
  plot(sim_out.freq_offset_log)
  subplot(212)
  hist(sim_out.freq_offset_log)

  figure(2)
  subplot(211)
  plot(sim_out.sync_time_log)
  subplot(212)
  hist(sim_out.sync_time_log)

  figure(3)
  subplot(211)
  plot(real(sim_out.tx_fdm_frame_log(1:2*960)))
  subplot(212)
  plot(real(sim_out.ch_symb_log(1:24,:)),'+')
endfunction


function [freq_off_log EsNodBSet] = freq_off_est_test_curves
  EsNodBSet = [20 12 8];

  sim_in.frames    = 100;
  sim_in.foff      = -20;
  sim_in.dfoff     = 0;
  sim_in.fading_en = 1;
  freq_off_log = 1E6*ones(sim_in.frames, length(EsNodBSet) );
  sync_time_log = 1E6*ones(sim_in.frames, length(EsNodBSet) );

  for i=1:length(EsNodBSet)
    sim_in.EsNodB = EsNodBSet(i);
    printf("%f\n", sim_in.EsNodB);

    sim_out = freq_off_est_test(sim_in);
    freq_off_log(1:length(sim_out.freq_offset_log),i) = sim_out.freq_offset_log;
    sync_time_log(1:length(sim_out.sync_time_log),i) = sim_out.sync_time_log;
  end

  figure(1)
  clf
  hold on;
  for i=1:length(EsNodBSet)
    data = freq_off_log(find(freq_off_log(:,i) < 1E6),i);
    s = std(data);
    m = mean(data);
    stdbar = [m-s; m+s];
    plot(EsNodBSet(i), data, '+')
    plot([EsNodBSet(i) EsNodBSet(i)]+0.5, stdbar,'+-')
  end
  hold off

  axis([6 22 -25 25])
  if sim_in.fading_en
    title_str = sprintf('foff = %d Hz Fading', sim_in.foff);
  else
    title_str = sprintf('foff = %d Hz AWGN', sim_in.foff);
  end
  title(title_str);
  xlabel('Es/No (dB)')
  ylabel('freq offset error (Hz)');
 
  figure(2)
  clf
  hold on;
  for i=1:length(EsNodBSet)
    leg = sprintf("%d;%d dB;", i, EsNodBSet(i));
    plot(freq_off_log(find(freq_off_log(:,i) < 1E6),i),leg)
  end
  hold off;
  title(title_str);
  xlabel('test');
  ylabel('freq offset error (Hz)');
  legend('boxoff');

  figure(3)
  clf
  hold on;
  for i=1:length(EsNodBSet)
    data = sync_time_log(find(sync_time_log(:,i) < 1E6),i);
    if length(data) 
      s = std(data);
      m = mean(data);
      stdbar = [m-s; m+s];
      plot(EsNodBSet(i), data, '+')
      plot([EsNodBSet(i) EsNodBSet(i)]+0.5, stdbar,'+-')
    end
  end 
  hold off;
  axis([6 22 0 10])
  ylabel('sync time (frames)')
  xlabel('Es/No (dB)');
  title(title_str);

  figure(4)
  clf
  hold on;
  for i=1:length(EsNodBSet)
    leg = sprintf("%d;%d dB;", i, EsNodBSet(i));
    plot(sync_time_log(find(sync_time_log(:,i) < 1E6),i),leg)
  end
  hold off;
  title(title_str);
  xlabel('test');
  ylabel('sync time (frames)');
  legend('boxoff');

endfunction


freq_off_est_test_single;
%freq_off_est_test_curves;

% 1. start with +/- 20Hz offset
% 2. Measure frames to sync.  How to define sync?  Foff to withn 1 Hz. Sync state
%    Need to see if we get false sync
% 3. Try shortened filter
% 4. Extend to parallel demods at +/- 
