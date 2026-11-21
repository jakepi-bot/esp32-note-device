# Notes UI renders

The executable half of [`../notes-ui-spec.md`](../notes-ui-spec.md).

`render.cpp` draws all seventeen UI states straight into an `hg::Canvas` using the
same 5 x 7 font the firmware uses, then writes each one three ways and checks the
result. Because it links the SDK's own drawing code, a wireframe in the spec is a
photograph of the layout, not a sketch of it, and a string that overflows its slot
fails the build instead of shipping truncated.

## Build and run

```sh
make -C docs/notes-ui
docs/notes-ui/render out/
```

Output, per state:

| File | What it is |
|---|---|
| `<state>.pbm` | the exact 1-bit panel image, 5,000 bytes, the bytes `EpdDisplay` would send |
| `<state>.txt` | 100 x 100, one character per 2 x 2 px — the blocks in the spec's Appendix A |
| `<state>.full.txt` | 200 x 200, one character per pixel — native resolution |

The run also prints the ink bounding box of every state and fails non-zero text on
stderr for:

- a string wider than the slot that holds it,
- any ink outside x 4 .. 196 or below y 186,
- an About row whose label does not clear its right-aligned value by 12 px.

## Polarity, the one trap

PBM `P4` encodes **1 as black**. This panel's image encodes **1 as white**
(`EpdDisplay::convert()` sets a bit when luma >= 128). The renderer keeps the panel
convention in memory and inverts on the way out to PBM. Inverting twice produces a
perfectly plausible-looking black-background mock-up that is wrong, which has
already happened once on this board.

## Changing the design

`render.cpp` is the normative layout: the constants at the top are the grid, and
the spec's coordinate tables are transcribed from them. Change a number here, run
the renderer, and paste the new block into the spec — not the other way round.
