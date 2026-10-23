#!/usr/bin/env python3
# SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
"""fetch-deps.py -- fetch the exact third-party packages the build compiles in.

Everything lands under build/deps/ in this repository, always. Each archive's
digest is checked before anything is extracted from it, and a package already
present is left untouched. Nothing third-party is committed: these are fetched.

    nuklear   pinned immediate-mode UI header (MIT/public domain)
"""

import argparse
import hashlib
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# name -> what to fetch, what it must hash to, which members to extract (None
# for all of them), and the file whose presence means it is installed. A
# member list keeps an archive's unrelated code out of build/deps entirely:
# 7-Zip's tree is mostly LGPL, and the five files taken from it are the ones
# that say "Public domain" in their own headers.
PACKAGES = {
    # Nuklear is a single header, not an archive: `raw` means the downloaded file
    # IS the artifact and there is nothing to extract.
    "nuklear": {
        "url": "https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/"
               "9f7750296f176e506c2b24ff55bc24495e2db750/nuklear.h",
        "sha256": "30a33586aeee98f71db083a6c1546295a4279fd8f214cfab36eec703eb98ebfd",
        "raw": True,
        "probe": "nuklear.h",
    },
}


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name: str, spec: dict, into: pathlib.Path) -> None:
    root = into / name
    if (root / spec["probe"]).is_file():
        print(f"{name}: already present at {root}")
        return
    root.mkdir(parents=True, exist_ok=True)
    archive = root / spec["url"].rsplit("/", 1)[1]
    if not archive.is_file() or digest(archive) != spec["sha256"]:
        with tempfile.NamedTemporaryFile(dir=root, delete=False) as tmp:
            temp = pathlib.Path(tmp.name)
        try:
            print(f"{name}: downloading {spec['url']}")
            with urllib.request.urlopen(spec["url"]) as response, \
                    temp.open("wb") as output:
                shutil.copyfileobj(response, output)
            actual = digest(temp)
            if actual != spec["sha256"]:
                raise SystemExit(f"{name}: digest mismatch: expected "
                                 f"{spec['sha256']}, got {actual}")
            temp.replace(archive)
        finally:
            temp.unlink(missing_ok=True)
    if spec.get("raw"):
        if not (root / spec["probe"]).is_file():
            raise SystemExit(f"{name}: download did not produce {spec['probe']}")
        print(f"{name}: installed at {root}")
        return
    with tarfile.open(archive, "r:*") as bundle:
        resolved_root = root.resolve()
        wanted = spec["members"]
        members = bundle.getmembers() if wanted is None \
            else [bundle.getmember(m) for m in wanted]
        for member in members:
            target = (root / member.name).resolve()
            if target != resolved_root and resolved_root not in target.parents:
                raise SystemExit(f"{name}: refusing archive path outside its "
                                 f"root: {member.name}")
        bundle.extractall(root, members=members)
    if not (root / spec["probe"]).is_file():
        raise SystemExit(f"{name}: archive did not produce {spec['probe']}")
    print(f"{name}: installed at {root}")


def main() -> int:
    parser = argparse.ArgumentParser()
    # --into is the directory that will CONTAIN each package, and it defaults
    # to this repository's build/deps. A dependency must never come from
    # whatever directory happens to sit beside the clone, so there is no probe
    # and no fallback: the default is anchored to REPO_ROOT, not to the
    # caller's cwd.
    parser.add_argument("--into", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "deps",
                        help="directory to place the packages under "
                             "(default: build/deps)")
    parser.add_argument("names", nargs="*",
                        help=f"packages to fetch: {', '.join(PACKAGES)} "
                             f"(default: all)")
    args = parser.parse_args()
    unknown = [n for n in args.names if n not in PACKAGES]
    if unknown:
        parser.error(f"unknown package(s): {', '.join(unknown)}")
    for name in args.names or PACKAGES:
        fetch(name, PACKAGES[name], args.into.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
