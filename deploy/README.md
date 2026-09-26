# Deploying WssdApi

CI (`.github/workflows/build_wssd-api.yml`) publishes two artifacts:

| artifact | contents | use it when |
|---|---|---|
| `wssd-api` | the bare `WssdApi` ELF | the host already has every runtime library |
| `wssd-api-bundle` | `wssd-api-bundle.tar.gz` — binary + bundled `lib/` + launcher + config | **recommended**: the host needs only glibc |

The binary is linked with `-static-libstdc++ -static-libgcc`, so the GCC 13
runtime the Sapo engine requires does **not** have to exist on the server.

## Bundle install (recommended)

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
