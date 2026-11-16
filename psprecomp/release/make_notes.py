#!/usr/bin/env python3
"""Release notes: install instructions + what changed since the last published release.

  make_notes.py --tag vX.Y.Z [-o notes.md] [--to REF]   (run inside the git checkout with the tags fetched; GH_TOKEN / gh for PR text)

Where the "what changed" part comes from, first match wins:
  1. release/notes/<tag>.md, used as written (hand-written notes for a release);
  2. the pull requests merged since the last *published* release (a tag whose release build failed has no release, so its changes are
     carried into the next one). Each PR gives the bullets of its "## Release notes" section (written for players), or its title when it
     has none. PRs labelled `no-release-notes`, a section that says "none", and CI / release tooling / docs / test-only PRs (title prefix)
     are left out. Items starting with "Fix" go under Fixes;
  3. commits made straight to main (not in a PR) are listed under "Other changes", filtered the same way.
--to builds the notes for an untagged ref (a preview: `make_notes.py --tag v9.9.9 --to origin/main`).
"""
import argparse
import json
import os
import re
import subprocess
import sys

INSTALL = """## Download ONE file
| Your device | Download this |
|---|---|
| Windows PC | `DissidiaRecomp-Setup-windows-x86_64.exe` |
| Linux PC | `DissidiaRecomp-Setup-linux-x86_64` (`chmod +x` it, then run it; also for the Steam Deck) |
| Android phone / handheld | `DissidiaRecomp-Setup-android.apk` |

Run it and follow the wizard. You need your own copy of the game (ISO or CSO); the wizard writes `setup.log` into the install folder, so attach that to bug reports.

The other files (`DissidiaRecomp-Setup-{v}-*` are the same setups with the version in the name; `.tar.xz`, `.zip`, `manifest.json`, `manifest.json.sig`) are what the in-app updater downloads. You do not need them, unless you would rather unpack the Windows / Linux release by hand.
"""

# Titles / commit subjects that are about the project's own tooling, not the game a player runs.
INTERNAL = re.compile(r"^(ci|release|docs?|tests?|harness|build_\w+|tools?|refactor|chore|wip)\b[:(]?", re.I)
HERE = os.path.dirname(os.path.abspath(__file__))


def run(*a):
    r = subprocess.run(list(a), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def git(*a):
    return run("git", *a)


def vkey(t):
    return tuple(int(x) for x in t.lstrip("v").split("."))


def published_tags(repo):
    """tags with a published (non-draft) release, or None when gh cannot tell"""
    out = run("gh", "release", "list", "--exclude-drafts", "--limit", "200", "--json", "tagName", *(["--repo", repo] if repo else []))
    if not out:
        return None
    try:
        return [r["tagName"] for r in json.loads(out)]
    except ValueError:
        return None


def release_section(body):
    """the bullets under a '## Release notes' heading ([] = no section, None = the section says none)"""
    m = re.search(r"^#+\s*release notes?\s*$(.*?)(?=^#+\s|\Z)", body or "", re.I | re.M | re.S)
    if not m:
        return []
    text = re.sub(r"<!--.*?-->", "", m.group(1), flags=re.S).strip()
    if not text or re.fullmatch(r"(?i)[-*\s]*(none|n/?a|-)\.?", text):
        return None
    items = [re.sub(r"^\s*[-*]\s*", "", l).strip() for l in text.splitlines()]
    return [i for i in items if i]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--to", help="build the notes up to this ref instead of the tag (preview)")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    ap.add_argument("-o", "--output", default="-")
    a = ap.parse_args()
    head = a.to or a.tag
    tags = [t for t in git("tag", "--list", "v*").split() if re.fullmatch(r"v\d+(\.\d+)*", t)]
    pub = published_tags(a.repo)
    earlier = sorted((t for t in tags if vkey(t) < vkey(a.tag) and (pub is None or t in pub)), key=vkey)
    prev = earlier[-1] if earlier else None

    out = INSTALL.format(v=a.tag.lstrip("v")) + "\n"
    curated = os.path.join(HERE, "notes", a.tag + ".md")
    if os.path.exists(curated):
        out += open(curated).read().rstrip() + "\n"
    else:
        rng = "%s..%s" % (prev, head) if prev else head
        new, fixes, other = [], [], []

        def add(item):
            item = item.rstrip(".")
            (fixes if re.match(r"(?i)fix", item) else new).append(item)

        for line in git("log", "--first-parent", "--pretty=%H%x09%P%x09%s", rng).splitlines():
            sha, parents, subj = (line.split("\t") + ["", "", ""])[:3]
            m = re.match(r"Merge pull request #(\d+)", subj)
            if m:
                pr = run("gh", "pr", "view", m.group(1), "--json", "title,body,labels", *(["--repo", a.repo] if a.repo else []))
                info = json.loads(pr) if pr else {"title": git("log", "-1", "--pretty=%b", sha).splitlines()[0:1] or [subj], "body": "", "labels": []}
                title = info["title"] if isinstance(info["title"], str) else (info["title"][0] if info["title"] else subj)
                if any(l.get("name") == "no-release-notes" for l in info.get("labels", [])):
                    continue
                sec = release_section(info.get("body", ""))
                if sec is None:
                    continue
                if sec:
                    for item in sec:
                        add(item)
                elif not INTERNAL.match(title):
                    add(title)
            elif len(parents.split()) == 1 and not INTERNAL.match(subj):
                other.append(subj.rstrip("."))
        out += "## What's new" + (" since %s" % prev if prev else "") + "\n"
        if new:
            out += "\n".join("- " + s for s in new) + "\n"
        if fixes:
            out += "\n### Fixes\n" + "\n".join("- " + s for s in fixes) + "\n"
        if other:
            out += "\n### Other changes\n" + "\n".join("- " + s for s in other[:15]) + ("\n- ... and %d more" % (len(other) - 15) if len(other) > 15 else "") + "\n"
        if not (new or fixes or other):
            out += "- Small internal changes only.\n"
    if a.output == "-":
        sys.stdout.write(out)
    else:
        open(a.output, "w").write(out)


if __name__ == "__main__":
    main()
