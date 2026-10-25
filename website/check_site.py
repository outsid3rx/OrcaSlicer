"""Check both static languages, internal links, assets and installation controls."""
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlparse
import json
import re
import struct
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent / 'dist'
PAGES = {'en': ROOT / 'index.html', 'ru': ROOT / 'ru/index.html'}
ORIGIN = 'https://orca-agent.ru'
URLS = {'en': ORIGIN + '/', 'ru': ORIGIN + '/ru/'}
ALTERNATES = {**URLS, 'x-default': URLS['en']}


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids, self.links, self.assets, self.copy_targets = [], [], [], []
        self.lang, self.select_count, self.text = None, 0, []
        self.copy_text, self.current_copy = {}, None
        self.alternates, self.canonicals, self.meta = {}, [], {}
        self.titles, self.jsonld = [], []
        self.current_title, self.current_jsonld = None, None

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == 'title':
            self.titles.append('')
            self.current_title = len(self.titles) - 1
        if tag == 'meta':
            key = attrs.get('name') or attrs.get('property')
            if key:
                self.meta.setdefault(key.lower(), []).append(attrs.get('content', ''))
        if tag == 'script' and attrs.get('type') == 'application/ld+json':
            self.jsonld.append('')
            self.current_jsonld = len(self.jsonld) - 1
        if tag == 'html':
            self.lang = attrs.get('lang')
        if tag == 'select':
            self.select_count += 1
        if 'id' in attrs:
            self.ids.append(attrs['id'])
        if tag == 'a' and attrs.get('href'):
            self.links.append(attrs['href'])
        if tag in ('img', 'script') and attrs.get('src'):
            self.assets.append(attrs['src'])
        if tag == 'link' and attrs.get('href'):
            self.assets.append(attrs['href'])
            relations = attrs.get('rel', '').split()
            if 'canonical' in relations:
                self.canonicals.append(attrs['href'])
            if 'alternate' in relations:
                self.alternates.setdefault(attrs.get('hreflang'), []).append(attrs['href'])
        if 'data-copy' in attrs:
            self.copy_targets.append(attrs['data-copy'])
        if tag == 'pre' and attrs.get('id'):
            self.current_copy = attrs['id']
            self.copy_text[self.current_copy] = ''

    def handle_endtag(self, tag):
        if tag == 'title':
            self.current_title = None
        if tag == 'script':
            self.current_jsonld = None
        if tag == 'pre':
            self.current_copy = None

    def handle_data(self, data):
        if self.current_jsonld is None:
            self.text.append(data)
        else:
            self.jsonld[self.current_jsonld] += data
        if self.current_title is not None:
            self.titles[self.current_title] += data
        if self.current_copy:
            self.copy_text[self.current_copy] += data


def local_target(page, url):
    parsed = urlparse(url)
    if parsed.scheme or parsed.netloc:
        if parsed.scheme != 'https' or parsed.netloc != urlparse(ORIGIN).netloc:
            return None
        target = (ROOT / parsed.path.lstrip('/')).resolve()
    elif parsed.path.startswith('/'):
        target = (ROOT / parsed.path.lstrip('/')).resolve()
    elif parsed.path:
        target = (page.parent / parsed.path).resolve()
    else:
        return None
    return target / 'index.html' if target.is_dir() else target


def check_seo(language, page, errors):
    def fail(message):
        errors.append(f'{language}: {message}')

    def meta(name):
        values = page.meta.get(name, [])
        if len(values) != 1 or not values[0].strip():
            fail(f'exactly one nonempty {name} meta tag required')
            return ''
        return values[0]

    if len(page.titles) != 1 or not page.titles[0].strip():
        fail('exactly one nonempty title required')
    description = meta('description')
    if page.canonicals != [URLS[language]]:
        fail('canonical must be the fully qualified self URL ' + URLS[language])
    expected_alternates = {key: [value] for key, value in ALTERNATES.items()}
    if page.alternates != expected_alternates:
        fail('hreflang must contain exactly one en, ru and x-default absolute URL')
    robots = {part.strip().lower() for part in meta('robots').split(',')}
    if robots != {'index', 'follow', 'max-image-preview:large'}:
        fail('robots must allow index, follow and max-image-preview:large')
    expected_image = ORIGIN + f'/assets/og-{language}.png'
    expected = {'og:type': 'website', 'og:site_name': 'OrcaSlicer Agent',
                'og:url': URLS[language], 'og:image': expected_image,
                'og:locale': 'en_US' if language == 'en' else 'ru_RU',
                'og:locale:alternate': 'ru_RU' if language == 'en' else 'en_US',
                'twitter:card': 'summary_large_image', 'twitter:image': expected_image}
    values = {name: meta(name) for name in (
        'og:title', 'og:description', 'twitter:title', 'twitter:description',
        'twitter:image:alt', *expected)}
    for name, value in expected.items():
        if values[name] != value:
            fail(f'{name} must be {value}')
    if page.titles and values['og:title'] != page.titles[0].strip():
        fail('og:title must match the localized title')
    if values['twitter:title'] != values['og:title']:
        fail('twitter:title must match og:title')
    if values['og:description'] != description or values['twitter:description'] != description:
        fail('social descriptions must match the localized meta description')
    for raw in page.jsonld:
        try:
            data = json.loads(raw)
        except (ValueError, TypeError) as exc:
            fail(f'invalid JSON-LD: {exc}')
            continue
        if not isinstance(data, dict):
            fail('JSON-LD must be a SoftwareApplication object')
            continue
        identity = {'@context': 'https://schema.org', '@type': 'SoftwareApplication',
                    'name': 'OrcaSlicer Agent', 'url': URLS[language],
                    'inLanguage': language, 'operatingSystem': 'macOS',
                    'applicationCategory': 'DesignApplication', 'isAccessibleForFree': True,
                    'license': 'https://www.gnu.org/licenses/agpl-3.0.html'}
        for name, value in identity.items():
            if data.get(name) != value:
                fail(f'JSON-LD {name} must be {value!r}')
        if data.get('description') != description:
            fail('JSON-LD description must match localized meta description')
        if any(name in data for name in ('offers', 'aggregateRating', 'review', 'publisher')):
            fail('JSON-LD must not invent pricing, ratings, reviews or publisher')


def check_crawl_files(errors):
    robots = ROOT / 'robots.txt'
    if not robots.is_file():
        errors.append('Missing robots.txt')
    else:
        directives = [line.split('#', 1)[0].strip() for line in robots.read_text().splitlines()]
        directives = [line for line in directives if line]
        if directives != ['User-agent: *', 'Allow: /', 'Sitemap: ' + ORIGIN + '/sitemap.xml']:
            errors.append('robots.txt must allow all crawlers and advertise the canonical sitemap')
    sitemap = ROOT / 'sitemap.xml'
    if not sitemap.is_file():
        errors.append('Missing sitemap.xml')
    else:
        ns = {'s': 'http://www.sitemaps.org/schemas/sitemap/0.9',
              'x': 'http://www.w3.org/1999/xhtml'}
        try:
            tree = ET.parse(sitemap).getroot()
            if tree.tag != '{' + ns['s'] + '}urlset':
                errors.append('Sitemap root must be a sitemap urlset')
            entries = tree.findall('s:url', ns)
            locations = [entry.findtext('s:loc', namespaces=ns) for entry in entries]
            if sorted(locations, key=str) != sorted(URLS.values()):
                errors.append('Sitemap must list each canonical page exactly once')
            for entry in entries:
                if len(entry.findall('s:loc', ns)) != 1:
                    errors.append('Every sitemap page needs exactly one canonical location')
                links = entry.findall('x:link', ns)
                actual = [(link.get('rel'), link.get('hreflang'), link.get('href')) for link in links]
                expected = [('alternate', key, value) for key, value in ALTERNATES.items()]
                if sorted(actual, key=str) != sorted(expected):
                    errors.append('Every sitemap page needs exact en, ru and x-default alternates')
        except ET.ParseError as exc:
            errors.append(f'Invalid sitemap XML: {exc}')
    for language in PAGES:
        image = ROOT / 'assets' / f'og-{language}.png'
        if not image.is_file():
            errors.append('Missing social image: ' + str(image))
            continue
        with image.open('rb') as stream:
            header = stream.read(24)
        if (len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n'
                or header[12:16] != b'IHDR' or struct.unpack('>II', header[16:24]) != (1200, 630)):
            errors.append(f'{image.name}: social image must be a 1200x630 PNG')


def main():
    errors, parsed = [], {}
    for language, filename in PAGES.items():
        if not filename.is_file():
            errors.append('Missing language page: ' + str(filename))
            continue
        page = Page()
        page.feed(filename.read_text())
        parsed[language] = page
        if page.lang != language:
            errors.append(f'{language}: incorrect HTML lang')
        if page.select_count:
            errors.append(f'{language}: client selector must not be present')
        if len(page.ids) != len(set(page.ids)):
            errors.append(f'{language}: duplicate HTML IDs')
        for target in page.copy_targets:
            if target not in page.ids:
                errors.append(f'{language}: missing copy target {target}')
        for href in page.links:
            if href.startswith('#') and href[1:] and href[1:] not in page.ids:
                errors.append(f'{language}: missing anchor {href}')
        for url in page.assets + page.links:
            target = local_target(filename, url)
            if target and not target.is_file():
                errors.append(f'{language}: missing local resource {url}')
        check_seo(language, page, errors)
        if language == 'en' and re.search('[А-Яа-яЁё]', ''.join(page.text)):
            errors.append('English default page contains untranslated Russian text')
    if set(parsed) == {'en', 'ru'}:
        for name in ('install-command', 'launch-command'):
            en = parsed['en'].copy_text.get(name, '').strip()
            ru = parsed['ru'].copy_text.get(name, '').strip()
            if not en or en != ru:
                errors.append('Commands differ between languages: ' + name)
        expected = './scripts/agent/install.sh install\n./scripts/agent/install.sh doctor'
        if parsed['en'].copy_text.get('install-command', '').strip() != expected:
            errors.append('Default manual install must use generic stdio configuration')
    for name in ('inter-regular.ttf', 'inter-semibold.ttf', 'nunito-extra-bold.ttf',
                 'inter-OFL.txt', 'nunito-OFL.txt', 'model.png', 'slice.png'):
        if not (ROOT / 'assets' / name).is_file():
            errors.append('Missing bundled font, license or preview: ' + name)
    check_crawl_files(errors)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('PASS: English default and Russian page; assets, language links, anchors, '
          'copy targets and identical generic commands; no client selector; '
          'canonical/hreflang, social metadata, JSON-LD, robots, sitemap and social images')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
