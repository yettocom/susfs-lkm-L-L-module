# -*- coding: utf-8 -*-
# Push the r18.5 state of D:/root/susfs4ksu-lkm-work onto yettocom/susfs-lkm-L-L-module main
# via the GitHub REST API (git push is unusable on this host), then create release
# v2.3.0-r18.5, upload the 3 ko + module zip, and retire v2.3.0-r18.4 so the repo has
# exactly one current version. Safe to re-run: blobs are content-addressed, the ref update
# is a fast-forward check (force=false), existing assets are skipped by name.
import base64, hashlib, json, os, subprocess, urllib.request, urllib.error

REPO = 'yettocom/susfs-lkm-L-L-module'
WORK = r'D:\root\susfs4ksu-lkm-work'
DIST = os.path.join(WORK, 'dist')
TAG = 'v2.3.0-r18.5'
OLD_TAG = 'v2.3.0-r18.4'
ASSETS = [
    ('android13-5.15_susfs_samsung_lkm.ko', '088ed7611b24c5e7606a169ff82627e26b43471d38c6fb8ffee3f16bf5adbcc4'),
    ('android14-6.1_susfs_samsung_lkm.ko', 'ad59beb6803aa90b39dde86d3d18089665fe45ac1858b6808de5c8c24daaf1c1'),
    ('android15-6.6_susfs_samsung_lkm.ko', 'bed18808c54c50ff7c4196620419873e17d50a60bff102c0b8090a7b85d36fed'),
    ('susfs-lkm-module-v2.3.0-r18.5-L-L-arm64.zip', '0a0c2b7cd928ccc9f97b6b23d5fdde8ec1ab1ade66ff2df54c54d6354492e98a'),
]
# Published source trees. Any tracked file here that the remote lacks is added.
PUB_TREES = ('kernel/', 'module/', 'tools/', 'bin/')
PUB_EXTRA = ('SAMSUNG.md',)
# Repo-owned docs: this working tree drifts back to upstream's copies (the upstream
# LICENSE is GPL-2.0 while the repo is GPL-3.0), so never replay them over main.
KEEP_REMOTE = ('LICENSE', 'README.md', 'README.en.md')

COMMIT_MSG = '''sync: upstream v2.3.0-r18.5 onto the Samsung delta

- three-way merge of upstream v2.3.0-r18.5 (0de81248b6, 2026-10-28);
  conflicts: Makefile (object union), imports_guard.c, ksu_allow_gate.c,
  sus_mount.c (upstream adaptive scan supersedes our fixed pass count),
  susfs_main.c (upstream base + our rkp_branch layer, /proc/susfs_status,
  up accounting, fp_drain)
- upstream r18.5: new kernel/susfs_create_gate.c (create-family EACCES via
  fp hooks), /proc/susfs_map add/del/clear, readlink via the inode i_op,
  sus_map table-entry wrappers instead of kprobes (#147), hide_syms and
  isolated-UID gating fixes, sus_mount dynamic sizing
- samsung: rkp_branch is layer 0 (route known before anything patches),
  fail_layer index shift 1..19, /proc/susfs_status node, susfs_fp_drain on
  unload, one landing-pad test in kernel/susfs_arm64.h (exact
  BTI C / BTI JC / PACIASP / PACIBSP set, not a bitmask - bti and bti j
  are not valid indirect-call targets)
- inode_readlink now has an LSM kprobe carrier, so a hidden symlink answers
  ENOENT on kernels where the LSM hook list cannot be written (Samsung
  NO_PATCH_TEXT); this is the only new code in this sync
- fixes found by the post-merge review: struct susfs_layer.up was missing
  (would not compile), and /proc/susfs_status creation now uses the
  upstream node gate susfs_control_node_allowed() instead of the handler
  gate (it used to create a 0777 node even with expose_proc=0)
- ABI unchanged for FinaSU ksud: version "v2.3.0", variant "GKI"
- module.prop -> v2.3.0-r18.5 (versionCode 3); ko rebuilt for 5.15/6.1/6.6
'''


def token():
    r = subprocess.run(['git', 'credential-manager', 'get'],
                       input='protocol=https\nhost=github.com\n\n', capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith('password='):
            return line.split('=', 1)[1]
    raise SystemExit('no token')


T = token()
PROXY = urllib.request.ProxyHandler({'https': 'http://127.0.0.1:7897', 'http': 'http://127.0.0.1:7897'})
DIRECT = urllib.request.ProxyHandler({})
opener = urllib.request.build_opener(PROXY)


def req(url, method='GET', payload=None, ctype='application/json', raw=None):
    global opener
    data = raw if raw is not None else (json.dumps(payload).encode() if payload is not None else None)
    r = urllib.request.Request(url, data=data, method=method, headers={
        'Authorization': 'Bearer ' + T, 'User-Agent': 'susfs-push',
        'Accept': 'application/vnd.github+json', 'Content-Type': ctype})
    for attempt in range(1, 6):
        try:
            with opener.open(r, timeout=600) as resp:
                body = resp.read()
                return resp.status, json.loads(body) if body and body[:1] in (b'{', b'[') else None
        except urllib.error.HTTPError as e:
            print('HTTP', e.code, method, url)
            print(e.read().decode(errors='replace')[:2000])
            raise
        except Exception as e:  # SSL EOF / reset through the local proxy
            print('retry %d/5 after %s: %s' % (attempt, type(e).__name__, e))
            if attempt == 3:
                print('switching to a direct connection')
                opener = urllib.request.build_opener(DIRECT)
            if attempt == 5:
                raise
            import time
            time.sleep(2 * attempt)


def blob_sha(path):
    with open(path, 'rb') as f:
        data = f.read()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest(), data


# 1. remote state
_, head = req('https://api.github.com/repos/%s/git/ref/heads/main' % REPO)
base_sha = head['object']['sha']
_, base_c = req('https://api.github.com/repos/%s/git/commits/%s' % (REPO, base_sha))
base_tree = base_c['tree']['sha']
_, tree = req('https://api.github.com/repos/%s/git/trees/%s?recursive=1' % (REPO, base_tree))
assert not tree.get('truncated')
remote = {e['path']: e for e in tree['tree'] if e['type'] == 'blob'}
print('remote base commit:', base_sha[:9], '| blobs:', len(remote))

# 2. diff the published trees against the remote
updates, added, missing_local, same = [], [], [], 0
for path, e in sorted(remote.items()):
    local = os.path.join(WORK, path.replace('/', os.sep))
    if not os.path.isfile(local):
        missing_local.append(path)
        continue
    sha, data = blob_sha(local)
    if sha == e['sha']:
        same += 1
        continue
    if path in KEEP_REMOTE:
        print('  !! %s differs locally; keeping the repo copy (see SAMSUNG.md 14)' % path)
        continue
    updates.append((path, e['mode'], sha, data))

tracked = subprocess.run(['git', 'ls-files'], cwd=WORK, capture_output=True, text=True).stdout.split()
for path in tracked:
    if not (path.startswith(PUB_TREES) or path in PUB_EXTRA):
        continue
    if path in remote:
        continue
    local = os.path.join(WORK, path.replace('/', os.sep))
    if not os.path.isfile(local):
        raise SystemExit('ADDITION missing locally: ' + path)
    sha, data = blob_sha(local)
    added.append((path, '100644', sha, data))

print('unchanged: %d | changed: %d | added: %d | remote-only (left as-is): %s'
      % (same, len(updates), len(added), missing_local))
for p, *_ in updates + added:
    print('  ->', p)
if not updates and not added:
    print('nothing to push')
    new_sha = base_sha
else:
    # 3. blobs
    entries = []
    for path, mode, sha, data in updates + added:
        st, b = req('https://api.github.com/repos/%s/git/blobs' % REPO, 'POST',
                    {'content': base64.b64encode(data).decode(), 'encoding': 'base64'})
        assert b['sha'] == sha, (path, b['sha'], sha)
        entries.append({'path': path, 'mode': mode, 'type': 'blob', 'sha': sha})
    # 4. tree + commit + ref
    st, t = req('https://api.github.com/repos/%s/git/trees' % REPO, 'POST',
                {'base_tree': base_tree, 'tree': entries})
    st, c = req('https://api.github.com/repos/%s/git/commits' % REPO, 'POST', {
        'message': COMMIT_MSG, 'tree': t['sha'], 'parents': [base_sha],
        'author': {'name': 'Ve.Ve.', 'email': 'Yettocom@outlook.com', 'date': '2026-10-28T22:40:00+08:00'},
        'committer': {'name': 'Ve.Ve.', 'email': 'Yettocom@outlook.com', 'date': '2026-10-28T22:40:00+08:00'}})
    st, ref = req('https://api.github.com/repos/%s/git/refs/heads/main' % REPO, 'PATCH',
                  {'sha': c['sha'], 'force': False})
    assert ref['object']['sha'] == c['sha']
    new_sha = c['sha']
    print('pushed commit:', c['sha'][:9], 'on main (parent %s)' % base_sha[:9])

# 5. release (skip if tag exists)
_, rels = req('https://api.github.com/repos/%s/releases' % REPO)
rel = next((r for r in rels if r['tag_name'] == TAG), None)
if rel:
    print('release %s already exists, skipping creation' % TAG)
else:
    with open(os.path.join(WORK, '_release_notes_r185.md'), 'rb') as f:
        notes = f.read().decode('utf-8')
    st, rel = req('https://api.github.com/repos/%s/releases' % REPO, 'POST', {
        'tag_name': TAG, 'target_commitish': 'main',
        'name': 'v2.3.0-r18.5 (Samsung LKM, late-load)', 'body': notes})
    print('release created:', rel['id'], rel['html_url'])

# 6. assets
have = {a['name'] for a in rel['assets']}
for name, want in ASSETS:
    p = os.path.join(DIST, name) if name.endswith('.zip') else os.path.join(WORK, 'out', name)
    with open(p, 'rb') as f:
        data = f.read()
    got = hashlib.sha256(data).hexdigest()
    assert got == want, (name, got, want)
    if name in have:
        print('asset already present:', name)
        continue
    url = 'https://uploads.github.com/repos/%s/releases/%d/assets?name=%s' % (REPO, rel['id'], name)
    try:
        st, a = req(url, 'POST', raw=data, ctype='application/octet-stream')
        print('uploaded:', a['name'], a['size'], a['state'])
    except urllib.error.HTTPError as e:
        # A reset response after a completed upload lands here on the retry as 422
        # already_exists; treat the asset as done when the size matches.
        if e.code != 422:
            raise
        _, cur = req('https://api.github.com/repos/%s/releases/%d' % (REPO, rel['id']))
        hit = next((x for x in cur['assets'] if x['name'] == name), None)
        if not hit or hit['size'] != len(data):
            raise
        print('asset already uploaded:', hit['name'], hit['size'], hit['state'])

# 7. retire the previous release (its ko and zip stay on disk and in this repo's history)
old = next((r for r in rels if r['tag_name'] == OLD_TAG), None)
if old:
    req('https://api.github.com/repos/%s/releases/%d' % (REPO, old['id']), 'DELETE')
    print('deleted release', OLD_TAG, old['id'])
    st, _ = req('https://api.github.com/repos/%s/git/refs/tags/%s' % (REPO, OLD_TAG), 'DELETE')
    print('deleted tag ref', OLD_TAG, 'status', st)
else:
    print('no release', OLD_TAG, 'to retire')

print('DONE. release:', rel['html_url'], '| main:', new_sha[:9])
