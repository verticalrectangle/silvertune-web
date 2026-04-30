use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{BufferSize, SampleRate, StreamConfig};
use crossbeam_channel::Sender;
use std::sync::atomic::Ordering;

use crate::dsp::{grain::GrainShifter, scale, yin::Yin};
use crate::params::SharedParams;

#[derive(Clone)]
pub struct PitchEvent {
    pub detected_note: i32,
    pub corrected_note: i32,
    pub rms: f32,
}

const POST_EVERY: usize = 600;
const DETUNE_CENTS: f64 = 0.035; // wide doubler detune, matches worklet DETUNE

pub fn start(params: SharedParams, tx: Sender<PitchEvent>) {
    std::thread::Builder::new()
        .name("silvertune-audio".into())
        .spawn(move || run_audio(params, tx))
        .expect("audio thread spawn");
}

fn run_audio(params: SharedParams, tx: Sender<PitchEvent>) {
    let host = cpal::default_host();

    let input_dev = host
        .default_input_device()
        .expect("no input device");
    let output_dev = host
        .default_output_device()
        .expect("no output device");

    let in_cfg = input_dev
        .default_input_config()
        .expect("no input config");
    let out_cfg = output_dev
        .default_output_config()
        .expect("no output config");

    let sample_rate = in_cfg.sample_rate().0 as f32;
    log::info!(
        "Audio: {} Hz  input={} channels  output={} channels",
        sample_rate,
        in_cfg.channels(),
        out_cfg.channels()
    );

    let stream_cfg = StreamConfig {
        channels: 1,
        sample_rate: SampleRate(sample_rate as u32),
        buffer_size: BufferSize::Fixed(128),
    };

    // Shared DSP state between input and output via a ringbuf-style channel
    let (proc_tx, proc_rx) = crossbeam_channel::bounded::<f32>(8192);

    // Clone params for the input closure
    let params_in = params.clone();
    let tx_in = tx.clone();

    let mut yin = Yin::new(sample_rate);
    let mut shifter = GrainShifter::new();
    let mut doubler = GrainShifter::new();
    let mut detected_note = -1i32;
    let mut corrected_note = -1i32;
    let mut rms_acc = 0.0f32;
    let mut rms_n = 0usize;
    let mut post_counter = 0usize;

    let input_stream = input_dev
        .build_input_stream(
            &stream_cfg,
            move |data: &[f32], _| {
                let key = params_in.key.load(Ordering::Relaxed) as u8;
                let scale_idx = params_in.scale.load(Ordering::Relaxed) as u8;
                let tune = params_in.tune();
                let wide = params_in.wide();

                for &sample in data {
                    // YIN
                    let new_pitch = yin.push(sample);
                    if new_pitch && yin.pitch_hz > 0.0 && yin.confidence > 0.5 {
                        let midi_f = crate::dsp::yin::hz_to_midi(yin.pitch_hz);
                        detected_note = midi_f.round() as i32;
                        corrected_note = scale::quantize(midi_f, key, scale_idx);
                    }

                    // Pitch ratio
                    let ratio = if detected_note >= 0 && corrected_note >= 0 {
                        let raw = scale::midi_to_ratio(
                            detected_note as f32,
                            corrected_note,
                        );
                        let blended = 1.0 + (raw - 1.0) * tune as f64;
                        blended
                    } else {
                        1.0
                    };

                    let wet = shifter.process(sample, ratio);
                    let dbl = doubler.process(sample, ratio * (1.0 + DETUNE_CENTS));
                    let out = wet + dbl * wide as f32;

                    let _ = proc_tx.try_send(out);

                    // RMS accumulation
                    rms_acc += sample * sample;
                    rms_n += 1;
                    post_counter += 1;
                    if post_counter >= POST_EVERY {
                        let rms = if rms_n > 0 { (rms_acc / rms_n as f32).sqrt() } else { 0.0 };
                        let _ = tx_in.try_send(PitchEvent {
                            detected_note,
                            corrected_note,
                            rms,
                        });
                        rms_acc = 0.0;
                        rms_n = 0;
                        post_counter = 0;
                    }
                }
            },
            |e| log::error!("input stream error: {e}"),
            None,
        )
        .expect("input stream");

    // Output stream — channels may differ from input
    let out_channels = out_cfg.channels() as usize;
    let out_stream_cfg = StreamConfig {
        channels: out_cfg.channels(),
        sample_rate: SampleRate(sample_rate as u32),
        buffer_size: BufferSize::Fixed(128),
    };

    let output_stream = output_dev
        .build_output_stream(
            &out_stream_cfg,
            move |data: &mut [f32], _| {
                let frame_count = data.len() / out_channels;
                for f in 0..frame_count {
                    let s = proc_rx.try_recv().unwrap_or(0.0);
                    for ch in 0..out_channels {
                        data[f * out_channels + ch] = s;
                    }
                }
            },
            |e| log::error!("output stream error: {e}"),
            None,
        )
        .expect("output stream");

    input_stream.play().expect("play input");
    output_stream.play().expect("play output");

    log::info!("Audio streams running");

    // Keep streams alive forever
    std::thread::park();
}
