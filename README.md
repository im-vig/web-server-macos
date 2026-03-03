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

## Notes

- static resources are served from `./resources`
- press `Ctrl+C` for graceful shutdown
