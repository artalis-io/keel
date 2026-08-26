# Site icon provenance

The inline SVG icons in `index.html` are **project-original geometry**, authored for this site.
They are NOT copied, transcribed, or adapted from Lucide or any other third-party icon set.

Context: C5-1 removed the runtime Lucide dependency (the page previously loaded
`unpkg.com/lucide` at runtime). The icons were then re-authored from scratch as simple,
primitive-based glyphs (rectangles, circles, lines, polylines/polygons, and a few short arcs),
using original coordinates on a 24x24 viewBox with `stroke="currentColor"`, `stroke-width="2"`. The
"github" links use a generic code glyph rather than the GitHub mark (no third-party trademark).

Because the geometry is original and committed in `index.html`, no third-party license notice is
required and future edits stay reproducible: add or change an icon by editing its inline `<svg>`
directly, keeping the same 24x24 stroke conventions. Do not paste geometry from an external icon
library into this repository without recording its source and license here.

The two brand marks (the Keel logo in the header and footer, `stroke-width="1.75"`) and
`mark.svg` / `og-card.svg` are the project's own brand assets.
