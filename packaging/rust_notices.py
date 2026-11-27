"""Collect license files from Cargo's locked dependency graph for the release."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    metadata = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--locked", "--format-version", "1"],
        cwd=repository, text=True, encoding="utf-8",
    ))
    args.destination.mkdir(parents=True, exist_ok=True)
    entries = ["name\tversion\tlicense\tsource\tfiles"]
    for package in sorted(metadata["packages"], key=lambda item: (item["name"], item["version"])):
        if package["id"] in metadata["workspace_members"]:
            continue
        root = Path(package["manifest_path"]).parent
        folder = args.destination / (package["name"] + "-" + package["version"])
        candidates = {
            path for path in root.iterdir() if path.is_file()
            and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))
        }
        if package["license_file"]:
            candidates.add(root / package["license_file"])
        licenses = root / "LICENSES"
        if licenses.is_dir():
            candidates.update(path for path in licenses.iterdir() if path.is_file())
        files = []
        for source in sorted(candidates):
            if not source.is_file():
                continue
            folder.mkdir(exist_ok=True)
            shutil.copyfile(source, folder / source.name)
            files.append(source.name)
        entries.append("\t".join([
            package["name"], package["version"], package["license"] or "",
            package["source"] or "", " ".join(files),
        ]))
    (args.destination / "index.tsv").write_text("\n".join(entries) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
