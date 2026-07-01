# eosfs — Coleco ADAM EOS filesystem tool

A small, portable C99 command-line tool for building and editing **EOS**
filesystem images for the Coleco ADAM, as raw **DDP** or **DSK** block images.
Intended as part of a build toolchain for software that runs under EOS.

* Create a new EOS filesystem (256K DDP, 160K DSK, or a custom size).
* Add, replace, remove, and extract files.
* Show the EOS directory.

Creating a filesystem lays down **3 directory blocks** by default (`--dir-blocks`
to change), matching the ADAM `INITIALIZE DIRECTORY` convention.

## Build

```sh
make            # produces ./eosfs
make test       # round-trip / interleave test suite
make install    # installs to $PREFIX/bin (default /usr/local)
```

No dependencies beyond the C standard library. `cc -std=c99 -o eosfs eosfs.c`
works anywhere.

## Usage

```
eosfs create <image> <preset> [options]
    presets: ddp256   256K DDP (256 blocks)
             dsk160   160K DSK (160 blocks)
             ddp      custom DDP, needs --blocks N
             dsk      custom DSK, needs --blocks N (multiple of 4)
    options: -v, --volume NAME     volume name (<= 11 chars)
             -d, --dir-blocks N    directory blocks (default 3)
             -b, --blocks N        block count for custom presets

eosfs list    <image>
eosfs add     <image> <hostfile> [--name EOSNAME] [--date YYYY-MM-DD] [--attr BYTE]
eosfs replace <image> <hostfile> [--name EOSNAME] [--date YYYY-MM-DD] [--attr BYTE]
eosfs remove  <image> <eosname>
eosfs extract <image> <eosname> [-o OUTFILE]
eosfs attr    <image> <eosname> <BYTE>
eosfs boot    <image> [mode]
```

### File attributes

Byte 12 of every directory entry is the EOS **attribute byte**. `add` gives new
files `0x10` (plain user file) unless you pass `--attr`, and `replace` keeps the
existing attribute unless you pass `--attr`. To change the attribute of a file
already on the image without rewriting its data, use `eosfs attr`:

```sh
eosfs add  game.ddp game.bin --name GAMEH --attr 0xD0   # write+delete protected
eosfs attr game.ddp GAMEH 0x90                           # change it later
```

`BYTE` is hex (`0xD0`), octal (`0320`), or decimal (`208`). The set bits are
(from the EOS 5 directory format):

| bit  | mask   | meaning                                   |
|------|--------|-------------------------------------------|
| 0    | `0x01` | not a file (BLOCKS LEFT terminator)       |
| 1    | `0x02` | execute protected                         |
| 2    | `0x04` | deleted                                   |
| 3    | `0x08` | system file (hidden from SmartBASIC CATALOG) |
| 4    | `0x10` | user file                                 |
| 5    | `0x20` | read protected                            |
| 6    | `0x40` | write protected (read only)               |
| 7    | `0x80` | delete protected                          |

So `0xD0` is a write- and delete-protected user file, `0xC8` is the `DIRECTORY`
system entry (write- and delete-protected system file), and `0xCA` adds
execute-protection to that. Bits `0x01` (not-a-file) and `0x04` (deleted) are
rejected — they would corrupt the directory or hide the file; use `remove` to
delete.

### File-name type byte

An EOS name is *up to 10 characters, a **type byte**, then a `0x03` terminator*
— i.e. the type byte is the last character of the name before `0x03`. EOS marks
runnable binaries with a **CTRL-B (`0x02`)** type byte there. Any name argument
(`--name`, and the `<eosname>` looked up by `extract`/`remove`/`attr`/`boot
--file`) accepts a `\xHH` escape, so you write the byte inline:

```sh
eosfs add     game.ddp game.bin --name 'GAME\x02' --attr 0xD0   # runnable binary
eosfs boot    game.ddp --file 'GAME\x02'
eosfs list    game.ddp            # shows the name as GAME^B (caret notation)
eosfs extract game.ddp 'GAME\x02' -o game.bin
```

Single-quote the name so the shell keeps the backslash. Control bytes are shown
in caret notation by `list` (CTRL-B → `^B`). A literal backslash is `\\`; a NUL
(`\x00`) and the terminator (`\x03`) are rejected as name bytes.

### Boot blocks

Block 0 of an ADAM medium is the boot block. On power-up EOS reads it into
`0xC800` and **jumps to it unconditionally** with `B` = the boot device number
— there is no "is this bootable" signature. A block 0 full of zeros would be
executed as `NOP`s and crash, so every image this tool writes has a valid
block 0. `create` installs a jump to SmartWriter by default; `eosfs boot`
changes it:

```
eosfs boot game.ddp                       # (default) DI ; JP SmartWriter (0xFCE7)
eosfs boot game.ddp --none                # same
eosfs boot game.ddp --block loader.bin    # install a verbatim 1024-byte block 0
eosfs boot game.ddp --file MYPROG         # load an EOS file already on the image
                  [--load ADDR] [--entry ADDR]
```

`--file` synthesises a small loader (the splash-free equivalent of
`smartbasic-1.x/boot-block.asm`, generalised to any file size) that opens the
named EOS file on the boot device, streams it into RAM, and runs it. Two file
shapes are recognised:

* **BLOAD** files begin with the 5-byte header `01 00 02 <load_lo> <load_hi>`.
  The loader discards the header, loads the payload at the address from the
  header, and jumps there. (This is how SmartBASIC 1.x boots.)
* **Raw** files have no header. The whole file is loaded — at `0x0100` by
  default, like the Coleco Disk Manager — and executed there.

`--load` overrides the load address (hex `0x...` accepted); `--entry` sets the
entry point (defaults to the load address). Any EOS error during boot falls
back to SmartWriter. Because the loader opens the file *by name*, the boot file
may be freely added/replaced later without regenerating the boot block.

Typical bootable build:

```sh
eosfs create game.dsk dsk160 -v MYGAME
eosfs add    game.dsk game.bin --name GAMEH   # a BLOAD image (01 00 02 ..)
eosfs boot   game.dsk --file GAMEH
```

Media type (DDP vs DSK) is inferred from the `.ddp` / `.dsk` filename
extension. Add `--type ddp|dsk` to any command to override or when the
extension is something else.

`add` uses the host filename (basename) as the EOS name unless `--name`
is given; EOS names are at most 11 characters.

### Example

```sh
eosfs create game.ddp ddp256 -v MYGAME
eosfs add    game.ddp loader.bin  --name BOOTLOADER
eosfs add    game.ddp level1.dat  --name LEVEL1
eosfs list   game.ddp
eosfs replace game.ddp level1.dat --name LEVEL1
eosfs remove game.ddp LEVEL1
```

## What the tool guarantees

The whole filesystem is read into memory as a model (volume parameters plus
the list of files and their contents) and rewritten compactly on every change.
Files are laid out contiguously immediately after the directory — exactly how
EOS appends them on a fresh volume. Removing a file compacts the survivors.
The output is always a clean, valid EOS volume with a single free extent
(`BLOCKS LEFT`); it never leaves deleted-file holes.

Because files move to stay contiguous, do not rely on a file keeping a fixed
block number across edits. EOS accesses files by directory name, so this is
transparent to software on the ADAM. The boot block (block 0) is preserved
across edits.

## On-medium format

### Block layout

```
block 0          BOOT block (reserved, 1 block)
blocks 1..D      DIRECTORY (D "directory blocks", default 3)
blocks D+1 ..    file data, then free space
```

### DDP vs DSK

Both images are `blocks * 1024` bytes. They differ only in where each logical
1K block lands in the file:

* **DDP** — blocks are contiguous: logical block *N* is at byte offset
  `N * 1024`.
* **DSK** — a 5:1, 512-byte-sector interleave (the same mapping the FujiNet
  ADAM media layer uses in `mediaTypeDSK.cpp`). Each 1K block is split into two
  512-byte halves placed at interleaved offsets within its 4-block (8-sector)
  group. DSK images must be a whole number of 4-block groups (160 is).

### Directory entry (26 bytes, little-endian)

| Offset | Size | Field                                             |
|-------:|-----:|---------------------------------------------------|
| 0      | 12   | name, `0x03`-terminated, space padded             |
| 12     | 1    | attribute byte                                    |
| 13     | 4    | start block (VOLUME entry: check code `55 AA 00 FF`) |
| 17     | 2    | allocated length, in blocks                       |
| 19     | 2    | used length, in blocks                            |
| 21     | 2    | byte count in the last block                      |
| 23     | 1    | year (`year - 1900`)                              |
| 24     | 1    | month                                             |
| 25     | 1    | day                                               |

39 entries fit in each 1K directory block (`39 * 26 = 1014`).

File size in bytes = `(used_length - 1) * 1024 + last_block_byte_count`.

Attribute bits: `0x01` = not-a-file (`BLOCKS LEFT` end marker), `0x04` =
deleted, `0x10` = user file, `0x80` = locked/system.

The first three directory entries are always **VOLUME** (attr `0x80 | D`,
check code, and volume length in the allocated-length field), **BOOT**
(attr `0x88`), and **DIRECTORY** (attr `0xC8`). File entries follow, then a
**BLOCKS LEFT** sentinel (attr `0x01`) whose start block and allocated length
describe the free extent.

This layout is taken directly from the EOS 5 `INITIALIZE DIRECTORY`
(Fn 47), `CREATE FILE` (Fn 51), and `DELETE FILE` (Fn 59) routines in the
disassembly under `learn/`.

## License

eosfs is free software, licensed under the GNU General Public License
version 3 or later (GPL-3.0-or-later). See [LICENSE](LICENSE) for the full
text.
