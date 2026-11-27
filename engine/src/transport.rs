use super::*;
use std::{
    io::{Read, Write},
    sync::{Arc, mpsc},
    thread,
};

const FRAME_LIMIT: usize = 128 * 1024;
const PROTOCOL_VERSION: u32 = 2;
const PROFILE_LIMIT: u64 = 16 * 1024;

#[cfg(windows)]
mod windows_folder;

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum ModelsAction {
    Apply,
    Default,
    Rescan,
    #[cfg(windows)]
    Browse,
}

#[derive(Deserialize)]
pub struct Request {
    pub(super) text: String,
    #[serde(default)]
    pub(super) options: Options,
}

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum Command {
    Init {
        root: PathBuf,
        protocol: u32,
        preferences: Option<PathBuf>,
        active_root: Option<PathBuf>,
        #[serde(default)]
        ui_strings: HashMap<String, String>,
    },
    Speak {
        id: u64,
        #[serde(flatten)]
        request: Request,
    },
    Preview {
        id: u64,
        #[serde(flatten)]
        request: Request,
    },
    Cancel {
        id: u64,
    },
    Profile {
        profile: Value,
    },
    Preload {
        options: Options,
    },
    Locale {
        ui_strings: HashMap<String, String>,
    },
    Models {
        action: ModelsAction,
        root: Option<PathBuf>,
    },
    Shutdown,
}

fn receive(input: &mut impl Read) -> Result<Command> {
    let mut size = [0_u8; 4];
    input.read_exact(&mut size)?;
    let size = u32::from_le_bytes(size) as usize;
    ensure!((2..=FRAME_LIMIT).contains(&size), "Invalid frame size");
    let mut frame = vec![0; size];
    input.read_exact(&mut frame)?;
    ensure!(frame[0] == b'J', "Invalid command frame");
    Ok(serde_json::from_slice(&frame[1..])?)
}

fn write_frame(out: &mut impl Write, kind: u8, bytes: &[u8]) -> Result<()> {
    ensure!(bytes.len() < FRAME_LIMIT, "Oversized response");
    out.write_all(&(bytes.len() as u32 + 1).to_le_bytes())?;
    out.write_all(&[kind])?;
    out.write_all(bytes)?;
    out.flush()?;
    Ok(())
}

fn message(out: &mut impl Write, json: Value) -> Result<()> {
    write_frame(out, b'J', &serde_json::to_vec(&json)?)
}

pub fn serve() -> Result<()> {
    let (send, commands) = mpsc::sync_channel(16);
    let cancelled = Arc::new(AtomicU64::new(0));
    let control = Arc::clone(&cancelled);
    thread::spawn(move || {
        let mut input = std::io::stdin().lock();
        while let Ok(command) = receive(&mut input) {
            match command {
                Command::Cancel { id } => control.store(id, Ordering::Release),
                Command::Shutdown => break,
                _ => {
                    if send.try_send(command).is_err() {
                        break;
                    }
                }
            }
        }
        control.store(u64::MAX, Ordering::Release);
    });
    let mut output = std::io::stdout().lock();
    let Command::Init {
        root,
        protocol: PROTOCOL_VERSION,
        preferences,
        active_root,
        ui_strings,
    } = commands.recv()?
    else {
        bail!("Expected protocol {PROTOCOL_VERSION} initialization");
    };
    locale::set_catalog(ui_strings);
    let mut profile = preferences
        .as_deref()
        .and_then(read_profile)
        .unwrap_or_else(|| json!({}));
    if let Some(path) = active_root {
        profile["model_root"] = json!(path);
    }
    let mut errors = Vec::new();
    let custom = profile["model_root"].as_str().and_then(|path| {
        open_custom_root(PathBuf::from(path))
            .map_err(|e| errors.push(locale::detail(&e)))
            .ok()
    });
    let mut engine = custom.unwrap_or_else(|| {
        Engine::open(root.clone()).unwrap_or_else(|error| {
            errors.push(
                locale::format_message(
                    "error.folder_read",
                    &[
                        ("path", &root.display().to_string()),
                        ("detail", &locale::detail(&error)),
                    ],
                )
                .to_string(),
            );
            Engine {
                root: root.clone(),
                models: Vec::new(),
                voices: Vec::new(),
                loaded: None,
            }
        })
    });
    let mut folder_error = errors.join(". ");
    message(
        &mut output,
        json!({"type":"ready","protocol":PROTOCOL_VERSION,"catalog":engine.capabilities(),
            "profile":profile,"model_root":engine.root,"model_root_error":folder_error}),
    )?;
    while let Ok(command) = commands.recv() {
        match command {
            Command::Preload { options } => {
                let detail = engine
                    .preload(&options)
                    .err()
                    .map(|error| locale::detail(&error));
                message(
                    &mut output,
                    json!({
                        "type":"preloaded", "loaded_model":engine.loaded_model(), "detail":detail
                    }),
                )?;
            }
            Command::Locale { ui_strings } => locale::set_catalog(ui_strings),
            Command::Speak { id, request } => {
                use std::cell::RefCell;
                let out = RefCell::new(&mut output);
                let stopped = || matches!(cancelled.load(Ordering::Acquire), value if value == id || value == u64::MAX);
                let result = engine.render(
                    &request.text,
                    &request.options,
                    stopped,
                    |offset, samples| {
                        for (index, block) in samples.chunks(1920).enumerate() {
                            let mut pcm = Vec::with_capacity(16 + block.len() * 4);
                            pcm.extend_from_slice(&id.to_le_bytes());
                            pcm.extend_from_slice(&((offset + index * 1920) as u64).to_le_bytes());
                            for sample in block {
                                pcm.extend_from_slice(&sample.to_le_bytes());
                            }
                            write_frame(*out.borrow_mut(), b'P', &pcm)?;
                        }
                        Ok(())
                    },
                    |text| {
                        message(
                            *out.borrow_mut(),
                            json!({"type":"prepared","id":id,"text":text}),
                        )
                    },
                );
                let mut terminal = match result {
                    Ok(stats) => json!({"type":"done","id":id,"status":"ok","stats":stats}),
                    Err(_) if stopped() => json!({"type":"done","id":id,"status":"cancelled"}),
                    Err(error) => {
                        json!({"type":"done","id":id,"status":"engine_error","detail":locale::detail(&error)})
                    }
                };
                terminal["loaded_model"] = json!(engine.loaded_model());
                message(&mut output, terminal)?;
            }
            Command::Preview { id, request } => {
                let result = engine
                    .resolve(&request.options)
                    .and_then(|(_, lang, _, _)| {
                        prepare(
                            &request.text,
                            lang,
                            request.options.radio_profile,
                            &request.options.lexicon,
                            &request.options.annotations,
                        )
                    });
                let reply = match result {
                    Ok(text) => json!({"type":"preview","id":id,"text":text}),
                    Err(error) => json!({"type":"preview","id":id,"detail":locale::detail(&error)}),
                };
                message(&mut output, reply)?;
            }
            Command::Profile {
                profile: mut updated,
            } => {
                if let Some(saved) = profile.get("model_root") {
                    updated["model_root"] = saved.clone();
                }
                match persist(preferences.as_deref(), &updated)
                    .with_context(|| locale::message("error.preferences"))
                {
                    Ok(()) => {
                        profile = updated;
                        message(&mut output, json!({"type":"profile","profile":profile}))?;
                    }
                    Err(error) => message(
                        &mut output,
                        json!({"type":"preferences_error","detail":locale::detail(&error)}),
                    )?,
                }
            }
            Command::Models {
                action,
                root: requested,
            } => {
                let result = (|| -> Result<Option<Engine>> {
                    if matches!(action, ModelsAction::Rescan) {
                        return Engine::open(engine.root.clone()).map(Some);
                    }
                    let path = match action {
                        ModelsAction::Default => None,
                        ModelsAction::Apply => Some(
                            requested
                                .with_context(|| crate::locale::message("error.folder_enter"))?,
                        ),
                        ModelsAction::Rescan => unreachable!(),
                        #[cfg(windows)]
                        ModelsAction::Browse => match windows_folder::browse()? {
                            Some(path) => Some(path),
                            None => return Ok(None),
                        },
                    };
                    let candidate = match &path {
                        Some(path) => open_custom_root(path.clone())?,
                        None => Engine::open(root.clone())?,
                    };
                    let mut updated = profile.clone();
                    if let Some(path) = path {
                        updated["model_root"] = json!(path);
                    } else if let Some(object) = updated.as_object_mut() {
                        object.remove("model_root");
                    }
                    persist(preferences.as_deref(), &updated)
                        .with_context(|| locale::message("error.preferences"))?;
                    profile = updated;
                    Ok(Some(candidate))
                })();
                match result {
                    Ok(Some(next)) => {
                        engine = next;
                        folder_error.clear();
                        message(
                            &mut output,
                            json!({"type":"models","catalog":engine.capabilities(),
                            "model_root":engine.root,"model_root_error":folder_error}),
                        )?;
                    }
                    Ok(None) => message(&mut output, json!({"type":"models","cancelled":true}))?,
                    Err(error) => message(
                        &mut output,
                        json!({"type":"models","detail":locale::detail(&error)}),
                    )?,
                }
            }
            _ => bail!("Unexpected command"),
        }
    }
    Ok(())
}

fn open_custom_root(path: PathBuf) -> Result<Engine> {
    ensure!(
        path.is_absolute(),
        crate::locale::message("error.folder_absolute")
    );
    ensure!(
        path.is_dir(),
        locale::format_message(
            "error.folder_missing",
            &[("path", &path.display().to_string())]
        )
    );
    let engine = Engine::open(path)?;
    ensure!(
        engine.models.iter().any(|model| model.available),
        locale::format_message(
            "error.folder_empty",
            &[("path", &engine.root.display().to_string())]
        )
    );
    Ok(engine)
}

fn read_profile(path: &Path) -> Option<Value> {
    if path.metadata().ok()?.len() > PROFILE_LIMIT {
        return None;
    }
    let value: Value = serde_json::from_reader(fs::File::open(path).ok()?).ok()?;
    let mut options = value.clone();
    options.as_object_mut()?.remove("model_root");
    serde_json::from_value::<Options>(options)
        .ok()
        .map(|_| value)
}

fn persist(path: Option<&Path>, profile: &Value) -> Result<()> {
    let Some(path) = path else { return Ok(()) };
    let serialized = serde_json::to_vec_pretty(profile)?;
    ensure!(
        serialized.len() as u64 <= PROFILE_LIMIT,
        crate::locale::message("error.profile_size")
    );
    let temporary = path.with_extension(format!("{}.part", std::process::id()));
    let saved = (|| {
        fs::write(&temporary, serialized)?;
        fs::rename(&temporary, path)?;
        Ok(())
    })();
    if saved.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    saved
}
