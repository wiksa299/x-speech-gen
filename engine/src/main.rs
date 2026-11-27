mod locale;
mod prepare;
mod transport;

use anyhow::{Context, Result, bail, ensure};
use prepare::{Annotation, RadioProfile, Rule, prepare};
use ptts::{
    checkpoint::{Checkpoint, ResolveOptions},
    preprocess::{Lang, Normalize},
    synth::{DeviceKind, Quant, SpeechOptions, Synth},
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::{HashMap, HashSet},
    fs,
    io::{BufRead, BufReader},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
    time::Instant,
};

const RATE: u32 = 24_000;
const MAX_SAMPLES: usize = RATE as usize * 60;
const MODELS: [(&str, &[&str]); 3] = [
    ("phonon-31m-en", &["en"]),
    ("phonon-40m-en", &["en"]),
    ("phonon-40m-multilingual", &["en", "fr", "es", "de", "pt"]),
];
const DEFAULT_MODEL: &str = "phonon-40m-en";

#[derive(Deserialize, Serialize)]
struct Voice {
    voice_id: String,
    name: String,
    language: String,
    locale: String,
    #[serde(skip_serializing)]
    filename: String,
}

#[derive(Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct Options {
    model: Option<String>,
    language: Option<String>,
    voice: Option<String>,
    #[serde(default)]
    radio_profile: RadioProfile,
    temperature: Option<f32>,
    seed: Option<u64>,
    guidance: Option<f32>,
    duration_delta: Option<f32>,
    max_tokens_per_chunk: Option<usize>,
    gain_db: Option<f32>,
    #[serde(default)]
    lexicon: Vec<Rule>,
    #[serde(default)]
    annotations: Vec<Annotation>,
}

struct Settings {
    temperature: f32,
    seed: u64,
    guidance: f32,
    duration_delta: f32,
    max_tokens: usize,
    gain: f32,
}

impl Options {
    fn settings(&self) -> Result<Settings> {
        let temperature = self.temperature.unwrap_or(0.3);
        let guidance = self.guidance.unwrap_or(1.0);
        let duration_delta = self.duration_delta.unwrap_or(0.0);
        let max_tokens = self.max_tokens_per_chunk.unwrap_or(50);
        let gain_db = self.gain_db.unwrap_or(0.0);
        ensure!(
            (0.0..=2.0).contains(&temperature),
            crate::locale::message("error.temperature")
        );
        ensure!(
            (1.0..=5.0).contains(&guidance),
            crate::locale::message("error.guidance")
        );
        ensure!(
            (-0.5..=0.5).contains(&duration_delta),
            crate::locale::message("error.duration")
        );
        ensure!(
            (1..=200).contains(&max_tokens),
            crate::locale::message("error.tokens")
        );
        ensure!(
            (-24.0..=12.0).contains(&gain_db),
            crate::locale::message("error.gain")
        );
        Ok(Settings {
            temperature,
            seed: self.seed.unwrap_or(42),
            guidance,
            duration_delta,
            max_tokens,
            gain: 10.0_f32.powf(gain_db / 20.0),
        })
    }
}

#[derive(Serialize)]
struct Model {
    id: &'static str,
    languages: &'static [&'static str],
    available: bool,
    format: &'static str,
    guidance: bool,
}

struct Loaded {
    model: String,
    voices: [Option<String>; 2],
    next_voice: usize,
    duration_delta: f32,
    synth: Synth,
}

struct Engine {
    root: PathBuf,
    voices: Vec<Voice>,
    models: Vec<Model>,
    loaded: Option<Loaded>,
}

const VOICE_SLOTS: [&str; 2] = ["selected", "alternate"];

impl Engine {
    fn open(root: PathBuf) -> Result<Self> {
        let mut voices = Vec::new();
        let mut ids = HashSet::new();
        let catalog = root.join("voices.jsonl");
        if catalog.is_file() {
            ensure!(
                catalog.metadata()?.len() <= 256 * 1024,
                crate::locale::message("error.catalog_large")
            );
            for (index, line) in BufReader::new(fs::File::open(&catalog)?)
                .lines()
                .enumerate()
            {
                let line = line?;
                if line.trim().is_empty() {
                    continue;
                }
                let voice: Voice = serde_json::from_str(&line).with_context(|| {
                    crate::locale::format_message(
                        "error.catalog_line",
                        &[
                            ("path", &catalog.display().to_string()),
                            ("line", &(index + 1).to_string()),
                        ],
                    )
                })?;
                ensure!(
                    !voice.voice_id.is_empty()
                        && voice.voice_id.len() <= 48
                        && voice
                            .voice_id
                            .bytes()
                            .all(|byte| byte.is_ascii_alphanumeric() || b"-_".contains(&byte))
                        && voice.filename == format!("voices/{}.safetensors", voice.voice_id)
                        && voice.name.len() <= 120
                        && voice.locale.len() <= 24
                        && ["en", "fr", "es", "de", "pt"].contains(&voice.language.as_str()),
                    crate::locale::message("error.catalog_voice")
                );
                ensure!(
                    ids.insert(voice.voice_id.clone()) && ids.len() <= 256,
                    crate::locale::message("error.catalog_ids")
                );
                if root.join(&voice.filename).is_file() {
                    voices.push(voice);
                }
            }
            voices.sort_by(|a, b| {
                (&a.language, &a.name, &a.voice_id).cmp(&(&b.language, &b.name, &b.voice_id))
            });
        }
        let models = MODELS
            .iter()
            .map(|&(id, languages)| {
                let path = root.join("models").join(id);
                let format = if path.join("model.q8.gguf").is_file() {
                    "q8"
                } else if path.join("model.safetensors").is_file() {
                    "safetensors"
                } else {
                    ""
                };
                let guidance = fs::read(path.join("config.json"))
                    .ok()
                    .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
                    .and_then(|value| value.get("cfg_null_audio_empty").and_then(Value::as_bool))
                    .unwrap_or(false);
                Model {
                    id,
                    languages,
                    format,
                    guidance,
                    available: !format.is_empty()
                        && path.join("config.json").is_file()
                        && path.join("tokenizer.json").is_file()
                        && path.join("default-voice.safetensors").is_file(),
                }
            })
            .collect();
        Ok(Self {
            root,
            models,
            voices,
            loaded: None,
        })
    }

    fn capabilities(&self) -> Value {
        json!({"models":self.models,"voices":self.voices,
            "radio_profiles":["sera","faa"],
            "aviation_tags":2,
            "limits":{"temperature":[0.0,2.0],"guidance":[1.0,5.0],"duration_delta":[-0.5,0.5],
                      "max_tokens_per_chunk":[1,200],"gain_db":[-24.0,12.0]}})
    }

    fn resolve<'a>(&'a self, options: &'a Options) -> Result<(&'a Model, Lang, &'a str, PathBuf)> {
        let id = options.model.as_deref().unwrap_or(DEFAULT_MODEL);
        let model = self
            .models
            .iter()
            .find(|model| model.id == id && model.available)
            .with_context(|| crate::locale::message("error.model"))?;
        let voice_id = options.voice.as_deref().unwrap_or("default");
        let selected = if voice_id == "default" {
            None
        } else {
            Some(
                self.voices
                    .iter()
                    .find(|voice| voice.voice_id == voice_id)
                    .with_context(|| crate::locale::message("error.voice_id"))?,
            )
        };
        let language = options
            .language
            .as_deref()
            .or(selected.map(|voice| voice.language.as_str()))
            .unwrap_or("en");
        ensure!(
            model.languages.contains(&language),
            crate::locale::message("error.language_model")
        );
        let lang = language
            .parse::<Lang>()
            .map_err(|_| anyhow::anyhow!(crate::locale::message("error.language")))?;
        if let Some(voice) = selected {
            ensure!(
                voice.language == language,
                crate::locale::message("error.language_voice")
            );
            return Ok((
                model,
                lang,
                &voice.voice_id,
                self.root.join(&voice.filename),
            ));
        }
        if language != "en" {
            let preferred = match language {
                "fr" => "Hélène",
                "es" => "Celia",
                "de" => "Mika",
                _ => "Marcelo",
            };
            let voice = self
                .voices
                .iter()
                .find(|v| v.language == language && v.name == preferred)
                .or_else(|| self.voices.iter().find(|v| v.language == language))
                .with_context(|| crate::locale::message("error.no_voice"))?;
            return Ok((
                model,
                lang,
                &voice.voice_id,
                self.root.join(&voice.filename),
            ));
        }
        Ok((
            model,
            lang,
            "default",
            self.root
                .join("models")
                .join(id)
                .join("default-voice.safetensors"),
        ))
    }

    fn loaded_model(&self) -> Option<&str> {
        self.loaded.as_ref().map(|loaded| loaded.model.as_str())
    }

    fn preload(&mut self, options: &Options) -> Result<()> {
        let (model, _, voice, path) = self.resolve(options)?;
        let (id, format, voice) = (model.id, model.format, voice.to_owned());
        self.load_voice(id, format, &voice, &path)?;
        Ok(())
    }

    fn load_voice(
        &mut self,
        id: &str,
        format: &str,
        voice: &str,
        voice_path: &Path,
    ) -> Result<(bool, usize)> {
        let cold = self.loaded.as_ref().is_none_or(|loaded| loaded.model != id);
        if cold {
            self.loaded = None;
            let quant = if format == "q8" {
                Quant::Q80
            } else {
                Quant::F32
            };
            let checkpoint = Checkpoint::resolve(
                self.root.join("models").join(id),
                ResolveOptions {
                    quant,
                    weights: None,
                },
            )?;
            let synth = checkpoint
                .builder(Normalize::OFF)
                .device(DeviceKind::Cpu)
                .build()?;
            ensure!(
                synth.sample_rate() == RATE,
                crate::locale::message("error.rate")
            );
            self.loaded = Some(Loaded {
                model: id.to_owned(),
                voices: [None, None],
                next_voice: 0,
                duration_delta: 0.0,
                synth,
            });
        }
        let loaded = self.loaded.as_mut().unwrap();
        let slot = match loaded
            .voices
            .iter()
            .position(|cached| cached.as_deref() == Some(voice))
        {
            Some(slot) => slot,
            None => {
                let slot = loaded.next_voice;
                loaded.synth.add_voice_file(VOICE_SLOTS[slot], voice_path)?;
                loaded.voices[slot] = Some(voice.to_owned());
                slot
            }
        };
        loaded.next_voice = 1 - slot;
        Ok((cold, slot))
    }

    fn render(
        &mut self,
        text: &str,
        options: &Options,
        mut stopped: impl FnMut() -> bool,
        mut samples: impl FnMut(usize, &[f32]) -> Result<()>,
        mut prepared_text: impl FnMut(&str) -> Result<()>,
    ) -> Result<Value> {
        let began = Instant::now();
        let settings = options.settings()?;
        let (model, lang, voice, voice_path) = self.resolve(options)?;
        ensure!(
            settings.guidance == 1.0 || model.guidance,
            crate::locale::message("error.no_guidance")
        );
        let (id, format, voice) = (model.id, model.format, voice.to_owned());
        let prepared = prepare(
            text,
            lang,
            options.radio_profile,
            &options.lexicon,
            &options.annotations,
        )?;
        prepared_text(&prepared)?;
        if stopped() {
            bail!(crate::locale::message("status.cancelled"));
        }
        let (cold, slot) = self.load_voice(id, format, &voice, &voice_path)?;
        let loaded = self.loaded.as_mut().unwrap();
        if loaded.duration_delta != settings.duration_delta {
            loaded.synth.set_conditions(HashMap::from([(
                "duration_delta".to_owned(),
                settings.duration_delta.to_string(),
            )]))?;
            loaded.duration_delta = settings.duration_delta;
        }
        let setup_ms = began.elapsed().as_secs_f64() * 1000.0;
        let overrides = SpeechOptions::default()
            .voice(VOICE_SLOTS[slot])
            .temperature(settings.temperature)
            .seed(settings.seed)
            .cfg_coef(settings.guidance)
            .max_tokens_per_chunk(settings.max_tokens);
        let mut offset = 0;
        let mut first_ms = None;
        {
            if stopped() {
                bail!(crate::locale::message("status.cancelled"));
            }
            let stream = loaded.synth.stream_with(&prepared, &overrides)?;
            for chunk in stream {
                if stopped() {
                    bail!(crate::locale::message("status.cancelled"));
                }
                let mut chunk = chunk?;
                if chunk.is_empty() {
                    continue;
                }
                ensure!(
                    offset + chunk.len() <= MAX_SAMPLES,
                    crate::locale::message("error.audio_limit")
                );
                ensure!(
                    chunk.iter().all(|sample| sample.is_finite()),
                    crate::locale::message("error.nonfinite")
                );
                if first_ms.is_none() {
                    first_ms = Some(began.elapsed().as_secs_f64() * 1000.0);
                }
                for sample in &mut chunk {
                    *sample = (*sample * settings.gain).clamp(-1.0, 1.0);
                }
                samples(offset, &chunk)?;
                offset += chunk.len();
            }
        }
        if stopped() {
            bail!(crate::locale::message("status.cancelled"));
        }
        ensure!(offset > 0, crate::locale::message("error.empty_audio"));
        Ok(
            json!({"model":id,"voice":voice,"language":lang.as_str(),"samples":offset,
            "cold":cold,"setup_ms":setup_ms,"first_ms":first_ms,"total_ms":began.elapsed().as_secs_f64()*1000.0}),
        )
    }
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() == 1 && args[0] == "--version" {
        println!("xsg-engine {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    // Inference competes with the simulator for CPU time.
    rayon::ThreadPoolBuilder::new()
        .num_threads(2)
        .build_global()?;
    if args.is_empty() {
        transport::serve()
    } else if args.len() == 3 && args[0] == "prepare" {
        let lang = args[1]
            .to_str()
            .context("Expected language")?
            .parse::<Lang>()
            .map_err(anyhow::Error::msg)?;
        println!(
            "{}",
            prepare(
                args[2].to_str().context("Expected UTF-8")?,
                lang,
                RadioProfile::default(),
                &[],
                &[]
            )?
        );
        Ok(())
    } else if args.len() == 4 && args[0] == "check" {
        let mut engine = Engine::open(PathBuf::from(&args[1]))?;
        let request: transport::Request = serde_json::from_reader(fs::File::open(&args[2])?)?;
        let mut wav = hound::WavWriter::create(
            Path::new(&args[3]),
            hound::WavSpec {
                channels: 1,
                sample_rate: RATE,
                bits_per_sample: 32,
                sample_format: hound::SampleFormat::Float,
            },
        )?;
        let stats = engine.render(
            &request.text,
            &request.options,
            || false,
            |_, chunk| {
                for &sample in chunk {
                    wav.write_sample(sample)?;
                }
                Ok(())
            },
            |text| {
                eprintln!("{text}");
                Ok(())
            },
        )?;
        wav.finalize()?;
        println!("{stats}");
        Ok(())
    } else {
        bail!("Usage: xsg-engine [prepare LANGUAGE TEXT | check ROOT REQUEST.json OUTPUT.wav]")
    }
}
