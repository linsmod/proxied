#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""winget_publish.py - open/update a microsoft/winget-pkgs PR for proxied.

The manual path for publishing proxied to winget. The GitHub Actions workflow
(.github/workflows/publish.yml) automates subsequent versions with
winget-releaser, but that action refuses to do the very first submission, so
this script covers that case (and serves as a manual fallback).

The whole flow is driven by the GitHub REST API:

    -> resolve release tag -> fetch x64 exe asset + SHA256
    -> render the 3 manifests (version / installer / locale)
    -> smoke-test them locally (winget validate + winget install; the
       publish aborts if either fails) and only then touch anything on GitHub
    -> ensure the winget-pkgs fork exists and is synced to upstream master
    -> create a branch + a single commit containing the manifests
    -> open (or find) the pull request

The manifests use the portable pattern, so winget registers the `proxied`
command (via Commands) into the WinGet Links directory, which is on
the user PATH by default - no manual environment setup needed.

Auth: uses `gh auth token`; override with the GITHUB_TOKEN env var.

Usage:
    python winget_publish.py                     # latest release
    python winget_publish.py v1.4.0              # specific tag
    python winget_publish.py --dry-run v1.4.0    # print manifests only
    python winget_publish.py --no-pr v1.4.0      # push branch, skip the PR
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

REPO = "linsmod/proxied"
UPSTREAM = "microsoft/winget-pkgs"
PACKAGE_ID = "linsmod.proxied"
MANIFEST_VERSION = "1.4.0"
X64_ASSET_RE = re.compile(r"^proxied\.exe$")
WINGET_PKGS_FORK = "winget-pkgs"


def log(msg):
    print("[winget_publish] %s" % msg, flush=True)


def fail(msg):
    log("ERROR: %s" % msg)
    sys.exit(1)


class ApiError(Exception):
    def __init__(self, code, detail):
        self.code = code
        super().__init__("GitHub API error %d: %s" % (code, detail))


def get_token():
    env = os.environ.get("GITHUB_TOKEN")
    if env:
        return env
    try:
        return subprocess.check_output(
            ["gh", "auth", "token"], text=True).strip()
    except Exception as exc:
        fail("could not obtain a GitHub token (gh auth token / GITHUB_TOKEN): %s"
             % exc)


def api(token, method, path, data=None):
    req = urllib.request.Request("https://api.github.com" + path, method=method)
    req.add_header("Authorization", "Bearer %s" % token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "proxied-winget-publish")
    if data is not None:
        req.data = json.dumps(data).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req) as resp:
            raw = resp.read().decode("utf-8")
            return json.loads(raw) if raw.strip() else None
    except urllib.error.HTTPError as exc:
        raise ApiError(exc.code, exc.read().decode("utf-8", "replace"))
    except urllib.error.URLError as exc:
        raise RuntimeError("network error: %s" % exc)


def repo_exists(token, repo):
    try:
        api(token, "GET", "/repos/" + repo)
        return True
    except ApiError as exc:
        if exc.code == 404:
            return False
        raise


def raw_status(owner_repo, ref, path):
    url = "https://raw.githubusercontent.com/%s/%s/%s" % (owner_repo, ref, path)
    try:
        with urllib.request.urlopen(url, timeout=60) as resp:
            return resp.status
    except urllib.error.HTTPError as exc:
        return exc.code
    except (urllib.error.URLError, ConnectionResetError,
            TimeoutError, OSError, http.client.HTTPException):
        return 0


def download_sha256(url):
    hasher = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=300) as resp:
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def resolve_release(token, tag):
    path = "/repos/%s/releases/latest" % REPO
    if tag:
        path = "/repos/%s/releases/tags/%s" % (REPO, tag)
    release = api(token, "GET", path)
    rtag = release["tag_name"]
    version = re.sub(r"^v", "", rtag)
    asset = next((a for a in release.get("assets", [])
                  if X64_ASSET_RE.match(a["name"])), None)
    if not asset:
        fail("no x64 exe asset in release %s (found: %s)"
             % (rtag, [a["name"] for a in release.get("assets", [])]))
    digest = asset.get("digest") or ""
    if digest.startswith("sha256:"):
        sha256 = digest[len("sha256:"):].lower()
    else:
        log("no digest on asset, downloading to compute SHA256 ...")
        sha256 = download_sha256(asset["browser_download_url"])
    return {
        "tag": rtag,
        "version": version,
        "exe_name": asset["name"],
        "url": asset["browser_download_url"],
        "sha256": sha256,
    }


def render_manifests(rel):
    base = "https://github.com/%s" % REPO
    v, tag, url, sha = rel["version"], rel["tag"], rel["url"], rel["sha256"]
    prefix = "manifests/l/linsmod/proxied/%s" % v
    desc = ("proxied is a system tray application that allows you to control "
            "system-level proxy settings directly from the taskbar and syncs them "
            "to development tools. It enables/disables Windows system proxy with "
            "real-time Windows settings sync via registry monitoring, supports "
            "proxy groups for quick switching, and syncs proxy settings to Git "
            "(Windows), Gradle, WSL Git, and environment variables. The application "
            "also supports optional startup with Windows.")

    files = {}
    files["%s/%s.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "DefaultLocale: en-US\n"
        "ManifestType: version\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, MANIFEST_VERSION)

    files["%s/%s.installer.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "Commands:\n"
        "  - proxied\n"
        "Installers:\n"
        "  - Architecture: x64\n"
        "    InstallerType: portable\n"
        "    InstallerUrl: %s\n"
        "    InstallerSha256: %s\n"
        "ManifestType: installer\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, url, sha.upper(), MANIFEST_VERSION)

    files["%s/%s.locale.en-US.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "PackageLocale: en-US\n"
        "Publisher: linsmod\n"
        "PublisherUrl: %s\n"
        "PublisherSupportUrl: %s/issues\n"
        "PackageName: proxied\n"
        "PackageUrl: %s\n"
        "License: MIT\n"
        "LicenseUrl: https://github.com/linsmod/proxied/blob/main/LICENSE\n"
        "ShortDescription: A system tray application to sync Windows proxy settings to development tools\n"
        "Description: >-\n"
        "  %s\n"
        "Moniker: proxied\n"
        "Tags:\n"
        "  - proxy\n"
        "  - windows-proxy\n"
        "  - system-tray\n"
        "  - gradle\n"
        "  - git\n"
        "  - wsl\n"
        "  - development-tools\n"
        "  - network\n"
        "  - sync\n"
        "ManifestType: defaultLocale\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, base, base, base, desc, MANIFEST_VERSION)
    return files


def run_winget(args):
    try:
        return subprocess.run(["winget"] + args,
                              capture_output=True, text=True)
    except OSError as exc:
        fail("could not run winget: %s" % exc)


def validate_with_winget(manifest_dir):
    """Smoke-test the rendered manifests before publishing anything.

    Runs `winget validate` (schema check) followed by `winget install`
    (a real install driven by the manifests, downloading the release
    asset) and finally removes the package again. Any failure aborts the
    publish, so nothing is pushed when the manifests are broken.
    """
    if not sys.platform.startswith("win32"):
        fail("winget validation requires Windows "
             "(run this script on a Windows machine)")

    def check(proc, what):
        if proc.returncode != 0:
            fail("winget %s failed (exit %d):\n%s"
                 % (what, proc.returncode,
                    (proc.stdout + proc.stderr).strip()))

    check(run_winget(["validate", "--manifest", manifest_dir]), "validate")
    log("winget validate OK: manifests are valid")

    check(run_winget([
        "install", "--manifest", manifest_dir,
        "--silent",
        "--accept-package-agreements",
        "--accept-source-agreements",
        "--disable-interactivity",
    ]), "install")
    log("winget install OK: package installed from the manifests")

    # Best-effort cleanup so the release machine is left as it was.
    # winget uninstall --manifest needs a recent winget; on failure only
    # warn, the install test already passed.
    proc = run_winget([
        "uninstall", "--manifest", manifest_dir,
        "--silent",
        "--accept-source-agreements",
        "--disable-interactivity",
    ])
    if proc.returncode != 0:
        log("warning: winget uninstall failed (exit %d), remove manually "
            "with: winget uninstall --id %s" % (proc.returncode, PACKAGE_ID))
    else:
        log("winget uninstall OK: test package removed")


def ensure_fork(token, fork_owner):
    fork = "%s/%s" % (fork_owner, WINGET_PKGS_FORK)
    if repo_exists(token, fork):
        log("fork %s already exists" % fork)
        return fork
    log("creating fork %s (winget-pkgs is large, this may take a while) ..."
        % fork)
    api(token, "POST", "/repos/%s/forks" % UPSTREAM)
    for _ in range(60):
        time.sleep(5)
        if repo_exists(token, fork):
            log("fork ready: %s" % fork)
            return fork
    fail("fork %s did not become available in time" % fork)


def sync_fork_master(token, fork, upstream_master):
    try:
        current = api(token, "GET", "/repos/%s/git/ref/heads/master" % fork)[
            "object"]["sha"]
    except ApiError as exc:
        if exc.code == 404:
            log("no master ref on fork yet; will be created by the first commit")
            return
        raise
    if current != upstream_master:
        log("syncing %s master to upstream (%s)" % (fork, upstream_master[:8]))
        api(token, "PATCH", "/repos/%s/git/refs/heads/master" % fork,
            {"sha": upstream_master, "force": True})
    else:
        log("fork master already up to date")


def create_branch_commit(token, fork, branch, version, master_sha, master_tree,
                         files):
    blobs = {}
    for path, content in files.items():
        payload = {
            "content": base64.b64encode(content.encode("utf-8")).decode(),
            "encoding": "base64",
        }
        blobs[path] = api(token, "POST", "/repos/%s/git/blobs" % fork,
                          payload)["sha"]

    tree = {
        "base_tree": master_tree,
        "tree": [
            {"path": path, "mode": "100644", "type": "blob", "sha": blobs[path]}
            for path in files
        ],
    }
    tree_sha = api(token, "POST", "/repos/%s/git/trees" % fork, tree)["sha"]

    commit = {
        "message": "New version: %s version %s" % (PACKAGE_ID, version),
        "tree": tree_sha,
        "parents": [master_sha],
    }
    commit_sha = api(token, "POST", "/repos/%s/git/commits" % fork, commit)[
        "sha"]

    # Force-update the branch in place when it exists; deleting and recreating
    # it would make GitHub auto-close any open PR on that branch.
    try:
        api(token, "PATCH", "/repos/%s/git/refs/heads/%s" % (fork, branch),
            {"sha": commit_sha, "force": True})
        log("updated branch %s in place" % branch)
    except ApiError as exc:
        if exc.code != 422:
            raise
        api(token, "POST", "/repos/%s/git/refs" % fork,
            {"ref": "refs/heads/%s" % branch, "sha": commit_sha})
        log("created branch %s" % branch)
    return commit_sha


def pr_body(tag):
    return """- [x] Have you signed the Contributor License Agreement?
- [x] Have you checked that there aren't other open pull requests for the same manifest update/change?
- [x] This PR only modifies a single manifest

###### Validation Steps Performed

- Verified the release asset `proxied.exe` at
  https://github.com/%s/releases/tag/%s
- Installer type: portable; the `proxied` command is exposed via Commands so it lands on PATH (WinGet Links dir).
""" % (REPO, tag)


def close_stale_prs(token, version):
    """Close open winget PRs for this package whose version is not `version`.

    winget-pkgs requires a single pending version per package; when a new
    release supersedes an older one that is still queued for moderator
    approval, that older PR must be closed or it blocks validation. The
    version is parsed from the PR title ("New version: linsmod.proxied version
    X.Y.Z").
    """
    import re
    pulls = api(token, "GET",
                "/search/issues?q=repo:%s+is:pr+is:open+in:title+%s"
                % (UPSTREAM, PACKAGE_ID))
    for item in pulls.get("items", []):
        title = item.get("title", "")
        m = re.search(r"version\s+([\d.]+)", title)
        if not m or m.group(1) == version:
            continue
        log("closing superseded PR #%d (%s) ..." % (item["number"], title))
        api(token, "PATCH", "/repos/%s/issues/%d"
            % (UPSTREAM, item["number"]),
            {"state": "closed",
             "state_reason": "not_planned"})
        log("closed PR #%d" % item["number"])


def open_or_find_pr(token, fork_owner, branch, title, body):
    pulls = api(token, "GET",
                "/repos/%s/pulls?state=open&head=%s:%s&per_page=10"
                % (UPSTREAM, fork_owner, branch))
    if pulls:
        return pulls[0]["html_url"]
    pr = api(token, "POST", "/repos/%s/pulls" % UPSTREAM, {
        "title": title,
        "head": "%s:%s" % (fork_owner, branch),
        "base": "master",
        "body": body,
    })
    return pr["html_url"]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", nargs="?", default=None,
                    help="release tag (default: latest release)")
    ap.add_argument("--dry-run", action="store_true",
                    help="render manifests and exit without any API writes")
    ap.add_argument("--no-pr", action="store_true",
                    help="push the branch but do not open a PR")
    ap.add_argument("--fork-owner", default=None,
                    help="account that owns the winget-pkgs fork "
                         "(default: authenticated user)")
    args = ap.parse_args()

    token = get_token()
    me = api(token, "GET", "/user")["login"]
    fork_owner = args.fork_owner or me

    rel = resolve_release(token, args.tag)
    version, tag = rel["version"], rel["tag"]
    branch = "proxied-%s" % version
    log("release %s -> version %s" % (tag, version))
    log("asset %s sha256=%s" % (rel["exe_name"], rel["sha256"]))

    files = render_manifests(rel)
    if args.dry_run:
        for path, content in files.items():
            print("===== %s =====" % path)
            print(content, end="")
        return 0

    # refuse if this version is already published upstream
    probe = "manifests/l/linsmod/proxied/%s/%s.yaml" % (version, PACKAGE_ID)
    status = raw_status(UPSTREAM, "master", probe)
    if status == 200:
        fail("version %s already exists in %s" % (version, UPSTREAM))
    if status == 0:
        log("warning: could not check upstream for an existing version, "
            "continuing")

    # mandatory local pre-publish smoke test: validate the rendered
    # manifests and install from them; nothing is published unless both pass
    tmpdir = tempfile.mkdtemp(prefix="winget-manifest-")
    try:
        for path, content in files.items():
            with open(os.path.join(tmpdir, os.path.basename(path)), "w",
                      encoding="utf-8", newline="\n") as fh:
                fh.write(content)
        log("rendered manifests written to %s for validation" % tmpdir)
        validate_with_winget(tmpdir)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    fork = ensure_fork(token, fork_owner)

    upstream_master = api(token, "GET",
                          "/repos/%s/git/ref/heads/master" % UPSTREAM)[
        "object"]["sha"]
    upstream_tree = api(token, "GET",
                        "/repos/%s/git/commits/%s" % (UPSTREAM, upstream_master))[
        "tree"]["sha"]
    sync_fork_master(token, fork, upstream_master)

    commit_sha = create_branch_commit(
        token, fork, branch, version, upstream_master, upstream_tree, files)
    log("branch %s pushed (commit %s)" % (branch, commit_sha[:8]))

    # verification: raw file visible on the fork branch
    probe = list(files)[0]
    if raw_status(fork, branch, probe) != 200:
        fail("verification failed: %s not visible on %s" % (probe, fork))
    log("verification OK: %s visible on %s" % (probe, fork))

    if args.no_pr:
        log("branch pushed; PR not opened (--no-pr)")
        return 0

    # winget-pkgs allows one pending version per package: close any older
    # open PR for this package before (re)opening the current one.
    close_stale_prs(token, version)

    url = open_or_find_pr(
        token, fork_owner, branch,
        "New version: %s version %s" % (PACKAGE_ID, version),
        pr_body(tag))
    log("PR: %s" % url)
    return 0


if __name__ == "__main__":
    sys.exit(main())