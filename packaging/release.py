"""Build a portable archive from the CMake install tree and verified model assets."""

import argparse
from hashlib import file_digest
import json
from pathlib import Path
import sys
import tomllib
from zipfile import ZIP_DEFLATED, ZipFile

from assets import MANIFEST, matches


def release_version():
    with (Path(__file__).resolve().parent.parent / "Cargo.toml").open("rb") as manifest:
        return tomllib.load(manifest)["package"]["version"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("install", type=Path)
    parser.add_argument("models", type=Path)
    parser.add_argument("target", choices=["windows-x86-64", "linux-x86-64", "macos-arm64", "macos-x86-64"])
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    metadata = json.loads(MANIFEST.read_text(encoding="utf-8"))
    module = {"windows": "win", "linux": "lin", "macos": "mac"}[args.target.split("-")[0]]
    required = ["README.md", "LICENSE", "xspeech_gen.h", "include/xspeech_gen.h", "licenses/rust/index.tsv",
                f"X-Speech-Gen/64/{module}.xpl", f"X-Speech-Gen-Diagnostic/64/{module}.xpl",
                "X-Speech-Gen/NotoSans-Regular.ttf",
                "X-Speech-Gen/bin/xsg-engine" + (".exe" if module == "win" else "")]
    catalogs = {"en.json", "fr.json", "es.json"}
    catalogs.update(path.name for path in (args.install / "X-Speech-Gen/locales").glob("*.json"))
    required.extend(f"X-Speech-Gen/locales/{name}" for name in sorted(catalogs))
    missing = [name for name in required if not (args.install / name).is_file()]
    if missing:
        parser.error("Incomplete CMake install: " + ", ".join(missing))
    for row in metadata["files"]:
        if not matches(args.models / row["path"], row):
            parser.error(f"Asset does not match the release manifest: {row['path']}")
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output / f"X-Speech-Gen-{release_version()}-{args.target}.zip"
    temporary = path.with_suffix(".zip.part")
    try:
        with ZipFile(temporary, "w", compression=ZIP_DEFLATED, compresslevel=3) as zip_file:
            binary_paths = [args.install / name for name in required if not name.startswith("licenses/")]
            notices = [file for file in (args.install / "licenses").rglob("*") if file.is_file()]
            for file in sorted(binary_paths + notices):
                if file.is_symlink():
                    parser.error(f"Staged file is a symbolic link: {file.relative_to(args.install)}")
                zip_file.write(file, file.relative_to(args.install).as_posix())
            for row in metadata["files"]:
                zip_file.write(args.models / row["path"], f"X-Speech-Gen/{row['path']}")
            zip_file.write(MANIFEST, "X-Speech-Gen/asset-manifest.json")
            for notice in sorted(Path(__file__).with_name("notices").iterdir()):
                if notice.is_file():
                    zip_file.write(notice, f"licenses/{notice.name}")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    with path.open("rb") as handle:
        digest = file_digest(handle, "sha256").hexdigest()
    path.with_suffix(".zip.sha256").write_text(f"{digest}  {path.name}\n", encoding="ascii")
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
