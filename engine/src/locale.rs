use std::{
    collections::HashMap,
    fmt,
    sync::{LazyLock, RwLock},
};

type Catalog = HashMap<String, String>;
static STRINGS: LazyLock<RwLock<Catalog>> = LazyLock::new(|| RwLock::new(english()));

fn english() -> Catalog {
    serde_json::from_str(include_str!("../../locales/en.json")).expect("validated English catalog")
}

pub fn set_catalog(overrides: Catalog) {
    let mut strings = english();
    strings.extend(overrides);
    *STRINGS.write().expect("translation catalog lock") = strings;
}

#[derive(Debug)]
pub struct Message(String);

impl fmt::Display for Message {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for Message {}

pub fn message(key: &str) -> Message {
    format_message(key, &[])
}

pub fn format_message(key: &str, arguments: &[(&str, &str)]) -> Message {
    let strings = STRINGS.read().expect("translation catalog lock");
    let mut rest = strings.get(key).map_or(key, String::as_str);
    let mut out = String::new();
    while let Some((prefix, tail)) = rest.split_once('{') {
        let Some((name, suffix)) = tail.split_once('}') else {
            break;
        };
        out.push_str(prefix);
        if let Some((_, value)) = arguments.iter().find(|(arg, _)| *arg == name) {
            out.push_str(value);
        } else {
            out.push('{');
            out.push_str(name);
            out.push('}');
        }
        rest = suffix;
    }
    out.push_str(rest);
    Message(out)
}

pub fn detail(error: &anyhow::Error) -> String {
    if error.is::<Message>() {
        format!("{error:#}")
    } else {
        format_message("error.external", &[("detail", &error.to_string())]).to_string()
    }
}
