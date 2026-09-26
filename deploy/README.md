# Deploying WssdApi

CI (`.github/workflows/build_wssd-api.yml`) publishes two artifacts:

| artifact | contents | use it when |
|---|---|---|
| `wssd-api` | the bare `WssdApi` ELF | the host already has every runtime library |
| `wssd-api-bundle` | `wssd-api-bundle.tar.gz` — binary + bundled `lib/` + launcher + config | you want the tree on disk, managed by rsync/Ansible |
| `wssd-api-run` | **`wssd-api.run` — one executable file containing everything** | **recommended**: copy one file, run it |

The binary is linked with `-static-libstdc++ -static-libgcc`, so the GCC 13
runtime the Sapo engine requires does **not** have to exist on the server.

## Single-file install (recommended)

```bash
sudo install -m755 wssd-api.run /usr/local/bin/wssd-api.run
sudo useradd --system --home /var/lib/wssd-api --shell /usr/sbin/nologin wssd   # once
sudo mkdir -p /opt/wssd-api/runtime /var/lib/wssd-api
sudo chown -R wssd:www-data /opt/wssd-api /var/lib/wssd-api

sudo install -m644 deploy/wssd-api.service /etc/systemd/system/wssd-api.service
# edit the unit: ExecStart=/usr/local/bin/wssd-api.run  (see comments in it)
sudo systemctl daemon-reload && sudo systemctl enable --now wssd-api
```

`wssd-api.run` is a shell stub with a gzip payload appended. On first start it
unpacks itself into `$WSSD_RUNTIME_DIR/<payload-hash>` (default
`/opt/wssd-api/runtime`, content-addressed so two versions never collide) and
copies `config.json`, `config.yaml` and `sapo/` into `$WSSD_WORKDIR` (default
`/var/lib/wssd-api`) **only if they are not already there** — your edited
config survives upgrades. Later starts just exec the cached runtime.

Upgrading is: replace the one file, `systemctl restart wssd-api`.

Useful flags:

```bash
wssd-api.run --version        # build + payload id
wssd-api.run --extract DIR    # unpack without running (inspection/debug)
wssd-api.run --runtime-dir    # print the unpacked runtime path
```

Caveat: the runtime directory must be writable once, and it needs exec
permission — if `/opt` is mounted `noexec`, point `WSSD_RUNTIME_DIR` somewhere
that is not.

## Bundle install (tree on disk)

```bash
sudo useradd --system --home /var/www/wssd-api --shell /usr/sbin/nologin wssd   # once
sudo systemctl stop wssd-api

sudo mkdir -p /var/www/wssd-api
sudo tar -xzf wssd-api-bundle.tar.gz -C /tmp
sudo rsync -a --delete /tmp/wssd-api/ /var/www/wssd-api/
sudo chown -R wssd:www-data /var/www/wssd-api

sudo install -m644 deploy/wssd-api.service /etc/systemd/system/wssd-api.service
sudo systemctl daemon-reload
sudo systemctl enable --now wssd-api
journalctl -u wssd-api -f
```

Start `bin/wssd-api` (the launcher), never `bin/WssdApi` directly — the
launcher points the dynamic loader at the bundled `lib/`. Running the raw
binary is what produces:

```
error while loading shared libraries: libcares.so.2: cannot open shared object file
```

## Bare-binary install

Keep `ExecStart=/var/www/wssd-api/WssdApi` and install the runtime libraries:

```bash
sudo apt-get install -y libc-ares2 libjsoncpp25 libpq5 libhiredis0.14 \
  libbrotli1 libssl3 zlib1g
ldd /var/www/wssd-api/WssdApi | grep 'not found'   # must print nothing
```

## Secrets

Put credentials in `/etc/wssd-api/wssd-api.env` (`root:root`, mode `0640`); the
unit loads it with `EnvironmentFile=-`. Never commit them.

## Notes on the hardened unit

* `ReadWritePaths` lists `logs/` and `sapo/` only — Drogon's log files and the
  Sapo file state-store checkpoints are the only things the service writes.
  `ProtectSystem=strict` makes the rest of the filesystem read-only.
* `StartLimitBurst=5` / `StartLimitIntervalSec=60` stop the endless restart loop
  a broken deploy otherwise produces (`restart counter is at 30`): the unit
  fails visibly instead.
* `MemoryDenyWriteExecute` is deliberately omitted — see the comment in the unit.
* `User=wssd` replaces `User=root`; run the `useradd`/`chown` above before the
  first `systemctl start`, or the service will fail with permission errors.
