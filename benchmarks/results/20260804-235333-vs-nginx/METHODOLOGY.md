# http-server vs nginx, methodology

Head-to-head run behind the "vs nginx" table in the README. Both servers
were measured in the same session, alternating per data point (mine,
nginx, mine, nginx, ...) rather than one server after the other, so that
any drift in machine state hits both equally.

## Both sides

- Same machine: Linux 6.18 (WSL2), 8 CPUs online.
- Same document root: `/home/hygor/projects/http-server/public`.
- Same files: `/hello.txt` (271 bytes) and `/benchmark-large.bin` (8MiB).
- Same load: `wrk -t<threads> -c<conns> -d10s --latency --timeout 10s`,
  threads = min(conns, 4), concurrency 1 / 10 / 50 / 200.
- One unrecorded 2s warmup per scenario, per server, before the first
  measured run.

## This server

- `http-server-release`, built by `make release`: `-O2`, no sanitizers.
- Port 8080, 8 worker threads (one per online CPU, auto-detected).
- No access log.

## nginx

- nginx/1.28.3 (Ubuntu), the system service, 8 worker processes
  (`worker_processes auto`).
- Port 8081, from `/etc/nginx/sites-available/http-server-bench`:

      server {
          listen 8081;
          server_name localhost;
          root /home/hygor/projects/http-server/public;
          location / { try_files $uri $uri/ =404; }
      }

- From `/etc/nginx/nginx.conf`: `sendfile on`, `tcp_nopush on`,
  `worker_connections 768`, `gzip on`,
  `access_log /var/log/nginx/access.log`.

## Differences that matter when reading the numbers

- **nginx writes an access log line per request**, this server writes
  none. That is a real cost on the small-file scenario, where per-request
  overhead dominates, and it is not configuration this run controlled for
  (the config was already in place on the machine).
- **nginx runs `try_files`**, which adds a `stat()` per request.
- **gzip is enabled** in nginx but never triggers: wrk does not send
  `Accept-Encoding: gzip`.
- **Different accept strategies**: this server uses one `SO_REUSEPORT`
  listen socket per worker; the nginx config uses a single shared listen
  socket without `reuseport`.
- **nginx implements vastly more** (vhosts, rewrites, proxying, TLS,
  compression, caching). The comparison is only meaningful for the narrow
  workload both are doing here: sending static bytes off local disk.
- **Loopback only.** No network, no RTT, no loss, and wrk competes with
  the server for the same 8 CPUs.
