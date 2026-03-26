# predictFS

A filesystem built from scratch in C, mounted into Linux via FUSE, that learns how files grow and pre-allocates contiguous space before it's needed.

## Idea

Most filesystems react to writes one block at a time. predictFS tracks historical file behavior by extension and size class — when a new file is written, it predicts how large the file will eventually become and reserves space upfront, reducing fragmentation without any changes to applications.

## How it works

On first write, predictFS looks up the file's extension and size bucket, computes a confidence score from historical stats, and pre-allocates blocks based on the predicted final size. On delete, it records whether the file grew and by how much, updating the stats for future predictions.

The prediction is gated by a confidence score built from three signals:
- **Sample weight** — do we have enough observations to trust the stats?
- **Tendency consistency** — is the grow/no-grow signal decisive?
- **Ratio consistency** — is the growth amount predictable?

If confidence is too low, it falls back to standard block-at-a-time allocation.

## Architecture

```
vfs   — FUSE integration, maps Linux syscalls into the stack
pfs   — prediction layer: bucket stats, confidence scoring, pre-allocation
fs    — core filesystem: inodes, extents, directories, bitmap, path traversal
disk  — raw block I/O on a flat binary image
```

### On-disk layout

```
Block 0          SuperBlock
Blocks 1–N       Inode table (10% of disk)
Blocks N–M       Bitmap
Block M+1        pfs ExtensionEntry stats table
Blocks M+2+      Data
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

### Run tests
```bash
./pfs
```

### Mount at /tmp/mnt
```bash
./fuse.sh
```
