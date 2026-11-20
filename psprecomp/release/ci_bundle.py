#!/usr/bin/env python3
"""Keep the toolchain bundle on a self-hosted CI runner between runs (GitHub-hosted runners use actions/cache instead).

    ci_bundle.py restore --platform P --key K --work work   copy a kept bundle into work/ and write hit=true|false to $GITHUB_OUTPUT
    ci_bundle.py save    --platform P --key K --work work   keep work/'s bundle (no-op when one is already kept for this key)

K is the hash of psprecomp/toolchain/** (the same key the actions/cache steps use). Kept bundles live in $DISSIDIA_CI_CACHE
(default ~/dissidia/ci-cache)/bundle-P-K/, holding bundle-P, bundle-P.host (when the platform has one) and cache/. A save is
copied to a temporary folder and renamed into place, so two runners saving the same bundle at once cannot leave a half copy.
The two newest keys per platform are kept; older ones are deleted.
"""
import argparse
import glob
import os
import shutil
import sys
import tempfile


def parts(plat):
    return ["bundle-" + plat, "bundle-%s.host" % plat, "cache"]


def output(name, value):
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a") as f:
            f.write("%s=%s\n" % (name, value))
    print("%s=%s" % (name, value))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["restore", "save"])
    ap.add_argument("--platform", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    if not a.key:
        sys.exit("empty --key")
    root = os.environ.get("DISSIDIA_CI_CACHE") or os.path.expanduser("~/dissidia/ci-cache")
    os.makedirs(root, exist_ok=True)
    kept = os.path.join(root, "bundle-%s-%s" % (a.platform, a.key))
    if a.action == "restore":
        if not os.path.isdir(os.path.join(kept, "bundle-" + a.platform)):
            output("hit", "false")
            return
        os.makedirs(a.work, exist_ok=True)
        for p in parts(a.platform):
            src, dst = os.path.join(kept, p), os.path.join(a.work, p)
            if os.path.isdir(src):
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                shutil.copytree(src, dst, symlinks=True)
        output("hit", "true")
        return
    # save
    if os.path.isdir(kept):
        print("already kept: " + kept)
        return
    if not os.path.isdir(os.path.join(a.work, "bundle-" + a.platform)):
        sys.exit("no %s/bundle-%s to keep" % (a.work, a.platform))
    tmp = tempfile.mkdtemp(prefix=".tmp-", dir=root)
    try:
        for p in parts(a.platform):
            src = os.path.join(a.work, p)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(tmp, p), symlinks=True)
        try:
            os.rename(tmp, kept)
            print("kept: " + kept)
        except OSError:
            print("kept by another run meanwhile: " + kept)
    finally:
        if os.path.isdir(tmp):
            shutil.rmtree(tmp, ignore_errors=True)
    # prune: the two newest keys of this platform stay
    old = sorted(glob.glob(os.path.join(root, "bundle-%s-*" % a.platform)), key=os.path.getmtime, reverse=True)[2:]
    for d in old:
        shutil.rmtree(d, ignore_errors=True)
        print("pruned: " + d)


if __name__ == "__main__":
    main()
