"""Check the shipped translations against the English catalog."""
from collections import Counter
import json
from pathlib import Path
import re
import sys


def unique(pairs):
    result = dict(pairs)
    if len(result) != len(pairs):
        raise ValueError("duplicate key")
    return result


def catalog(path):
    strings = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    if not isinstance(strings, dict) or not strings:
        raise ValueError("expected a nonempty catalog")
    fields = {}
    for key, value in strings.items():
        if not isinstance(value, str) or not value.strip() or len(value.encode()) > 1024:
            raise ValueError(f"invalid translation for {key}")
        if "##" in value or any(ord(c) < 32 for c in value):
            raise ValueError(f"invalid control or ImGui ID in {key}")
        pattern = r"\{([a-z][a-z0-9_]*)\}"
        remainder = re.sub(pattern, "", value)
        if "{" in remainder or "}" in remainder:
            raise ValueError(f"invalid placeholder in {key}")
        fields[key] = Counter(re.findall(pattern, value))
    return fields


def main(root):
    english = catalog(root / "en.json")
    for lang in ("fr", "es"):
        if not (root / f"{lang}.json").is_file():
            raise ValueError(f"missing {lang}.json")
    for path in sorted(root.glob("*.json")):
        try:
            fields = catalog(path)
            differences = [k for k in english.keys() | fields.keys() if english.get(k) != fields.get(k)]
            if differences:
                raise ValueError(f"keys or placeholders differ: {', '.join(sorted(differences))}")
        except ValueError as error:
            raise SystemExit(f"{path}: {error}") from error
    print(f"Validated {len(english)} translation keys per catalog")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
