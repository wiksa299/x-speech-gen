"""Fetch or verify the pinned Phonon release assets without installing a client."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from hashlib import file_digest
import json
from pathlib import Path
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


MANIFEST = Path(__file__).with_name("assets.json")


def matches(path, row):
    if not path.is_file() or path.stat().st_size != row["bytes"]:
        return False
    with path.open("rb") as handle:
        return file_digest(handle, "sha256").hexdigest() == row["sha256"]


def process(root, row, fetch, token):
    path = root / row["path"]
    if matches(path, row):
        return
    if not fetch:
        raise ValueError(f"{row['path']}: missing or SHA-256 mismatch")
    request = Request(row["url"], headers={"User-Agent": "X-Speech-Gen-Packaging/1"})
    if row.get("gated") and token:
        request.add_unredirected_header("Authorization", f"Bearer {token}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    try:
        with urlopen(request, timeout=45) as source, temporary.open("wb") as output:
            received = 0
            while block := source.read(1024 * 1024):
                received += len(block)
                if received > row["bytes"]:
                    raise ValueError(f"{row['path']}: response too large")
                output.write(block)
        if not matches(temporary, row):
            raise ValueError(f"{row['path']}: downloaded file fails verification")
        temporary.replace(path)
    except HTTPError as error:
        raise ValueError(f"{row['path']}: HTTP {error.code}") from None
    except URLError as error:
        raise ValueError(f"{row['path']}: {error.reason}") from None
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--model", choices=["phonon-31m-en", "phonon-40m-en", "phonon-40m-multilingual"],
                        help="Process only the files for the selected model")
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--token-file", type=Path)
    args = parser.parse_args()
    token = args.token_file.read_text(encoding="utf-8").strip() if args.token_file else ""
    if token and not re.fullmatch(r"hf_[A-Za-z0-9]+", token):
        parser.error("Invalid Hugging Face token file")
    metadata = json.loads(MANIFEST.read_text(encoding="utf-8"))
    files = metadata["files"]
    if args.model:
        files = [row for row in files if row["path"].startswith(f"models/{args.model}/")]
    failures = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        jobs = [pool.submit(process, args.root, row, args.fetch, token) for row in files]
        for job in jobs:
            try:
                job.result()
            except (ValueError, OSError) as error:
                failures.append(str(error))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"Verified {len(jobs)} Phonon and catalog files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
