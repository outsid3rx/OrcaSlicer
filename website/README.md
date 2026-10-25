# OrcaSlicer Agent — static website

[English](README.md) | [Русский](README.ru.md)

A bilingual project page (English by default, Russian at `/ru/`): fork description,
model and slice demonstration, capabilities, AI-assisted installation, manual
installation, and FAQ.

Canonical sources live in `website/` in the main OrcaSlicer fork.
`dist/` contains ready-to-serve HTML, CSS, JavaScript, local fonts, and images.
English lives in `dist/index.html`, Russian in `dist/ru/index.html`; both use the
shared `dist/app.js` and include language navigation. No build, npm, database,
or server-side logic is required.

## Local preview

From the fork root:

```sh
python3 -m http.server 8080 --directory website/dist --bind 127.0.0.1
```

Open `http://127.0.0.1:8080` for English or `http://127.0.0.1:8080/ru/` for Russian.
Both pages include a language switch. Files can also be opened directly;
copying has a fallback, and text remains available for manual selection even
without JavaScript. The FAQ and manual instructions use HTML `details`.

## Hosting

Publish the contents of `website/dist/` on any static host.
For GitHub Pages, select **GitHub Actions** as the source in Settings → Pages. The **Agent website** workflow publishes `website/dist/` automatically after website changes reach `main`; manual runs are also available. The rest of the repository is not uploaded to Pages.

Primary website: https://outsid3rx.github.io/OrcaSlicer-Agent-control/.

Paths are relative, so hosting under a GitHub Pages subdirectory also works.
`.openai/hosting.json` contains only the Sites deployment identifier and
static-file path; it is not needed for ordinary static hosting.

The website does not install software: it copies a readable prompt or commands.
The installation prompt is universal for stdio MCP clients, including Codex,
Claude Desktop, Cursor, Windsurf, and ZCode, subject to each client's capabilities.
There is no client selector. Manual installation emits generic configuration by
default; `--client codex` and `--client zcode` are optional automatic setup flags.
The page does not connect to the slicer, MCP, printer, analytics, or third-party
APIs. Fonts are included locally; external services open only through links
followed by the user.

Without `--client`, installation sets up only the MCP server and prints its stdio configuration; it does not install the skill. For other clients, copy the bundled `skills/orca-mcp-control/` following the client’s skill-installation documentation. If the client has no native skill support, apply the instructions in `SKILL.md` as project guidance.

## Updating content

Check instructions against [`docs/agent/INSTALL.md`](../docs/agent/INSTALL.md),
`scripts/agent/install.sh`, and `skills/orca-mcp-control/SKILL.md`. Do not announce
unverified releases, platforms, or printers as supported. A `doctor` check does
not prove connection to an Orca window. Do not add print authorization to the
installation prompt.

## Design and assets

The light design system from the supplied private reference uses a gray canvas, dotted
grid, white rounded surfaces, yellow/black button pairing, and blue, red, and
peach accents. Nunito Sans is the suggested Poppins replacement with Cyrillic
support; Inter is used for body text.

`dist/assets/favicon.svg` is the shared rounded logo used in the header, footer
and browser tab. `dist/assets/logo.png` is its 512px transparent export for the
Pillow social-card generator; both localized cards use this same mark. The SVG
refines the rounded concept generated in Canva, with equal 10-unit layer spacing
and a 3.6-unit stroke. The symbol is centered at 88% scale to leave more internal
padding. When changing the SVG, refresh its PNG before the cards
(Node.js with `sharp` installed is needed only for this export):

```sh
node -e "require('sharp')('website/dist/assets/favicon.svg', {density: 768}).resize(512, 512).png().toFile('website/dist/assets/logo.png')"
python3 website/generate_social_images.py
```

Fonts are distributed under the SIL Open Font License; license texts are in
`dist/assets/*-OFL.txt`. `model.png` and `slice.png` were obtained through native
MCP in an OrcaSlicer test project. The conversation and cards on the page are an
illustrative demonstration, not a record of an actual submitted job.

## Checks

```sh
node --check website/dist/app.js
python3 website/check_site.py
```

The previous version was visually checked at widths of 390, 620, and 1440 px,
including client selection, prompt copying, manual instructions, FAQ, and the
model/slice toggle. These are historical results for the previous interface;
they do not establish verification of the current language pages or constitute
a physical mobile-device test. After changing the pages, check both languages,
language navigation, universal prompt copying, manual instructions, FAQ, and the
model/slice toggle again.

## Search metadata and social previews

The primary public URL is `https://outsid3rx.github.io/OrcaSlicer-Agent-control/`. Both language pages have
self-canonical URLs, reciprocal absolute hreflang links and an English x-default.
sitemap.xml lists the two canonical pages and their language alternatives.
The bundled robots.txt advertises this sitemap, but crawlers read robots.txt at
the host root; the project subdirectory file does not control the entire
outsid3rx.github.io host. Hosting must allow anonymous access.

Open Graph and Twitter use localized 1200x630 PNG cards. SoftwareApplication
JSON-LD describes the open-source macOS project without invented ratings,
reviews, prices or versions. These tags do not guarantee indexing or rich results.

When changing the primary site URL, update both HTML heads, JSON-LD, robots.txt,
sitemap.xml and SITE_URL in check_site.py. UI asset paths remain relative; SEO
URLs deliberately name the primary site URL. Social cards can be regenerated
with `python3 website/generate_social_images.py` from the fork root. Pillow is
required only for that optional maintenance operation; serving the site still
requires no build or Python dependencies. Font licenses remain with the assets.
