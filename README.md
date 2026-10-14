# Pacman Multiplayer Server

A concurrent client–server version of a Pac-Man–style terminal game, written in **C** with **POSIX** primitives.
Built for the *Operating Systems* course at Instituto Superior Técnico (2025/26), as a group project.

## Highlights
- **Client–server over named pipes (FIFOs):** clients register via a public FIFO and get private request/notification pipes.
- **Producer–consumer connection queue:** a bounded buffer protected by **semaphores** (`sem_t`) and a mutex limits the number of concurrent games.
- **Thread-per-entity game loop:** each session runs Pac-Man, every ghost and the board-update sender on separate **pthreads**, synchronised with mutexes and read-write locks.
- **Signal handling:** `SIGUSR1` is blocked on worker threads and handled only by the main thread, which writes a top-5 leaderboard to `top5.txt`. `SIGPIPE` is ignored so client disconnects don't crash the server.
- Compiled with `-Wall -Wextra -std=c17`.

## Build & run
Requires `gcc`, `make` and `ncurses`.

```bash
make                                  # builds bin/server and bin/client
./bin/server levels 4 /tmp/pacman_reg # <levels_dir> <max_concurrent_games> <register_fifo>
./bin/client 1 /tmp/pacman_reg        # <client_id> <register_fifo> [commands_file]
```

## Structure
| Path | Contents |
|---|---|
| `src/server/server.c` | Server: FIFO registration, connection queue, worker threads, signals |
| `src/client/api.c`, `client_main.c` | Client-side protocol and main loop |
| `src/server/board.c`, `parser.c`, `src/client/display.c` | Game logic, level parser and ncurses display (course base code) |
| `levels/` | Level (`.lvl`), ghost (`.mon`) and Pac-Man (`.pac`) files |
