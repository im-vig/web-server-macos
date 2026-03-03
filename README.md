# web-server

## Overview

This is a lightweight C++ web server with:

- event-driven I/O (`epoll` on Linux, `kqueue` on macOS)
- thread pool for request handling
- static file serving via `mmap`
- optional MySQL-backed `/login` check

## Build

```bash
make
```

The build auto-detects MySQL via `mysql_config`:

- if detected: MySQL support is enabled
- if not detected: server still builds, `/login` will return failed and log that MySQL is disabled

## Run

```bash
./server 8080
```

Then open:

- `http://127.0.0.1:8080/`
- `http://127.0.0.1:8080/login`

## Database Config

The server reads DB config from environment variables:

- `WS_DB_HOST` (default `localhost`)
- `WS_DB_PORT` (default `3306`)
- `WS_DB_USER` (default `root`)
- `WS_DB_PASSWORD` (default empty)
- `WS_DB_NAME` (default `webdb`)
- `WS_DB_POOL_SIZE` (default `10`)

If these variables are not provided, the server uses local defaults:
`127.0.0.1:3306`, user `root`, password `123456`, database `webdb`.

## Notes

- static resources are served from `./resources`
- press `Ctrl+C` for graceful shutdown
- path traversal like `/../main.cpp` is blocked with `403`
