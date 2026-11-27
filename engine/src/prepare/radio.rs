use super::cardinal::cardinal;
use ptts::preprocess::Lang;
use serde::Deserialize;

#[derive(Clone, Copy, Default, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum RadioProfile {
    #[default]
    Sera,
    Faa,
}

#[derive(Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    Runway,
    Heading,
    FlightLevel,
    Altitude,
    Speed,
    Wind,
    Frequency,
    Qnh,
    Squawk,
    Registration,
    #[serde(alias = "callsign")]
    CallSign,
    Number,
    Digits,
    Decimal,
    Icao,
    Visibility,
    Rvr,
    CloudHeight,
    Clock,
    Time,
    RadioNumber,
    Spell,
}

const ALPHABET: [&str; 26] = [
    "Alfa", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel", "India", "Juliett",
    "Kilo", "Lima", "Mike", "November", "Oscar", "Papa", "Quebec", "Romeo", "Sierra", "Tango",
    "Uniform", "Victor", "Whiskey", "X-ray", "Yankee", "Zulu",
];

fn digits(value: &str, lang: Lang) -> String {
    let words = match lang {
        Lang::Fr => [
            "zéro", "unité", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf",
        ],
        Lang::Es => [
            "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve",
        ],
        _ => [
            "zero", "one", "two", "tree", "four", "fife", "six", "seven", "eight", "niner",
        ],
    };
    value
        .bytes()
        .map(|digit| words[usize::from(digit - b'0')])
        .collect::<Vec<_>>()
        .join(" ")
}

fn decimal_digits(value: &str) -> bool {
    !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit())
}

fn label(lang: Lang, choices: [&'static str; 3]) -> &'static str {
    match lang {
        Lang::Fr => choices[1],
        Lang::Es => choices[2],
        _ => choices[0],
    }
}

fn hundreds(value: u32, lang: Lang) -> String {
    match lang {
        Lang::Fr => [
            "",
            "cent",
            "deux cents",
            "trois cents",
            "quatre cents",
            "cinq cents",
            "six cents",
            "sept cents",
            "huit cents",
            "neuf cents",
        ][value as usize]
            .into(),
        Lang::Es => [
            "",
            "cien",
            "doscientos",
            "trescientos",
            "cuatrocientos",
            "quinientos",
            "seiscientos",
            "setecientos",
            "ochocientos",
            "novecientos",
        ][value as usize]
            .into(),
        _ => format!("{} hundred", digits(&value.to_string(), lang)),
    }
}

fn round_number(value: &str, lang: Lang, unit: u32) -> String {
    let number_value = value.parse::<u32>().unwrap_or(0);
    if number_value < unit || !number_value.is_multiple_of(unit) {
        return digits(value, lang);
    }
    let thousands = number_value / 1000;
    let remaining_hundreds = number_value % 1000 / 100;
    let mut parts = Vec::new();
    if thousands != 0 {
        let thousand = label(lang, ["thousand", "mille", "mil"]);
        if thousands == 1 && lang != Lang::En {
            parts.push(thousand.to_owned());
        } else {
            parts.push(format!(
                "{} {thousand}",
                digits(&thousands.to_string(), lang)
            ));
        }
    }
    if remaining_hundreds != 0 {
        parts.push(hundreds(remaining_hundreds, lang));
    }
    parts.join(" ")
}

fn spell(code: &str, lang: Lang) -> Option<String> {
    let mut words = Vec::new();
    for byte in code.bytes() {
        match byte.to_ascii_uppercase() {
            letter @ b'A'..=b'Z' => words.push(ALPHABET[usize::from(letter - b'A')].to_owned()),
            digit @ b'0'..=b'9' => words.push(digits(std::str::from_utf8(&[digit]).ok()?, lang)),
            b'-' | b' ' => (),
            _ => return None,
        }
    }
    (!words.is_empty()).then(|| words.join(" "))
}

fn spell_letters(code: &str, lang: Lang) -> Option<String> {
    let names = match lang {
        Lang::Fr => [
            "a",
            "bé",
            "cé",
            "dé",
            "euh",
            "effe",
            "gé",
            "ache",
            "i",
            "ji",
            "ka",
            "elle",
            "emme",
            "enne",
            "o",
            "pé",
            "ku",
            "erre",
            "esse",
            "té",
            "u",
            "vé",
            "double vé",
            "iks",
            "i grec",
            "zède",
        ],
        Lang::Es => [
            "a",
            "be",
            "ce",
            "de",
            "e",
            "efe",
            "ge",
            "hache",
            "i",
            "jota",
            "ka",
            "ele",
            "eme",
            "ene",
            "o",
            "pe",
            "cu",
            "erre",
            "ese",
            "te",
            "u",
            "uve",
            "uve doble",
            "equis",
            "ye",
            "zeta",
        ],
        _ => [
            "ay",
            "bee",
            "see",
            "dee",
            "ee",
            "eff",
            "jee",
            "aitch",
            "eye",
            "jay",
            "kay",
            "el",
            "em",
            "en",
            "oh",
            "pee",
            "cue",
            "ar",
            "ess",
            "tee",
            "you",
            "vee",
            "double you",
            "ex",
            "why",
            "zee",
        ],
    };
    let mut words = Vec::new();
    for character in code.chars() {
        match character.to_ascii_uppercase() {
            letter @ 'A'..='Z' => words.push(names[letter as usize - 'A' as usize].to_owned()),
            digit @ '0'..='9' => words.push(cardinal(digit as usize - '0' as usize, lang)),
            'ñ' | 'Ñ' if lang == Lang::Es => words.push("eñe".into()),
            '-' | ' ' => (),
            _ => return None,
        }
    }
    (!words.is_empty()).then(|| words.join(" "))
}

fn signed_number(
    value: &str,
    lang: Lang,
    render: impl FnOnce(&str) -> Option<String>,
) -> Option<String> {
    let (sign, magnitude) = match value.as_bytes().first()? {
        b'-' => (label(lang, ["minus ", "moins ", "menos "]), &value[1..]),
        b'+' => (label(lang, ["plus ", "plus ", "más "]), &value[1..]),
        _ => ("", value),
    };
    Some(format!("{sign}{}", render(magnitude)?))
}

fn wind(value: &str, lang: Lang) -> Option<String> {
    let upper = value.to_ascii_uppercase();
    if matches!(upper.as_str(), "CALM" | "000/0" | "000/00" | "000/000") {
        return Some(label(lang, ["calm", "calme", "calma"]).into());
    }
    let (direction, speed) = upper.split_once('/')?;
    let direction = if direction == "VRB" {
        label(lang, ["variable", "variable", "variable"]).to_owned()
    } else if direction.len() == 3
        && decimal_digits(direction)
        && direction.parse::<u32>().ok()? <= 360
    {
        digits(direction, lang)
    } else {
        return None;
    };
    let speed_value =
        |value: &str| (value.len() <= 3 && decimal_digits(value)).then(|| digits(value, lang));
    let (speed, gust) = speed
        .split_once('G')
        .map_or((speed, None), |(speed, gust)| (speed, Some(gust)));
    let at = label(lang, ["at", "pour", "a"]);
    let mut spoken = format!("{direction} {at} {}", speed_value(speed)?);
    if let Some(gust) = gust {
        let gusting = label(lang, ["gusting", "rafales", "rachas"]);
        spoken.push_str(&format!(" {gusting} {}", speed_value(gust)?));
    }
    Some(spoken)
}

fn call_sign(value: &str, lang: Lang, profile: RadioProfile) -> Option<String> {
    let (operator, flight) = value.rsplit_once(' ').unwrap_or(("", value));
    let split = flight.bytes().take_while(u8::is_ascii_digit).count();
    let (flight_number, suffix) = flight.split_at(split);
    if operator.trim().is_empty() || flight_number.is_empty() {
        return spell(value, lang);
    }
    if !operator.chars().any(char::is_alphabetic)
        || !(1..=4).contains(&flight_number.len())
        || suffix.len() > 2
        || !suffix.bytes().all(|byte| byte.is_ascii_alphabetic())
    {
        return None;
    }
    let group = |part: &str| {
        if part.len() == 1 || part.starts_with('0') {
            digits(part, lang)
        } else {
            cardinal(part.parse().unwrap_or_default(), lang)
        }
    };
    let flight_number = if profile == RadioProfile::Faa && !flight_number.starts_with('0') {
        if flight_number.len() <= 2 {
            group(flight_number)
        } else {
            let (first, last) = flight_number.split_at(flight_number.len() - 2);
            format!("{} {}", group(first), group(last))
        }
    } else {
        digits(flight_number, lang)
    };
    let operator = operator.trim();
    if suffix.is_empty() {
        Some(format!("{operator} {flight_number}"))
    } else {
        Some(format!(
            "{operator} {flight_number} {}",
            spell(suffix, lang)?
        ))
    }
}

pub(super) fn radio(kind: Kind, value: &str, lang: Lang, profile: RadioProfile) -> Option<String> {
    let faa = profile == RadioProfile::Faa;
    let separator = if faa {
        label(lang, ["point", "point", "punto"])
    } else {
        label(lang, ["decimal", "décimale", "decimal"])
    };
    let integer = |max| value.len() <= max && decimal_digits(value);
    match kind {
        Kind::Frequency => {
            let (whole, fraction) = value.split_once(['.', ','])?;
            if whole.len() != 3
                || !decimal_digits(whole)
                || !(118..=136).contains(&whole.parse::<u32>().ok()?)
                || !(1..=3).contains(&fraction.len())
                || !decimal_digits(fraction)
            {
                return None;
            }
            let padded = format!("{fraction:0<3}");
            let fraction = if faa {
                let trimmed = padded[..2].trim_end_matches('0');
                if trimmed.is_empty() { "0" } else { trimmed }
            } else if padded.ends_with("00") {
                &padded[..1]
            } else {
                &padded
            };
            Some(format!(
                "{} {separator} {}",
                digits(whole, lang),
                digits(fraction, lang)
            ))
        }
        Kind::Runway => {
            let width = value.bytes().take_while(u8::is_ascii_digit).count();
            let (number_value, side) = value.split_at(width);
            let runway = number_value.parse::<u32>().ok()?;
            if !(1..=36).contains(&runway) || width > 2 {
                return None;
            }
            let direction = match side.to_ascii_uppercase().as_str() {
                "" => "",
                "L" | "G" => label(lang, [" left", " gauche", " izquierda"]),
                "C" => label(lang, [" center", " centre", " central"]),
                "R" | "D" => label(lang, [" right", " droite", " derecha"]),
                _ => return None,
            };
            let runway = if faa {
                runway.to_string()
            } else {
                format!("{runway:02}")
            };
            Some(format!("{}{direction}", digits(&runway, lang)))
        }
        Kind::Heading if integer(3) && value.parse::<u32>().ok()? <= 360 => {
            if faa && value.parse::<u32>().ok()? == 0 {
                Some(digits("360", lang))
            } else {
                Some(digits(&format!("{value:0>3}"), lang))
            }
        }
        Kind::FlightLevel if value.len() == 3 && integer(3) => Some(if faa {
            digits(value, lang)
        } else {
            round_number(value, lang, 100)
        }),
        Kind::Altitude | Kind::CloudHeight | Kind::Rvr if integer(5) => {
            Some(round_number(value, lang, 100))
        }
        Kind::Speed if integer(3) => Some(if faa {
            digits(value, lang)
        } else {
            round_number(value, lang, 100)
        }),
        Kind::Wind => wind(value, lang),
        Kind::Qnh if faa => {
            let (whole, fraction) = value.split_once(['.', ','])?;
            (whole.len() == 2
                && fraction.len() == 2
                && decimal_digits(whole)
                && decimal_digits(fraction))
            .then(|| digits(&format!("{whole}{fraction}"), lang))
        }
        Kind::Qnh if matches!(value.len(), 3 | 4) && integer(4) => {
            Some(round_number(value, lang, 1000))
        }
        Kind::Squawk
            if value.len() == 4 && value.bytes().all(|byte| (b'0'..=b'7').contains(&byte)) =>
        {
            Some(if faa {
                digits(value, lang)
            } else {
                round_number(value, lang, 1000)
            })
        }
        Kind::Registration if value.len() <= 12 => spell(value, lang),
        Kind::CallSign if value.len() <= 64 => call_sign(value, lang, profile),
        Kind::Icao if value.len() <= 64 => spell(value, lang),
        Kind::Spell if value.chars().count() <= 64 => spell_letters(value, lang),
        Kind::Number => signed_number(value, lang, |n| {
            if n.len() > 9 || !decimal_digits(n) {
                return None;
            }
            Some(cardinal(n.parse().ok()?, lang))
        }),
        Kind::Digits if integer(32) => Some(digits(value, lang)),
        Kind::Decimal => signed_number(value, lang, |n| {
            let (whole, fraction) = n.split_once(['.', ','])?;
            if whole.len() > 16
                || fraction.len() > 16
                || !decimal_digits(whole)
                || !decimal_digits(fraction)
            {
                return None;
            }
            if faa && whole == "0" {
                Some(format!("{separator} {}", digits(fraction, lang)))
            } else {
                Some(format!(
                    "{} {separator} {}",
                    digits(whole, lang),
                    digits(fraction, lang)
                ))
            }
        }),
        Kind::Visibility if integer(5) => Some(if faa {
            digits(value, lang)
        } else {
            round_number(value, lang, 100)
        }),
        Kind::RadioNumber => signed_number(value, lang, |n| {
            (n.len() <= 8 && decimal_digits(n)).then(|| {
                if faa {
                    digits(n, lang)
                } else {
                    round_number(n, lang, 100)
                }
            })
        }),
        Kind::Clock if integer(2) && (1..=12).contains(&value.parse::<u32>().ok()?) => {
            let clock = value.trim_start_matches('0');
            let hour = cardinal(clock.parse().ok()?, lang);
            Some(match lang {
                Lang::Fr if clock == "1" => "une heure".into(),
                Lang::Fr => format!("{hour} heures"),
                Lang::Es if clock == "1" => "la una".into(),
                Lang::Es => format!("las {hour}"),
                _ => format!("{hour} o’clock"),
            })
        }
        Kind::Time => {
            let (hours, minutes) = value
                .split_once(':')
                .map_or((None, value), |(h, m)| (Some(h), m));
            let valid = |time: &str, max: u32| {
                time.len() == 2
                    && decimal_digits(time)
                    && time.parse::<u32>().is_ok_and(|n| n <= max)
            };
            if !valid(minutes, 59) || hours.is_some_and(|h| !valid(h, 23)) {
                return None;
            }
            Some(digits(&format!("{}{minutes}", hours.unwrap_or("")), lang))
        }
        _ => None,
    }
}

pub(super) fn labeled_radio(
    kind: Kind,
    value: &str,
    lang: Lang,
    profile: RadioProfile,
) -> Option<String> {
    let spoken = radio(kind, value, lang, profile)?;
    let names = match kind {
        Kind::Runway => ["runway", "piste", "pista"],
        Kind::Heading => ["heading", "cap", "rumbo"],
        Kind::FlightLevel => ["flight level", "niveau de vol", "nivel de vuelo"],
        Kind::Altitude => ["altitude", "altitude", "altitud"],
        Kind::Speed => ["speed", "vitesse", "velocidad"],
        Kind::Wind => ["wind", "vent", "viento"],
        Kind::Qnh if profile == RadioProfile::Faa => ["altimeter", "altimètre", "altímetro"],
        Kind::Qnh => ["Q N H", "Q N H", "Q N H"],
        Kind::Squawk => ["squawk", "transpondeur", "transpondedor"],
        _ => return Some(spoken),
    };
    Some(format!("{} {spoken}", label(lang, names)))
}
