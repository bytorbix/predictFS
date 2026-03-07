# predictFS

A research-driven filesystem built from scratch in C, integrated into Linux via FUSE, designed to explore whether file behavior can be predicted and pre-allocated for optimal contiguous writing.

## Idea

Instead of reacting to writes, it learns from them — tracking how large files of each type tend to grow, so it can reserve space before it's needed.

## Architecture

```
vfs   — FUSE integration, maps Linux syscalls into the stack
pfs   — prediction layer, tracks extension metadata and drives allocation hints
fs    — core filesystem: inodes, blocks, directories, bitmap, path traversal
disk  — raw block I/O on a flat binary image
```

## Build & Run

### Dependencies
```bash
sudo apt install gcc make libfuse-dev pkg-config
```

### Build
```bash
make
```

### Mount at /tmp/mnt
```bash
./fuse.sh
```
