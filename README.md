# X-Speech Gen

X-Speech Gen is a local text-to-speech provider for X-Plane 12.4.4 and later. Other plugins can use it for announcements, copilot replies, and spoken checklists. Gradium Phonon runs on the CPU. Complete packages include the models and voices for offline use.

Version **0.1.0-beta.4** targets testing in X-Plane on Windows x64 (x86-64-v3). It includes the English, French, and Spanish interface catalogs, a configurable models folder, and an Activity tab for client requests. The complete beta package includes the three Q8 Phonon models and their voice catalog. In-simulator validation is pending. The planned platforms for the stable release are Windows and Linux with an x86-64-v3 CPU, and macOS 15 or later on Apple Silicon and Intel.

GitHub Actions produces build artifacts for the plugins and helper. Use the [packaging steps](#package-a-build) to assemble a complete offline ZIP with the models and voices.

## Install and test

1. With X-Plane closed, extract the package and place the `X-Speech-Gen` and `X-Speech-Gen-Diagnostic` folders in `Resources/plugins`.
2. If the plugin package comes with a separate model archive, extract `X-Speech-Gen-Models-Q8.zip` into `Resources/plugins` and merge its `X-Speech-Gen` folder with the installed one. Check that `Resources/plugins/X-Speech-Gen` contains `models/`, `voices/`, and `voices.jsonl` alongside the plugin files.
3. Start X-Plane, open **Plugins → X-Speech Gen → Settings**, and select a model, language, and voice. **Voice preview** plays a short phrase using the current selection. Set the default radio profile to **SERA (Europe)** or **FAA (United States)**. X-Plane's preferences folder stores the settings. The initial model is Phonon 40M English with SERA.
4. Open **Test**, choose whether to use the saved profile for this request, and edit the sample. Select **Preview pronunciation** to inspect the prepared text or **Listen** to hear the result.
5. In **X-Speech Gen Diagnostic**, toggle **Streaming** to switch between a stream and a complete buffer. Open **Activity** in the provider to inspect recent client requests. Each entry shows queue time, the delay to the first audio callback and the total time from acceptance, the audio delivered, and any error.

| Gradium model | Languages | Catalog voices | License |
| --- | --- | ---: | --- |
| [Phonon 31M English](https://huggingface.co/gradium/phonon-31m-en) | English | 37 | Apache 2.0 |
| [Phonon 40M English](https://huggingface.co/gradium/phonon-40m-en) | English | 37 | CC BY-NC 4.0 |
| [Phonon 40M Multilingual](https://huggingface.co/gradium/phonon-40m-multilingual) | English, French, Spanish, German, and Brazilian Portuguese | 102, including 15 French and 25 Spanish voices | CC BY-NC 4.0 |

Complete packages include the shared [Phonon Voices](https://huggingface.co/gradium/phonon-voices) catalog and the three Q8 models. Find voices by language, accent, or ID.

### Interface language

In **Settings → Interface language**, select **Follow X-Plane** (the default), **English**, **Français**, or **Español**. This updates both plugins and saves the choice in `X-Speech-Gen-ui.json` in X-Plane’s preferences folder. With **Follow X-Plane** selected, the plugins use the simulator language when enabled.

The voice preview uses the selected **speech** language. Select the multilingual model and a Spanish voice to hear the Spanish sample while using the French interface. Client requests keep their own speech language and radio profile.

Both plugins read UTF-8 JSON files from `Resources/plugins/X-Speech-Gen/locales/`. To edit a translation, change the values in `fr.json`, `es.json`, or `en.json`. To add another language for **Follow X-Plane**, copy `en.json` to its language code, for example `it.json`. Keep the keys and named placeholders such as `{count}` and `{seconds}`. You can reorder placeholders in the sentence. Disable and re-enable both plugins, or restart X-Plane, to load your changes.

A missing entry uses the English text. If a file is missing or invalid, the plugin records the issue in X-Plane’s `Log.txt` and uses the English fallback. An embedded English catalog is available even when the external English file is missing.

### Model loading and unloading

The **Models** card in Settings shows the current model and whether it is unloaded, loading, or ready. **Load model** defaults to **On first use**. **On plugin activation** also prepares the selected voice in the background and loads the selected model and voice after a change in Settings. Preloading produces no audio.

**Unload model** offers three policies.

| Policy | When the helper releases the model |
| --- | --- |
| **After each use** | When the synthesis queue becomes empty. A model preloaded at activation stays ready for the first synthesis. |
| **On plugin deactivation** | When the provider is disabled or X-Plane exits. |
| **After inactivity** | Once the queue is idle for the selected **1–60 minutes**. The default is **5 minutes**. |

The provider keeps the model catalog available after unloading. The next synthesis starts the helper and loads the requested model.

### Models folder

**Settings → Models folder** shows the active folder. Enter an absolute path and select **Apply**, or use **Browse…** on Windows. Choose the parent of `models/`. The folder must contain at least one supported Phonon pack. Keep `voices.jsonl` and `voices/` alongside `models/` to use the voice catalog and synthesize languages other than English.

If the startup catalog cannot be read, Settings shows the error. Use **Rescan** after repairing or adding files, or choose a different folder. **Use default** restores the plugin folder or the path supplied in `XSG_MODEL_ROOT`. A successful change refreshes the catalog and saves the choice in X-Plane's preferences. Wait until client requests finish before changing folders.

## Integrate a client plugin

The XPLM module exposes a versioned C ABI. A helper process keeps one model loaded at a time, synthesizes audio off the simulator's main thread, and sends PCM to the module over a private channel. The provider handles one request at a time, queues up to eight requests in arrival order, and keeps the two most recently used voices for the active model.

- Include [xspeech_gen.h](include/xspeech_gen.h) and look up `wiksa.x-speech-gen` from an XPLM callback. Zero-initialize the request, then set `version = XSG_ABI_VERSION` (1), `struct_size = sizeof(XsgRequest)`, `operation`, and `status = XSG_UNAVAILABLE` before sending `XSG_MESSAGE_REQUEST`. Gen returns `XSG_BAD_ABI` for an unsupported version or an undersized request when the caller supplies the status field. `GET_STATUS` returns the service state and `GET_CAPABILITIES` returns the JSON catalog. Call `GET_CAPABILITIES` without a buffer first to get the required size, including the terminating NUL.
- `SPEAK` takes UTF-8 text and returns a request ID. The client can select the model, language, and voice ID. The option mask enables temperature, seed, guidance when supported by the model, speaking rate via `duration_delta`, the token limit per segment, and gain in dB. The three bundled models support guidance at 1. Select the multilingual model in the profile or request to synthesize French or Spanish.
- In **streaming mode**, `AUDIO` events deliver ordered PCM chunks as they become available. In **complete mode**, one `AUDIO` event delivers the entire buffer after successful synthesis. Both modes return mono `float32` PCM at 24 kHz, followed by a `DONE` event. The client controls playback and copies any samples it needs after the callback returns.
- `CANCEL` applies to requests from the same client. After it returns `XSG_OK`, the provider discards any undelivered audio for that request and still sends a terminal `DONE`. If cancellation times out and the helper must restart, other queued requests receive `XSG_ENGINE_LOST`. If a stream fails after delivering some audio, its terminal event reports the failure. Each request accepts up to 8 KiB of text and produces up to 60 seconds of PCM.

Set `request.radio_profile` to `XSG_SERA` or `XSG_FAA` to override the user's selection for one request. Zero (`XSG_DEFAULT_RADIO_PROFILE`) inherits the saved selection, which defaults to SERA. `GET_CAPABILITIES` lists the supported profiles under `radio_profiles`. Radio profile, voice, and audio delivery can be selected independently.

### Aviation pronunciation

Use inline tags such as `maintain {speed:150} knots` and `flight level {flight_level:150}` to request radio pronunciation in English, French, and Spanish. Plain text uses standard normalization and the client's lexicon. To annotate existing text, set `annotations` and `annotation_count` on the request. Each `XsgAnnotation` supplies an inclusive start and exclusive end in UTF-8 bytes, plus a kind from `XsgValueKind`. For example, `{0, 3, XSG_HEADING}` marks all three bytes of `150` as a heading.

Inline tags expand to the spoken value. Supply the surrounding phrase and units in the template, for example `Turn heading {heading:150}`. Range annotations for values such as headings and flight levels still include their label. Tags and annotations take priority over lexical rules. Use doubled braces to escape literal braces. Unknown tags and invalid values, such as `{runway:99}`, fail before audio generation. `GET_CAPABILITIES` reports `aviation_tags: 2` for this tag set.

| Tag examples | Input |
| --- | --- |
| `{number:150}`, `{digits:0150}`, `{decimal:0.82}` | Ordinary signed integer (up to nine digits), individual digits, or signed radio decimal |
| `{icao:LFPG}`, `{registration:F-GABC}`, `{callsign:Speedbird 150A}` | Phonetic spelling, registration, or spoken operator followed by a flight number and optional letter suffix |
| `{spell:ILS}` | Ordinary letter and digit names in the voice language, such as “i elle esse” in French. Accepts A–Z, digits, spaces, hyphens, and Ñ in Spanish. |
| `{runway:09L}`, `{heading:150}`, `{flight_level:200}`, `{altitude:3400}`, `{speed:300}` | Runway designator, heading in degrees, flight level, altitude, and indicated airspeed in knots |
| `{wind:230/12G25}`, `{wind:VRB/03}`, `{wind:CALM}` | Direction and speed, with an optional gust after `G`, variable direction, or calm |
| `{frequency:118.050}`, `{qnh:993}`, `{qnh:29.92}`, `{squawk:2400}` | VHF COM channel, altimeter setting in hPa for SERA or inHg for FAA, and four-digit octal code |
| `{visibility:1000}`, `{rvr:600}`, `{cloud_height:2200}`, `{radio_number:-5}` | Whole-number visibility, runway visual range, cloud height, or a general signed radio integer |
| `{clock:10}`, `{time:14:05}`, `{time:05}` | Relative traffic bearing, full UTC time, or UTC minutes when the hour is clear |

With SERA, `{speed:300}` reads “tree hundred” and `{frequency:118.050}` reads “one one eight decimal zero fife zero” in English. With FAA, they read “tree zero zero” and “one one eight point zero fife”. FAA frequencies use at most two digits after the point, so `{frequency:135.275}` ends in “point two seven”. FAA headings accept zero as north and speak it as “tree six zero”. The profile formats the tagged values. The client provides the message wording and chooses the local units and call sign. [EASA SERA.14035](https://www.easa.europa.eu/en/document-library/easy-access-rules/online-publications/easy-access-rules-standardised-european?erules-id=ERULES-1963177438-9832) · [FAA AIM §4-2](https://www.faa.gov/air_traffic/publications/aim_html/chap4_section_2.html)

## Build and test

Install CMake 3.24 or later, a C++17 compiler, Rust 1.95, and Python 3.11 or later. CMake verifies the C++ dependency archives, and Cargo resolves dependencies from `Cargo.lock`.

```sh
cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Release -DXSG_TEST_MODELS="/path/to/Phonon"
cmake --build build/dev --config Release
ctest --test-dir build/dev -C Release --output-on-failure
cargo test --locked
```

The Phonon test directory contains `models/phonon-31m-en`, `models/phonon-40m-en`, `models/phonon-40m-multilingual`, `voices.jsonl`, and `voices/`. Each model directory contains `config.json`, `tokenizer.json`, `default-voice.safetensors`, and the Q8 or Safetensors weights. Set `XSG_MODEL_ROOT` to use this directory as the default models folder with a development build in X-Plane.

The compiled helper accepts `--version`, `prepare LANGUAGE TEXT` using SERA, and `check PHONON_DIR REQUEST.json OUTPUT.wav`. A test request contains `text` and an optional `options` object with fields such as `model`, `language`, `voice`, and `radio_profile` (`"sera"` or `"faa"`).

### Package a build

```sh
cmake --install build/dev --config Release --prefix build/stage
python3 packaging/rust_notices.py build/stage/licenses/rust
python3 packaging/assets.py build/model-assets
python3 packaging/release.py build/stage build/model-assets linux-x86-64 dist
```

The asset check runs offline. To populate the asset directory, run `packaging/assets.py` with `--fetch`. For the two 40M models, supply `--token-file` with a token whose account already has access. The packaging script supports `linux-x86-64`, `windows-x86-64`, `macos-arm64`, and `macos-x86-64`. It uses the version in `Cargo.toml` in the ZIP filename and writes a SHA-256 checksum next to it.

## Licenses

X-Speech Gen is released under the [MIT License](LICENSE). Each model and dependency retains its own license. Packages include the Gradium notices, license texts, and an asset manifest with revisions and SHA-256 checksums. The 40M models are for noncommercial use. Check Gradium's access conditions for those models before distributing a package that includes them.

Codex prompted by wiksa
