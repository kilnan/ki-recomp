# HD pipeline: from the game to an HD pack

How the HD sprites and stages of `ki_play --hd-pack` are made, step by step,
and what has been done so far.

The formats and the reverse engineering behind them are in
[TECHNICAL_NOTE.md](TECHNICAL_NOTE.md): sections "Sprite blitter", "Character
data", "Stages" and "HD rendering".

**Everything this pipeline produces is derived from the game.** That covers
extracted sprites, upscaled images and packs. It stays in `data/hd/`, which
Git ignores, and is never committed or distributed.

## Layout of `data/hd/` (local)

| Path | Contents |
|---|---|
| `pack/` | The HD pack `ki_play` loads: `index.txt`, `shapes.txt`, `panoramas.txt`, `prerendered.txt`, `screens.txt`, `*.ki4`, `*.kix` |
| `upscaled/c0` … `c10`, `misc` | SeedVR2 output per character (`c160/`, `c320/` canvases, 4x) and for the scene sprites. Keep these: they cost GPU time. A pack can be rebuilt from them without the GPU. |
| `upscaled/stages/` | Upscaled panorama layers (`combo_layers_up.png` = stage 7, `stage6_up.png`, `stage8_up.png`) and `prerendered/` (the 84 pre-rendered stage frames, `sNN_fK.png`) |
| `atlases/` | The atlas layouts (`atlas.txt`) used to unpack the upscaled atlases |

Character numbers:

| Number | Character |
|---|---|
| 0 | Jago |
| 1 | Combo |
| 2 | Spinal |
| 3 | Thunder |
| 4 | Glacius |
| 5 | Fulgore |
| 6 | Cinder |
| 7 | Sabrewulf |
| 8 | Orchid |
| 9 | Riptor |
| 10 | Eyedol |

Character `i` uses palettes `6i+1` to `6i+6`, one per costume.

## Play with it

```sh
build/ki_play --data data/roms/kinst --hd-pack data/hd/pack --scale 4 \
    --opponent orchid,cinder --stage 4,1
```

| Key | What it does |
|---|---|
| H | HD on and off |
| P | Pause |
| R | Reset |
| M | Writes a report of the sprites drawn without HD, plus both pictures, into the preferences folder |
| F6 / F7 | Next fight's opponent / stage (on a Mac, hold Fn) |

## 1. Extract a character (local, no GPU)

The character's data are loaded at `0x880b1400` for player 1 and `0x88331000`
for the game-controlled opponent. SIZE is the byte size of its entry in the
directory at `0x8800d86c` (sectors × 512).

To force the opponent, use `--opponent`:

```sh
X=work/c8; mkdir -p $X
build/ki_run --boot rom --disk data/kinst.img --rom data/roms/kinst/ki-l15d.u98 \
    --inputs INPUTS --opponent 8 --frames 2200 --dump-every 100000 \
    --extract-sprites $X --extract-character 88331000:267e00:49,50,51,52,53,54 --out $X
```

Inputs: any script that reaches a fight. A one-player game with coins, start
and random moves is enough.

What it writes:
- every pose in all 6 costumes (`*.ppm`);
- `*.kix` (palette entry per pixel, for recolouring at run time);
- `keys.txt` and `index.txt` (including the palette `pal n`).

Scene sprites (HUD, objects, effects) are not part of the character data. Get
them with `--extract-sprites` alone, one fight per stage (`--stage 0..15`).
Then keep the images whose key and shape are not in the pack yet.

## 2. Prepare canvases and atlases

Upscale only one costume per character (palette `6i+1`); `pack.py --recolor`
derives the other five.

```sh
python3 tools/hd/prepare.py $X work/p8           # canvases of 160 or 320, colours spread into the transparent area
# keep only the canvases of the first costume, then:
python3 tools/hd/atlas.py pack CANVASES_160 work/atl/c8/a160 --grid 4
python3 tools/hd/atlas.py pack CANVASES_320 work/atl/c8/a320 --grid 2
```

Why atlases: SeedVR2's cost is mostly per image (~10 s each). With 16 sprites
per 640×640 atlas the cost drops to ~3 s per sprite. The canvases' 8 px margin
keeps neighbours from bleeding into each other.

## 3. Upscale on a GPU instance

Upscaler: **SeedVR2 7B sharp**, one image at a time (`--batch_size 1`).
- Video mode ghosts between poses.
- AnimeSharp looked close to bilinear.

Instance: an A100 80 GB on Vast.ai with the SeedVR2 CLI. The `seedvr2`
instance of `~/Projects/video-restore` was used and has since been destroyed.
`video-restore/seedvr2/scripts/bootstrap.sh` and `install_remote.sh` set up a
new one.

**Ask the owner before creating, starting, stopping or destroying an
instance: it is paid from their credit.**

```sh
SSH=~/Projects/video-restore/seedvr2/scripts/ssh.sh     # runs a command on the instance
COPYFILE_DISABLE=1 tar cf c8.tar -C work/atl c8
tools/hd/remote.py $SSH put c8.tar /workspace/ki-sprites/atl c8
cat tools/hd/vast_queue.sh | $SSH 'cat > /workspace/ki-sprites/atl/vast_queue.sh'
$SSH 'echo "c8" > /workspace/ki-sprites/atl/queue.txt; cd /workspace/ki-sprites/atl; nohup bash vast_queue.sh . > queue.log 2>&1 < /dev/null &'
# ... when queue.log says "done c8":
tools/hd/remote.py $SSH get /workspace/ki-sprites atl/out/c8 c8_out.tar
```

Lessons from the first run:
- **Banner:** remote commands print a banner; filter it out.
- **Start jobs with `nohup … &` after a `;`, not `&&`.** With `&&` the job dies with the SSH session.
- **Busy instance:** transfers stall when the instance is loaded. `remote.py` checks sizes and retries.
- **Credit:** when the credit runs out, Vast *stops* the instance and keeps the disk. Restarting it gives everything back.
- **Clean up:** delete `/workspace/ki-sprites` when done and log it in the instance's ACCESSO.md.

Measured: ~13–23 minutes per character (450–730 sprites) with atlases, and ~1 h for 1,536 scene sprites.

## 4. Unpack and build the pack (local)

```sh
tar xf c8_out.tar
python3 tools/hd/atlas.py unpack work/atl/c8/a160 atl/out/c8/a160 data/hd/upscaled/c8/c160
python3 tools/hd/atlas.py unpack work/atl/c8/a320 atl/out/c8/a320 data/hd/upscaled/c8/c320
python3 tools/hd/pack.py $X work/p8 data/hd/upscaled/c8 work/pack_c8 --recolor
```

Merge into `data/hd/pack`:
1. Move the `.ki4` and `.kix` files.
2. Append `index.txt` and `shapes.txt`, keeping the first line per key: `awk '!seen[$1]++'`.

**Disk:** a character's pack is ~1 GB with the current `.ki4` format (zlib ARGB). Delete the tars and intermediates as you go; the full pack is ~12 GB.

## 5. Stages

- **Panoramas** (stages 6, 7, 8): `tools/hd/panorama.py extract`, upscale, then `tools/hd/panorama.py pack`.
  - The layers' offsets in the stage file are those printed by `KI_HD_DEBUG=1 ki_run --hd-pack …` ("panorama copy: src …"), minus the stage's load address.
  - Stage 7: `0x3a218:74 0x5cf68:242`. Stage 6: `0xf9e4:100 0x3eb04:104`. Stage 8: `0x35b20:240`.
- **Pre-rendered** (stages 0–4 and 10–15, 8 frames each, 4 for stage 13): `tools/hd/prerendered.py extract`, upscale at `--resolution 960`, then `tools/hd/prerendered.py pack`.

## Status (21/10/2026)

**Done:**
- all 11 characters × 6 costumes;
- 1,536 scene sprites;
- panoramas of stages 6, 7 and 8;
- 84 pre-rendered frames.

**Open:**
- **Winner portraits.** The "WINNER" screen picture is decoded from disk (compressed, decoder around 0x88028dc0) into 0x884a0000. `ki_run --hd-pack P --dump-screens DIR` dumps the pictures, and `screens.txt` + `screen_<key>.ki4` replace them. The portraits still have to be collected (one won fight per character) and upscaled.
- **Mode-7 floors.** Stage 8's desert and stage 7's rooftop floor (0x8801c740 and its siblings) need their drawing redone at 4x.
- **Pre-rendered stages: the animated fire.** It is drawn separately and stays upscaled normally (about 1–2% of the blocks). The camera movement itself is handled exactly (see TECHNICAL_NOTE "Stages").
- **Smaller pack.** Use PNG or WebP instead of zlib ARGB.
