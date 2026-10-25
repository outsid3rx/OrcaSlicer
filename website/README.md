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
For GitHub Pages, select **GitHub Actions** in Settings → Pages, then manually
run the **Agent website** workflow. It checks the sources and publishes only
`website/dist/`, without uploading the rest of the repository. These instructions
do not assert that the workflow has been run.

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

The primary public origin is `https://orca-agent.ru`. Both language pages have
self-canonical URLs, reciprocal absolute hreflang links and an English x-default.
robots.txt allows crawling and advertises sitemap.xml, which lists the two
canonical pages and their language alternatives. Hosting access and working
DNS/TLS must also allow anonymous access; metadata cannot activate pending DNS.

Open Graph and Twitter use localized 1200x630 PNG cards. SoftwareApplication
JSON-LD describes the open-source macOS project without invented ratings,
reviews, prices or versions. These tags do not guarantee indexing or rich results.

When changing the primary domain, update both HTML heads, JSON-LD, robots.txt,
sitemap.xml and ORIGIN in check_site.py. UI asset paths remain relative; SEO
URLs deliberately name the primary domain. Social cards can be regenerated
with `python3 website/generate_social_images.py` from the fork root. Pillow is
required only for that optional maintenance operation; serving the site still
requires no build or Python dependencies. Font licenses remain with the assets.
