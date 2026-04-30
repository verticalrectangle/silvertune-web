use nih_plug::prelude::*;
use silvertune_dsp::{grain::GrainShifter, scale, yin::Yin};
use std::sync::Arc;

const DETUNE_CENTS: f64 = 0.035;

struct SilvertunePlugin {
    params: Arc<SilvertuneParams>,
    yin: Option<Yin>,
    shifter: GrainShifter,
    doubler: GrainShifter,
    detected_note: i32,
    corrected_note: i32,
}

#[derive(Params)]
struct SilvertuneParams {
    #[id = "key"]
    key: IntParam,
    #[id = "scale"]
    scale: IntParam,
    #[id = "tune"]
    tune: FloatParam,
    #[id = "wide"]
    wide: FloatParam,
}

impl Default for SilvertunePlugin {
    fn default() -> Self {
        Self {
            params: Arc::new(SilvertuneParams::default()),
            yin: None,
            shifter: GrainShifter::new(),
            doubler: GrainShifter::new(),
            detected_note: -1,
            corrected_note: -1,
        }
    }
}

impl Default for SilvertuneParams {
    fn default() -> Self {
        Self {
            key: IntParam::new("Key", 0, IntRange::Linear { min: 0, max: 11 })
                .with_value_to_string(Arc::new(|v| {
                    let names = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
                    names.get(v as usize).unwrap_or(&"C").to_string()
                })),
            scale: IntParam::new("Scale", 1, IntRange::Linear { min: 0, max: 2 })
                .with_value_to_string(Arc::new(|v| {
                    match v as usize {
                        0 => "Chromatic".to_string(),
                        1 => "Major".to_string(),
                        2 => "Minor".to_string(),
                        _ => "Major".to_string(),
                    }
                })),
            tune: FloatParam::new("Tune", 1.0, FloatRange::Linear { min: 0.0, max: 1.0 })
                .with_unit("%")
                .with_value_to_string(formatters::v2s_f32_percentage(0)),
            wide: FloatParam::new("Wide", 0.0, FloatRange::Linear { min: 0.0, max: 1.0 })
                .with_unit("%")
                .with_value_to_string(formatters::v2s_f32_percentage(0)),
        }
    }
}

impl Plugin for SilvertunePlugin {
    const NAME: &'static str = "Silvertune";
    const VENDOR: &'static str = "Silvertune";
    const URL: &'static str = "https://silvertune.live";
    const EMAIL: &'static str = "";
    const VERSION: &'static str = env!("CARGO_PKG_VERSION");

    const AUDIO_IO_LAYOUTS: &'static [AudioIOLayout] = &[AudioIOLayout {
        main_input_channels: NonZeroU32::new(1),
        main_output_channels: NonZeroU32::new(1),
        ..AudioIOLayout::const_default()
    }];

    const MIDI_INPUT: MidiConfig = MidiConfig::None;
    const MIDI_OUTPUT: MidiConfig = MidiConfig::None;
    const SAMPLE_ACCURATE_AUTOMATION: bool = false;

    type SysExMessage = ();
    type BackgroundTask = ();

    fn params(&self) -> Arc<dyn Params> {
        self.params.clone()
    }

    fn initialize(
        &mut self,
        _audio_io_layout: &AudioIOLayout,
        buffer_config: &BufferConfig,
        _context: &mut impl InitContext<Self>,
    ) -> bool {
        self.yin = Some(Yin::new(buffer_config.sample_rate));
        true
    }

    fn reset(&mut self) {
        self.shifter = GrainShifter::new();
        self.doubler = GrainShifter::new();
        self.detected_note = -1;
        self.corrected_note = -1;
        if let Some(yin) = &mut self.yin {
            *yin = Yin::new(yin.sample_rate());
        }
    }

    fn process(
        &mut self,
        buffer: &mut Buffer,
        _aux: &mut AuxiliaryBuffers,
        _context: &mut impl ProcessContext<Self>,
    ) -> ProcessStatus {
        let key       = self.params.key.value() as u8;
        let scale_idx = self.params.scale.value() as u8;
        let tune      = self.params.tune.value() as f64;
        let wide      = self.params.wide.value();

        let yin = match &mut self.yin {
            Some(y) => y,
            None    => return ProcessStatus::Error("not initialized"),
        };

        for channel_samples in buffer.iter_samples() {
            for sample in channel_samples {
                if yin.push(*sample) && yin.pitch_hz > 0.0 && yin.confidence > 0.5 {
                    let midi_f = silvertune_dsp::yin::hz_to_midi(yin.pitch_hz);
                    self.detected_note = midi_f.round() as i32;
                    self.corrected_note = scale::quantize(midi_f, key, scale_idx);
                }

                let ratio = if self.detected_note >= 0 && self.corrected_note >= 0 {
                    let raw = scale::midi_to_ratio(self.detected_note as f32, self.corrected_note);
                    1.0 + (raw - 1.0) * tune
                } else {
                    1.0
                };

                let wet = self.shifter.process(*sample, ratio);
                let dbl = self.doubler.process(*sample, ratio * (1.0 + DETUNE_CENTS));
                *sample = wet + dbl * wide;
            }
        }

        ProcessStatus::Normal
    }
}

impl ClapPlugin for SilvertunePlugin {
    const CLAP_ID: &'static str = "live.silvertune.plugin";
    const CLAP_DESCRIPTION: Option<&'static str> = Some("Autotune pitch correction");
    const CLAP_MANUAL_URL: Option<&'static str> = None;
    const CLAP_SUPPORT_URL: Option<&'static str> = Some("https://silvertune.live");
    const CLAP_FEATURES: &'static [ClapFeature] = &[
        ClapFeature::AudioEffect,
        ClapFeature::Pitch,
        ClapFeature::Mono,
    ];
}

impl Vst3Plugin for SilvertunePlugin {
    const VST3_CLASS_ID: [u8; 16] = *b"SilvertunePlug!0";
    const VST3_SUBCATEGORIES: &'static [Vst3SubCategory] =
        &[Vst3SubCategory::Fx, Vst3SubCategory::PitchShift];
}

nih_export_clap!(SilvertunePlugin);
nih_export_vst3!(SilvertunePlugin);
