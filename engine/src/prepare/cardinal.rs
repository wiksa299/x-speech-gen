use ptts::preprocess::Lang;

const EN_SMALL: [&str; 20] = [
    "zero",
    "one",
    "two",
    "three",
    "four",
    "five",
    "six",
    "seven",
    "eight",
    "nine",
    "ten",
    "eleven",
    "twelve",
    "thirteen",
    "fourteen",
    "fifteen",
    "sixteen",
    "seventeen",
    "eighteen",
    "nineteen",
];
const FR_SMALL: [&str; 17] = [
    "zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf", "dix", "onze",
    "douze", "treize", "quatorze", "quinze", "seize",
];
const ES_SMALL: [&str; 30] = [
    "cero",
    "uno",
    "dos",
    "tres",
    "cuatro",
    "cinco",
    "seis",
    "siete",
    "ocho",
    "nueve",
    "diez",
    "once",
    "doce",
    "trece",
    "catorce",
    "quince",
    "dieciséis",
    "diecisiete",
    "dieciocho",
    "diecinueve",
    "veinte",
    "veintiuno",
    "veintidós",
    "veintitrés",
    "veinticuatro",
    "veinticinco",
    "veintiséis",
    "veintisiete",
    "veintiocho",
    "veintinueve",
];

fn tens(value: usize, lang: Lang) -> String {
    match lang {
        Lang::Fr => match value {
            0..=16 => FR_SMALL[value].into(),
            17..=19 => format!("dix-{}", FR_SMALL[value % 10]),
            71 => "soixante et onze".into(),
            70..=79 => format!("soixante-{}", tens(value - 60, lang)),
            80 => "quatre-vingts".into(),
            81..=99 => format!("quatre-vingt-{}", tens(value - 80, lang)),
            _ => {
                let head = [
                    "",
                    "",
                    "vingt",
                    "trente",
                    "quarante",
                    "cinquante",
                    "soixante",
                ][value / 10];
                match value % 10 {
                    0 => head.into(),
                    1 => format!("{head} et un"),
                    unit => format!("{head}-{}", FR_SMALL[unit]),
                }
            }
        },
        Lang::Es if value < ES_SMALL.len() => ES_SMALL[value].into(),
        Lang::Es => {
            let head = [
                "",
                "",
                "",
                "treinta",
                "cuarenta",
                "cincuenta",
                "sesenta",
                "setenta",
                "ochenta",
                "noventa",
            ][value / 10];
            match value % 10 {
                0 => head.into(),
                unit => format!("{head} y {}", ES_SMALL[unit]),
            }
        }
        _ if value < EN_SMALL.len() => EN_SMALL[value].into(),
        _ => {
            let head = [
                "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty",
                "ninety",
            ][value / 10];
            match value % 10 {
                0 => head.into(),
                unit => format!("{head}-{}", EN_SMALL[unit]),
            }
        }
    }
}

fn group(value: usize, lang: Lang, before_thousand: bool) -> String {
    let hundreds = value / 100;
    let rest = value % 100;
    let mut spoken = if hundreds == 0 {
        tens(rest, lang)
    } else {
        let head = match lang {
            Lang::Fr if hundreds == 1 => "cent".into(),
            Lang::Fr if rest == 0 && !before_thousand => format!("{} cents", FR_SMALL[hundreds]),
            Lang::Fr => format!("{} cent", FR_SMALL[hundreds]),
            Lang::Es if value == 100 => "cien".into(),
            Lang::Es => [
                "",
                "ciento",
                "doscientos",
                "trescientos",
                "cuatrocientos",
                "quinientos",
                "seiscientos",
                "setecientos",
                "ochocientos",
                "novecientos",
            ][hundreds]
                .into(),
            _ => format!("{} hundred", EN_SMALL[hundreds]),
        };
        if rest == 0 {
            head
        } else {
            format!("{head} {}", tens(rest, lang))
        }
    };
    if before_thousand && lang == Lang::Fr && rest == 80 {
        spoken.pop();
    }
    spoken
}

fn spanish_before_scale(value: usize) -> String {
    let spoken = group(value, Lang::Es, false);
    if value % 100 == 21 {
        format!(
            "{}veintiún",
            spoken.strip_suffix("veintiuno").unwrap_or(&spoken)
        )
    } else if let Some(prefix) = spoken.strip_suffix("uno") {
        format!("{prefix}un")
    } else {
        spoken
    }
}

pub(super) fn cardinal(value: usize, lang: Lang) -> String {
    let millions = value / 1_000_000;
    let thousands = value / 1000 % 1000;
    let units = value % 1000;
    let mut parts = Vec::new();
    if millions != 0 {
        parts.push(match lang {
            Lang::Fr if millions == 1 => "un million".into(),
            Lang::Fr => format!("{} millions", group(millions, lang, false)),
            Lang::Es if millions == 1 => "un millón".into(),
            Lang::Es => format!("{} millones", spanish_before_scale(millions)),
            _ => format!("{} million", group(millions, lang, false)),
        });
    }
    if thousands != 0 {
        parts.push(match lang {
            Lang::Fr if thousands == 1 => "mille".into(),
            Lang::Fr => format!("{} mille", group(thousands, lang, true)),
            Lang::Es if thousands == 1 => "mil".into(),
            Lang::Es => format!("{} mil", spanish_before_scale(thousands)),
            _ => format!("{} thousand", group(thousands, lang, false)),
        });
    }
    if units != 0 || parts.is_empty() {
        parts.push(group(units, lang, false));
    }
    parts.join(" ")
}
