# Baked player appearances

Player clips in `hd/sprites.txt` can use the key
`<class>/<armor>/<equipment>/<action>/<direction>`, for example
`warrior/light/sword_shield/walk/n`. Classes are `warrior`, `rogue`, and
`sorcerer`; armor tiers are `light`, `medium`, and `heavy`. Equipment names
are `unarmed`, `unarmed_shield`, `sword`, `sword_shield`, `bow`, `axe`,
`mace`, `mace_shield`, and `staff`. The renderer selects these from the
player's packed graphics number, so gear changes select the matching art.

Death uses the armor tier's `unarmed` namespace. Town casting with a sword
or mace uses `unarmed`, and casting with either plus a shield uses
`unarmed_shield`, matching the classic animation selection. Actions remain
`stand`, `town_stand`, `walk`, `attack`, `block`, `hit`, `cast`, and `death`.
An optional `town_walk` clip overrides `walk` in town for each available exact facing,
allowing a stowed weapon. Packs without it continue to use `walk`.
Directions use the existing `s`, `sw`, `w`, `nw`, `n`, `ne`, `e`, `se` order.

An incomplete appearance falls back to the classic body when no usable HD
clip exists for that facing. While a clip loads, a resident pose of the same
appearance and facing may stand in. Equipment namespaces never borrow a
neighbor's facing. Legacy `warrior/<action>/<direction>` clips are eligible
only for a light-armored sword-and-shield warrior; they retain their existing
neighbor-facing fallback. A modern matching appearance takes precedence.
The repository ships no player sheets.

# Cathedral floor preview

`cathedral-floor-atlas.png` is an original 4x floor set for the WebGPU browser
build. It is a 1024x512 RGBA PNG containing sixteen 256x128 variants in a
four-column, four-row grid. Each variant covers one logical 64x32 Cathedral
floor diamond. The texture uses small, staggered charcoal and brown bricks to
stay close to the classic Cathedral pattern. `cathedral-floor-source.png` is
the generated source artwork.

The browser build bundles the atlas. A loose file at
`hd/cathedral-floor-atlas.png` under the active asset override path takes
precedence, so an edited atlas can be tried without changing the game archive.
Reload after rebuilding the browser package. The classic floor is the default;
open the game with `?hdFloor=1` to opt into the HD preview at the same save and
location. Remove the query parameter to return to the classic floor.

The preview applies to Cathedral floors and, with the separate town pages
described below, to the town floor. It keeps the original dungeon
collision, vision, and tile composition data. Zoom currently uses the classic
floor until the floor placement metadata is scaled with the zoomed viewport.
The atlas uses neutral albedo. Without hybrid lighting the presenter scales it
by the classic floor's local colour (see below) before applying the existing
WebGPU light mask, so the preview does not expand the player's visible radius.

# Town floor

The same `?hdFloor=1` flag also replaces the town floor when the asset path
contains `hd/town-floor.txt` and its atlas pages (DamnationX HD runtime
contract v1, section 1). The repository does not ship these files. Each line is
`atlas <N> <file> <width> <height>` (a 4096-wide page, up to 4096 tall, of
256x128 cells, 16 per row) or `piece <piece_index> <page> <cell>`. A town floor
diamond whose piece (the `dPiece` value) is listed is drawn from that cell;
unlisted pieces keep the classic floor. Listed pieces may be walkable floor
pieces or, in town, solid pieces (river banks, tree bases, wall feet) whose
bottom floor blocks are drawn with the walls: for those, only the diamond
pixels the bottom blocks actually draw are replaced, and their upper blocks stay
classic.

The cell alpha is a blend weight: 255 is HD ground, 0 keeps the classic pixel
(water, bank edges, roofs, rocks, roots, wall feet), values in between mix the
two, in the classic composite and under hybrid lighting alike. A cell that is
opaque everywhere replaces the whole diamond. The classic-brightness match
below counts each classic sample by the alpha at its place in the cell. The pages load
the first time town is drawn with the flag set, into one GPU texture array
(4096 x tallest page x pages). A missing or malformed manifest or page logs an
error and keeps the classic town floor.

Town and Cathedral cells keep their own detail but follow the classic floor at
low frequency: the presenter averages the captured classic floor colour over
about 20x20 pixels (so the classic dither and brick pattern do not show),
divides it by the mean colour of the HD cells drawn there, and multiplies the
HD albedo by that gain. The HD floor therefore takes the classic local
brightness and tint (the per-pixel lightmap, lamps and the forge, the blue
night palette of town) before the same light mask applies. Where no classic
floor sample is near, the diamond's light-table level is used instead.

Floor decals of the classic Cathedral art stay classic: blood pools, bone piles
and the shadows at torch stand bases are part of the floor CEL frames, so the
presenter keeps the classic pixel where the captured palette index belongs to
a decal colour family that is dense nearby (the red ramp 48-63 and its rim
80-95 for blood, the blue-grey ramps 1-31 for bones, runs of the darkest shades
inside a lighter surround for shadows), with a soft one-pixel edge. Light
tables only move an index along its 16-entry ramp, so this works at any light
level. Plain Cathedral floor uses the grey and brown ramps. Under
hybrid lighting the GPU lights the raw albedo (town keeps the classic path,
since hybrid lighting is off in town). Zoom uses the classic floor.
