import assert from 'node:assert/strict'
import test from 'node:test'
import { extractToc, markdownToHtml } from '../lib/markdown.mjs'

const doc = { relative: 'concepts/example.md', href: '/docs/concepts/example/' }

test('mermaid fence becomes pre.mermaid with escaped content', () => {
  const html = markdownToHtml('```mermaid\ngraph TD\n  A["x < y"] --> B\n```', doc)
  assert.match(html, /<pre class="mermaid">/)
  assert.match(html, /&lt;/)
  assert.doesNotMatch(html, /<code/)
  assert.doesNotMatch(html, /x < y/)
})

test('non-mermaid fence keeps language class', () => {
  const html = markdownToHtml('```bash\necho hi\n```', doc)
  assert.match(html, /<pre><code class="language-bash">echo hi<\/code><\/pre>/)
  assert.doesNotMatch(html, /class="mermaid"/)
})

test('extractToc ignores headings inside fences', () => {
  const markdown = [
    '# Title',
    '',
    '## Real heading',
    '',
    '```mermaid',
    'flowchart LR',
    '  A --> B',
    '## Not a TOC heading',
    '```',
    '',
    '### Nested real',
    '',
    '```bash',
    '## Also ignored',
    'echo ok',
    '```',
  ].join('\n')

  const toc = extractToc(markdown)
  assert.deepEqual(
    toc.map((entry) => entry.text),
    ['Real heading', 'Nested real'],
  )
})

test('extractToc matches markdownToHtml for indented fence-like lines', () => {
  const markdown = [
    '## Outside',
    '',
    '  ```not-a-fence',
    '## Still a heading',
    '  ```',
    '',
    '## Also outside',
  ].join('\n')

  const toc = extractToc(markdown).map((entry) => entry.text)
  const htmlHeadings = [...markdownToHtml(markdown, doc).matchAll(/<h[23][^>]*>([^<]+)/g)].map(
    (match) => match[1],
  )

  assert.deepEqual(toc, ['Outside', 'Still a heading', 'Also outside'])
  assert.deepEqual(toc, htmlHeadings)
})

test('extractToc matches markdownToHtml when fence closer is indented', () => {
  const markdown = [
    '## Before',
    '```bash',
    'echo hi',
    '  ```',
    '## Inside code in HTML',
    '```',
    '## After',
  ].join('\n')

  const toc = extractToc(markdown).map((entry) => entry.text)
  const htmlHeadings = [...markdownToHtml(markdown, doc).matchAll(/<h[23][^>]*>([^<]+)/g)].map(
    (match) => match[1],
  )

  assert.deepEqual(toc, ['Before', 'After'])
  assert.deepEqual(toc, htmlHeadings)
})
