#!/bin/bash
# Publish the GitHub release for the current version with the packages in
# dist/ attached. Re-running attaches whatever is still missing. Needs:
#   - the tag v<version> already pushed to origin
#   - the packages built: ./scripts/package.sh all
#   - packaging/release-notes/<version>.md
#   - a token with contents read and write on the repository, in
#     $GITHUB_TOKEN or in the file named by $GITHUB_TOKEN_FILE

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/.*AC3SPDIF_VERSION "\([0-9.]*\)".*/\1/p' src/engine/version.h)
REPO=$(git remote get-url origin | sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##')
NOTES=packaging/release-notes/$VERSION.md
TOKEN="${GITHUB_TOKEN:-}"
[ -n "$TOKEN" ] || [ -z "${GITHUB_TOKEN_FILE:-}" ] || TOKEN=$(tr -d '[:space:]' < "$GITHUB_TOKEN_FILE")
[ -n "$TOKEN" ] || { echo "set GITHUB_TOKEN or GITHUB_TOKEN_FILE" >&2; exit 1; }
[ -r "$NOTES" ] || { echo "missing $NOTES" >&2; exit 1; }
git ls-remote --exit-code --tags origin "v$VERSION" >/dev/null ||
  { echo "tag v$VERSION is not on origin: git tag -a v$VERSION -m 'ac3spdif $VERSION' && git push origin v$VERSION" >&2; exit 1; }
ASSETS=(dist/ac3spdif-"$VERSION"-*.rpm dist/ac3spdif_"$VERSION"_*.deb dist/org.ac3spdif.App.flatpak)
for asset in "${ASSETS[@]}"; do
  [ -r "$asset" ] || { echo "missing $asset: ./scripts/package.sh all" >&2; exit 1; }
done

export TOKEN REPO VERSION NOTES
python3 - "${ASSETS[@]}" <<'PY'
import json, os, sys, urllib.request, urllib.error
token, repo, version, notes = (os.environ[k] for k in ("TOKEN", "REPO", "VERSION", "NOTES"))
api = f"https://api.github.com/repos/{repo}"
headers = {"Authorization": "Bearer " + token, "Accept": "application/vnd.github+json",
           "X-GitHub-Api-Version": "2022-11-28"}

def call(url, data=None, ctype="application/json"):
    req = urllib.request.Request(url, data=data, headers={**headers, "Content-Type": ctype},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")

status, release = call(f"{api}/releases/tags/v{version}")
if status == 404:
    status, release = call(f"{api}/releases", json.dumps({
        "tag_name": f"v{version}", "name": f"ac3spdif {version}", "body": open(notes).read()}).encode())
if status not in (200, 201):
    sys.exit(f"cannot create the release: HTTP {status} {release.get('message')}")
print(("created " if status == 201 else "exists ") + release["html_url"])
attached = {a["name"] for a in release.get("assets", [])}
upload = release["upload_url"].split("{")[0]
for path in sys.argv[1:]:
    name = os.path.basename(path)
    if name in attached:
        print(f"  {name}: already attached")
        continue
    st, body = call(f"{upload}?name={name}", open(path, "rb").read(), "application/octet-stream")
    if st != 201:
        sys.exit(f"  {name}: upload failed: HTTP {st} {body.get('message')}")
    print(f"  {name}: uploaded, {body['size']} bytes")
PY
