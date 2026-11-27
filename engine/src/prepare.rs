// The Unicode lexical matching follows Saywell (MIT, copyright 2026 wiksa).
mod cardinal;
mod radio;

use anyhow::{Result, ensure};
use ptts::preprocess::{Lang, Rules, normalize_text};
pub use radio::RadioProfile;
use radio::{Kind, labeled_radio, radio};
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    expression: String,
    spoken: String,
    language: Option<String>,
    #[serde(default)]
    case_sensitive: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Annotation {
    start: usize,
    end: usize,
    kind: Kind,
}

fn number(value: &str, lang: Lang) -> String {
    normalize_text(value, lang, Rules::DEFAULT)
}

fn matching_prefix(text: &str, rule: &Rule, folded: &str) -> Option<usize> {
    if rule.case_sensitive {
        return text
            .starts_with(&rule.expression)
            .then_some(rule.expression.len());
    }
    let mut expected = folded.chars().peekable();
    for (offset, character) in text.char_indices() {
        for lowercase in character.to_lowercase() {
            if expected.next() != Some(lowercase) {
                return None;
            }
        }
        if expected.peek().is_none() {
            return Some(offset + character.len_utf8());
        }
    }
    None
}

struct Replacement {
    start: usize,
    end: usize,
    spoken: String,
}

fn inline_values(text: &str, lang: Lang, profile: RadioProfile) -> Result<Vec<Replacement>> {
    let mut results = Vec::new();
    let mut offset = 0;
    while let Some(relative) = text[offset..].find(['{', '}']) {
        let start = offset + relative;
        let rest = &text[start..];
        let brace = rest.as_bytes()[0];
        offset = start + 1;
        if rest.as_bytes().get(1) == Some(&brace) {
            offset += 1;
            results.push(Replacement {
                start,
                end: offset,
                spoken: rest[..1].into(),
            });
        } else if brace == b'{' {
            let body = &rest[1..];
            let body_end = body.find(['{', '}']).unwrap_or(body.len());
            let Some((name, value)) = body[..body_end].split_once(':') else {
                continue;
            };
            if name.is_empty() || !name.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') {
                continue;
            }
            ensure!(
                body.as_bytes().get(body_end) == Some(&b'}'),
                crate::locale::format_message("error.tag_unclosed", &[("tag", name)])
            );
            let kind = Kind::deserialize(serde::de::value::StrDeserializer::<
                serde::de::value::Error,
            >::new(name))
            .map_err(|_| {
                anyhow::anyhow!(crate::locale::format_message(
                    "error.tag_unknown",
                    &[("tag", name)]
                ))
            })?;
            let spoken = radio(kind, value.trim(), lang, profile).ok_or_else(|| {
                anyhow::anyhow!(crate::locale::format_message(
                    "error.tag_value",
                    &[("tag", name)]
                ))
            })?;
            offset = start + body_end + 2;
            results.push(Replacement {
                start,
                end: offset,
                spoken,
            });
        }
    }
    Ok(results)
}

pub fn prepare(
    text: &str,
    lang: Lang,
    profile: RadioProfile,
    rules: &[Rule],
    annotations: &[Annotation],
) -> Result<String> {
    ensure!(
        !text.trim().is_empty() && text.len() <= 8192 && !text.contains('\0'),
        crate::locale::message("error.text")
    );
    ensure!(
        rules.len() <= 64 && annotations.len() <= 64,
        crate::locale::message("error.rules_count")
    );
    let mut candidates = Vec::new();
    for rule in rules {
        ensure!(
            !rule.expression.trim().is_empty()
                && rule.expression.len() <= 128
                && !rule.spoken.trim().is_empty()
                && rule.spoken.len() <= 256
                && !rule.expression.contains('\0')
                && !rule.spoken.contains('\0'),
            crate::locale::message("error.lexicon")
        );
        if rule
            .language
            .as_deref()
            .is_none_or(|value| value == lang.as_str())
        {
            candidates.push((rule, rule.expression.to_lowercase()));
        }
    }
    candidates.sort_by_key(|(rule, _)| {
        (
            std::cmp::Reverse(rule.expression.len()),
            rule.language.is_none(),
        )
    });
    let aviation_language = matches!(lang, Lang::En | Lang::Fr | Lang::Es);
    let mut explicit = if aviation_language {
        inline_values(text, lang, profile)?
    } else {
        Vec::new()
    };
    let mut previous = 0;
    for annotation in annotations {
        ensure!(
            annotation.start >= previous && annotation.start < annotation.end,
            crate::locale::message("error.range")
        );
        let value = text
            .get(annotation.start..annotation.end)
            .ok_or_else(|| anyhow::anyhow!(crate::locale::message("error.range")))?;
        previous = annotation.end;
        if aviation_language {
            let spoken = labeled_radio(annotation.kind, value, lang, profile)
                .ok_or_else(|| anyhow::anyhow!(crate::locale::message("error.annotation")))?;
            explicit.push(Replacement {
                start: annotation.start,
                end: annotation.end,
                spoken,
            });
        }
    }
    explicit.sort_by_key(|span| span.start);
    ensure!(
        explicit
            .windows(2)
            .all(|spans| spans[0].end <= spans[1].start),
        crate::locale::message("error.overlap")
    );
    let mut explicit = explicit.into_iter().peekable();
    let normalize = |gap: &str| {
        let trimmed = gap.trim();
        if !trimmed.chars().any(char::is_alphanumeric) {
            return gap.to_owned();
        }
        let start = gap.len() - gap.trim_start().len();
        format!(
            "{}{}{}",
            &gap[..start],
            number(trimmed, lang),
            &gap[start + trimmed.len()..]
        )
    };
    let mut output = String::new();
    let mut cursor = 0;
    let mut gap = 0;
    while cursor < text.len() {
        let rest = &text[cursor..];
        let boundary = text[..cursor]
            .chars()
            .next_back()
            .is_none_or(|c| !c.is_alphanumeric());
        let stop = explicit.peek().map_or(text.len(), |span| span.start);
        let replacement = if stop == cursor {
            explicit
                .next()
                .map(|span| (span.end - span.start, span.spoken))
        } else if boundary {
            let available = &text[cursor..stop];
            candidates.iter().find_map(|(rule, folded)| {
                let size = matching_prefix(available, rule, folded)?;
                rest[size..]
                    .chars()
                    .next()
                    .is_none_or(|c| !c.is_alphanumeric())
                    .then(|| (size, rule.spoken.clone()))
            })
        } else {
            None
        };
        if let Some((size, spoken)) = replacement {
            output.push_str(&normalize(&text[gap..cursor]));
            output.push_str(&spoken);
            cursor += size;
            gap = cursor;
        } else {
            cursor += rest.chars().next().unwrap().len_utf8();
        }
        ensure!(
            output.len() <= 32768,
            crate::locale::message("error.text_large")
        );
    }
    output.push_str(&normalize(&text[gap..]));
    ensure!(
        output.len() <= 32768 && !output.trim().is_empty(),
        crate::locale::message("error.prepared")
    );
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_inline_values() {
        for (tag, expected) in [
            ("{number:150}", "one hundred fifty"),
            ("{digits:0150}", "zero one fife zero"),
            ("{decimal:0.82}", "zero decimal eight two"),
            ("{icao:LFPG}", "Lima Foxtrot Papa Golf"),
            ("{spell:ILS}", "eye el ess"),
            ("{registration:F-GABC}", "Foxtrot Golf Alfa Bravo Charlie"),
            ("{callsign:Speedbird 150A}", "Speedbird one fife zero Alfa"),
            ("{runway:09L}", "zero niner left"),
            ("heading {heading:150}", "heading one fife zero"),
            ("{flight_level:200}", "two hundred"),
            ("{altitude:3400}", "tree thousand four hundred"),
            ("{speed:300}", "tree hundred"),
            (
                "{wind:230/12G25}",
                "two tree zero at one two gusting two fife",
            ),
            ("{wind:VRB/03}", "variable at zero tree"),
            ("{wind:CALM}", "calm"),
            ("{frequency:118.000}", "one one eight decimal zero"),
            ("{qnh:993}", "niner niner tree"),
            ("{squawk:2400}", "two four zero zero"),
            ("{visibility:1000}", "one thousand"),
            ("{rvr:600}", "six hundred"),
            ("{cloud_height:2200}", "two thousand two hundred"),
            ("{clock:10}", "ten o’clock"),
            ("{time:14:05}", "one four zero fife"),
            ("{time:05}", "zero fife"),
            ("{radio_number:-5}", "minus fife"),
            ("{radio_number:1200}", "one thousand two hundred"),
            ("maintain {speed:150} knots", "maintain one fife zero knots"),
            ("{callsign:Speedbird 150}", "Speedbird one fife zero"),
        ] {
            assert_eq!(
                prepare(tag, Lang::En, RadioProfile::Sera, &[], &[]).unwrap(),
                expected,
                "{tag}"
            );
        }
    }

    #[test]
    fn faa_and_multilingual_values() {
        for (input, expected) in [
            ("{runway:09L}", "niner left"),
            ("{frequency:135.275}", "one tree fife point two seven"),
            ("{heading:000}", "tree six zero"),
            (
                "flight level {flight_level:200}",
                "flight level two zero zero",
            ),
            ("{decimal:0.82}", "point eight two"),
            ("{speed:300}", "tree zero zero"),
            ("{squawk:2000}", "two zero zero zero"),
            ("{qnh:29.92}", "two niner niner two"),
            ("{frequency:118.050}", "one one eight point zero fife"),
            ("{callsign:United 25}", "United twenty-five"),
            (
                "{callsign:Midwest Commuter 711}",
                "Midwest Commuter seven eleven",
            ),
        ] {
            assert_eq!(
                prepare(input, Lang::En, RadioProfile::Faa, &[], &[]).unwrap(),
                expected,
                "{input}"
            );
        }
        for (input, lang, expected) in [
            ("{number:150}", Lang::Fr, "cent cinquante"),
            ("{spell:ILS}", Lang::Fr, "i elle esse"),
            ("{spell:Ñ-5}", Lang::Es, "eñe cinco"),
            ("{number:150}", Lang::Es, "ciento cincuenta"),
            (
                "{number:280071}",
                Lang::Fr,
                "deux cent quatre-vingt mille soixante et onze",
            ),
            ("{number:21000}", Lang::Es, "veintiún mil"),
            ("{heading:150}", Lang::Fr, "unité cinq zéro"),
            ("{decimal:0.82}", Lang::Es, "cero decimal ocho dos"),
        ] {
            assert_eq!(
                prepare(input, lang, RadioProfile::Sera, &[], &[]).unwrap(),
                expected,
                "{input}"
            );
        }
    }

    #[test]
    fn tag_validation_and_explicit_precedence() {
        let prepare_tag = |input| prepare(input, Lang::En, RadioProfile::Sera, &[], &[]);
        for input in [
            "{runway:99}",
            "{time:24:00}",
            "{digits:1x}",
            "{frequency:+18.0}",
            "{unknown:1}",
            "{spell:*}",
            "{heading:150",
            "{wind:230/12G}",
        ] {
            assert!(prepare_tag(input).is_err(), "{input}");
        }
        assert_eq!(prepare_tag("{{heading:150}}").unwrap(), "{heading:150}");
        let rules = serde_json::from_str::<Vec<Rule>>(
            r#"[{"expression":"heading {heading:150}","spoken":"wrong"}]"#,
        )
        .unwrap();
        assert_eq!(
            prepare(
                "heading {heading:150}",
                Lang::En,
                RadioProfile::Sera,
                &rules,
                &[]
            )
            .unwrap(),
            "heading one fife zero"
        );
    }

    #[test]
    fn cockpit_and_sera() {
        let result = prepare(
            "flight level {flight_level:100}, heading {heading:270}; runway {runway:09L}, {frequency:118.000}",
            Lang::En,
            RadioProfile::default(),
            &[],
            &[],
        )
        .unwrap();
        assert_eq!(
            result,
            "flight level one hundred, heading two seven zero, runway zero niner left, one one eight decimal zero"
        );
        assert_eq!(
            prepare(
                "piste {runway:09D}",
                Lang::Fr,
                RadioProfile::default(),
                &[],
                &[]
            )
            .unwrap(),
            "piste zéro neuf droite"
        );
        assert_eq!(
            prepare(
                "pista {runway:09L}",
                Lang::Es,
                RadioProfile::default(),
                &[],
                &[]
            )
            .unwrap(),
            "pista cero nueve izquierda"
        );
    }

    #[test]
    fn french_and_spanish_tags() {
        let fr = prepare(
            "{registration:F-GABC}, {frequency:118.025} et Q N H {qnh:1000}",
            Lang::Fr,
            RadioProfile::default(),
            &[],
            &[],
        )
        .unwrap();
        assert!(fr.contains("Foxtrot Golf Alfa Bravo Charlie"));
        assert!(fr.contains("unité unité huit décimale zéro deux cinq"));
        assert!(fr.ends_with("Q N H mille"));
        let es = prepare(
            "rumbo {heading:270}, nivel de vuelo {flight_level:100}, viento {wind:230/12}",
            Lang::Es,
            RadioProfile::default(),
            &[],
            &[],
        )
        .unwrap();
        assert_eq!(
            es,
            "rumbo dos siete cero, nivel de vuelo cien, viento dos tres cero a uno dos"
        );
    }

    #[test]
    fn untagged_text_uses_standard_normalization() {
        for (lang, text) in [
            (
                Lang::En,
                "heading 150, maintain 150 knots, FL100, runway 09L",
            ),
            (Lang::En, "ratio 120.500, contact LFPG, F-GABC"),
            (Lang::Fr, "Cap 150, maintenez 150 nœuds, QNH 993"),
            (Lang::Es, "Rumbo 050, pista 09L, viento 230/12"),
        ] {
            for profile in [RadioProfile::Sera, RadioProfile::Faa] {
                assert_eq!(
                    prepare(text, lang, profile, &[], &[]).unwrap(),
                    normalize_text(text, lang, Rules::DEFAULT),
                    "{text}"
                );
            }
        }
    }

    #[test]
    fn lexical_replacements_do_not_cascade_and_annotations_apply() {
        let rules = serde_json::from_str::<Vec<Rule>>(
            r#"[{"expression":"hdg270","spoken":"RESET"},{"expression":"RESET","spoken":"wrong"}]"#,
        )
        .unwrap();
        assert_eq!(
            prepare("HDG270", Lang::En, RadioProfile::default(), &rules, &[]).unwrap(),
            "RESET"
        );
        let hints = [Annotation {
            start: 0,
            end: 3,
            kind: Kind::Heading,
        }];
        assert_eq!(
            prepare("270", Lang::Fr, RadioProfile::default(), &[], &hints).unwrap(),
            "cap deux sept zéro"
        );
    }
}
