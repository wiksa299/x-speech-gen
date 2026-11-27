"""Synthesize representative local utterances with each model and language."""

import json
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory


def main():
    engine, root = sys.argv[1:3]
    models = {
        "phonon-31m-en": ["en"],
        "phonon-40m-en": ["en"],
        "phonon-40m-multilingual": ["en", "fr", "es", "de", "pt"],
    }
    utterances = {
        "en": "Runway 09L, heading 270.",
        "fr": "Piste 09G, cap 270.",
        "es": "Pista 09L, rumbo 270.",
        "de": "Willkommen an Bord.",
        "pt": "Bem-vindo a bordo.",
    }
    voices = {
        item["language"]
        for line in (Path(root) / "voices.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
        if (Path(root) / (item := json.loads(line))["filename"]).is_file()
    }
    if "--require-all-voices" in sys.argv:
        assert voices == set(utterances), voices
    with TemporaryDirectory() as temp:
        request, output = (Path(temp) / name for name in ("request.json", "output.wav"))
        for model, languages in models.items():
            for language in languages:
                if language != "en" and language not in voices:
                    print(f"{model} {language}: skipped, voice not installed", flush=True)
                    continue
                request.write_text(json.dumps({
                    "text": utterances[language],
                    "options": {"model": model, "language": language, "seed": 42},
                }), encoding="utf-8")
                run = subprocess.run([engine, "check", root, str(request), str(output)],
                                     check=True, capture_output=True, text=True, encoding="utf-8", timeout=60)
                summary = json.loads(run.stdout)
                header = output.read_bytes()[:60]
                assert header[:4] == b"RIFF" and header[8:16] == b"WAVEfmt "
                encoding, channels, rate = struct.unpack_from("<HHI", header, 20)
                assert channels == 1 and rate == 24000
                assert encoding == 3 or (encoding == 0xFFFE and struct.unpack_from("<I", header, 44)[0] == 3)
                assert summary["samples"] > 1920 and summary["language"] == language
                print(f"{model} {language}: {summary['first_ms']:.0f} ms first PCM, "
                      f"{summary['samples']} samples", flush=True)


if __name__ == "__main__":
    main()
