import fs from 'node:fs/promises'
import path from 'node:path'

const root = path.resolve(new URL('..', import.meta.url).pathname)
const repoRoot = path.resolve(root, '..')
const docsRoot = process.env.ARBITER_DOCS_PATH
  ? path.resolve(process.env.ARBITER_DOCS_PATH)
  : path.join(repoRoot, 'docs')
const dist = path.join(root, 'dist')
const stylesPath = path.join(root, 'src', 'styles.css')
const installPath = path.join(root, 'install.sh')
const assetsPath = path.join(repoRoot, 'assets')

const sectionOrder = [
  'getting-started',
  'concepts',
  'cli',
  'tui',
  'api',
  'philosophy',
]

const sectionLabels = {
  'getting-started': 'Getting Started',
  concepts: 'Concepts',
  cli: 'CLI',
  tui: 'TUI',
  api: 'API Reference',
  philosophy: 'Philosophy',
}

await fs.rm(dist, { force: true, recursive: true })
await fs.mkdir(dist, { recursive: true })
await fs.copyFile(stylesPath, path.join(dist, 'styles.css'))
await fs.copyFile(installPath, path.join(dist, 'install.sh'))
await copyAssets()

const docs = await readDocs(docsRoot)
const sortedDocs = sortDocs(docs)

await writeFile('index.html', renderHome(sortedDocs))
await writeFile('docs/index.html', renderDocsIndex(sortedDocs))

for (let index = 0; index < sortedDocs.length; index += 1) {
  const doc = sortedDocs[index]
  const prev = sortedDocs[index - 1]
  const next = sortedDocs[index + 1]
  await writeFile(
    doc.outputPath,
    renderDocPage({
      doc,
      docs: sortedDocs,
      html: markdownToHtml(doc.source, doc),
      next,
      prev,
    }),
  )
}

await writeFile('robots.txt', 'User-agent: *\nAllow: /\nSitemap: /sitemap.xml\n')
await writeFile('sitemap.xml', renderSitemap(sortedDocs))

console.log(`Built ${sortedDocs.length + 2} pages into ${path.relative(repoRoot, dist)}`)

async function writeFile(relativePath, contents) {
  const target = path.join(dist, relativePath)
  await fs.mkdir(path.dirname(target), { recursive: true })
  await fs.writeFile(target, contents)
}

async function readDocs(base) {
  const files = await walk(base)
  const markdownFiles = files.filter((file) => file.endsWith('.md'))
  const docs = []

  for (const file of markdownFiles) {
    const relative = slash(path.relative(base, file))
    const source = await fs.readFile(file, 'utf8')
    const title = extractTitle(source) ?? titleFromSlug(relative)
    const slug = relative.replace(/\.md$/, '').replace(/\/index$/, '')
    const section = relative.includes('/') ? relative.split('/')[0] : slug
    const href = `/docs/${slug}/`
    docs.push({
      href,
      outputPath: `docs/${slug}/index.html`,
      relative,
      section,
      source,
      title,
    })
  }

  return docs
}

async function walk(dir) {
  const entries = await fs.readdir(dir, { withFileTypes: true })
  const results = []
  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      results.push(...(await walk(fullPath)))
    } else if (entry.isFile()) {
      results.push(fullPath)
    }
  }
  return results
}

function sortDocs(docs) {
  return [...docs].sort((a, b) => {
    const sectionDiff = sectionRank(a.section) - sectionRank(b.section)
    if (sectionDiff !== 0) return sectionDiff
    const aIndex = a.relative.endsWith('/index.md') || a.relative === 'index.md'
    const bIndex = b.relative.endsWith('/index.md') || b.relative === 'index.md'
    if (aIndex !== bIndex) return aIndex ? -1 : 1
    return a.relative.localeCompare(b.relative)
  })
}

function sectionRank(section) {
  const index = sectionOrder.indexOf(section)
  return index === -1 ? 999 : index
}

function renderHome(docs) {
  const gettingStarted = docs.find((doc) => doc.relative === 'getting-started/index.md')
  const api = docs.find((doc) => doc.relative === 'api/index.md')
  const philosophy = docs.find((doc) => doc.relative === 'philosophy.md')

  return layout({
    title: 'Arbiter - Self-hosted agent runtime for events and operations',
    description:
      'Arbiter is a self-hosted agent runtime that turns prompts, webhooks, and device events into supervised, stateful work with durable memory and observable streams.',
    body: `
      <section class="hero">
        <div class="hero-inner">
          <div class="hero-copy-panel">
            <p class="eyebrow">Self-hosted agent runtime</p>
            <h1>The agent that runs anywhere.</h1>
            <p class="hero-copy">Arbiter is one small binary for laptops, servers, CI runners, edge boxes, and the systems around them. Use the TUI, one-shot CLI, HTTP API, or event router to turn real work into supervised agent runs.</p>
          </div>
          <div class="install-callout" aria-label="Install command">
            <span>Install Arbiter</span>
            <code>curl -fsSL https://arbiter.run/install.sh | sh</code>
          </div>
          ${terminalScene()}
          <div class="hero-points">
            <div class="point"><strong>Route</strong><span>Map prompts, webhooks, incidents, schedules, and hardware events to the right agent.</span></div>
            <div class="point"><strong>Supervise</strong><span>Constrain tools and require advisor review before consequential work leaves the runtime.</span></div>
            <div class="point"><strong>Replay</strong><span>Persist request streams so clients can reconnect, audit, or tail in-flight work.</span></div>
          </div>
        </div>
      </section>

      <section class="section" id="platform">
        <div class="section-inner">
          <h2>The control plane for agentic operations.</h2>
          <p class="section-lede">Coders need IDE agents. People need chat assistants. Systems need a runtime that can receive events, choose an agent, execute tools under policy, and leave an auditable trail.</p>
          <div class="feature-grid">
            <article class="feature">
              <h3>For operators</h3>
              <p>Route production incidents, scheduled checks, queue messages, and service webhooks into supervised agent runs.</p>
            </article>
            <article class="feature">
              <h3>For developers</h3>
              <p>Drive the same agents from the TUI, one-shot CLI, HTTP API, or Agent2Agent clients without changing the runtime.</p>
            </article>
            <article class="feature">
              <h3>For devices</h3>
              <p>Let sensors and edge machines emit structured events while Arbiter handles routing, memory, tools, and final action.</p>
            </article>
          </div>
        </div>
      </section>

      <section class="section section-contrast" id="workflow">
        <div class="section-inner split-section">
          <div>
            <p class="eyebrow">How it works</p>
            <h2>One event stream from request to result.</h2>
            <p class="section-lede">Arbiter makes the invisible parts of agent work visible: routing, delegation, tool calls, memory reads, advisor gates, artifacts, token use, and terminal status.</p>
          </div>
          <div class="steps">
            <div class="step"><span>1</span><strong>Receive</strong><p>Accept a prompt, CLI send, HTTP request, webhook, schedule, or device event.</p></div>
            <div class="step"><span>2</span><strong>Route</strong><p>Match the work to an agent constitution with explicit tools, memory, and event patterns.</p></div>
            <div class="step"><span>3</span><strong>Act</strong><p>Execute writ commands inline while advisor gates review consequential decisions.</p></div>
            <div class="step"><span>4</span><strong>Observe</strong><p>Stream and persist every event so clients can reconnect, inspect, and audit outcomes.</p></div>
          </div>
        </div>
      </section>

      <section class="section" id="positioning">
        <div class="section-inner">
          <h2>Built for the space between copilots and cron jobs.</h2>
          <p class="section-lede">Arbiter is not trying to be your editor, your hosted assistant, or a chat app. It is the runtime layer underneath agentic workflows that need local control and operational visibility.</p>
          <div class="feature-grid">
            <article class="feature">
              <h3>Self-hosted by default</h3>
              <p>Your binary, your machine, your tenant database, your agent constitutions, and your provider keys.</p>
            </article>
            <article class="feature">
              <h3>Policy in the loop</h3>
              <p>Tool access is an allowlist. Advisor gates are structural checks, not another prompt asking the same model to self-grade.</p>
            </article>
            <article class="feature">
              <h3>Interfaces are optional</h3>
              <p>Use the terminal, script it, embed the API, federate over A2A, or let events arrive from systems that never show a UI.</p>
            </article>
          </div>
        </div>
      </section>

      <section class="section" id="docs">
        <div class="section-inner">
          <h2>Documentation that ships with the code.</h2>
          <p class="section-lede">This site ingests the repository Markdown directly, preserving the docs authoring flow while publishing static HTML.</p>
          <div class="flow">
            <a class="flow-row" href="${gettingStarted?.href ?? '/docs/'}"><code>getting-started</code><span>Install locally, configure providers, and get the first agent reply.</span></a>
            <a class="flow-row" href="${philosophy?.href ?? '/docs/'}"><code>philosophy</code><span>The design themes behind Arbiter's runtime, memory, and supervision model.</span></a>
            <a class="flow-row" href="${api?.href ?? '/docs/'}"><code>api</code><span>HTTP, SSE, tenants, requests, conversations, schedules, agents, and A2A endpoints.</span></a>
          </div>
        </div>
      </section>
    `,
  })
}

function terminalScene() {
  return `
    <img class="terminal-image" src="/assets/terminal.jpg" alt="Arbiter terminal interface with sessions and inline diff rendering">
  `
}

async function copyAssets() {
  try {
    await fs.access(assetsPath)
  } catch {
    return
  }
  await fs.cp(assetsPath, path.join(dist, 'assets'), {
    recursive: true,
    filter: (source) => /\.(gif|jpe?g|png|webp|svg)$/i.test(source) || source === assetsPath,
  })
}

function renderDocsIndex(docs) {
  const groups = groupDocs(docs)
  const cards = [...groups.entries()]
    .map(([section, sectionDocs]) => {
      const first = sectionDocs[0]
      return `
        <a class="flow-row" href="${first.href}">
          <code>${escapeHtml(sectionLabels[section] ?? titleCase(section))}</code>
          <span>${sectionDocs.length} page${sectionDocs.length === 1 ? '' : 's'} covering ${sectionDocs
            .slice(0, 4)
            .map((doc) => doc.title)
            .join(', ')}.</span>
        </a>
      `
    })
    .join('')

  return layout({
    title: 'Arbiter Documentation',
    description: 'Static documentation for the Arbiter reasoning runtime.',
    body: `
      <section class="section">
        <div class="section-inner">
          <p class="eyebrow">Documentation</p>
          <h1>Build, operate, and extend Arbiter.</h1>
          <p class="hero-copy">These pages are generated from the repository Markdown so implementation and documentation evolve together.</p>
          <div class="flow">${cards}</div>
        </div>
      </section>
    `,
  })
}

function renderDocPage({ doc, docs, html, next, prev }) {
  const nav = renderDocsSidebar(docs, doc)
  const breadcrumbs = `${escapeHtml(sectionLabels[doc.section] ?? titleCase(doc.section))} / ${escapeHtml(doc.title)}`
  return layout({
    title: `${doc.title} - Arbiter Docs`,
    description: `Arbiter documentation: ${doc.title}.`,
    body: `
      <div class="docs-layout">
        ${nav}
        <main class="docs-main">
          <article class="doc-shell">
            <div class="breadcrumbs">${breadcrumbs}</div>
            <div class="doc-content">${html}</div>
            <nav class="doc-nav" aria-label="Documentation pagination">
              ${prev ? `<a href="${prev.href}"><span>Previous</span>${escapeHtml(prev.title)}</a>` : '<span></span>'}
              ${next ? `<a href="${next.href}"><span>Next</span>${escapeHtml(next.title)}</a>` : '<span></span>'}
            </nav>
          </article>
        </main>
      </div>
    `,
  })
}

function renderDocsSidebar(docs, activeDoc) {
  const groups = groupDocs(docs)
  const body = [...groups.entries()]
    .map(([section, sectionDocs]) => {
      const links = sectionDocs
        .map((doc) => {
          const active = doc.href === activeDoc.href ? ' active' : ''
          return `<a class="docs-link${active}" href="${doc.href}">${escapeHtml(doc.title)}</a>`
        })
        .join('')
      return `<div class="docs-group"><p class="docs-group-title">${escapeHtml(
        sectionLabels[section] ?? titleCase(section),
      )}</p>${links}</div>`
    })
    .join('')

  return `<aside class="docs-sidebar"><h2>Docs</h2>${body}</aside>`
}

function groupDocs(docs) {
  const groups = new Map()
  for (const doc of docs) {
    const group = groups.get(doc.section) ?? []
    group.push(doc)
    groups.set(doc.section, group)
  }
  return groups
}

function layout({ title, description, body }) {
  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="description" content="${escapeHtml(description)}">
    <title>${escapeHtml(title)}</title>
    <link rel="stylesheet" href="/styles.css">
  </head>
  <body>
    <header class="site-header">
      <nav class="nav" aria-label="Main navigation">
        <a class="brand" href="/"><span class="brand-mark">⛮</span></a>
        <div class="nav-links">
          <a href="/#platform">Platform</a>
          <a href="/#workflow">Workflow</a>
          <a href="/docs/">Docs</a>
          <a href="https://github.com/tylerreckart/arbiter">GitHub</a>
          <a class="button" href="/docs/getting-started/">Start</a>
        </div>
      </nav>
    </header>
    ${body}
    <footer class="site-footer">
      <div class="site-footer-inner">
        <span>Arbiter is experimental open-source infrastructure.</span>
        <a href="/docs/philosophy/">Philosophy</a>
      </div>
    </footer>
  </body>
</html>
`
}

function markdownToHtml(markdown, doc) {
  const lines = markdown.replace(/\r\n/g, '\n').split('\n')
  const html = []
  let index = 0

  while (index < lines.length) {
    const line = lines[index]

    if (!line.trim()) {
      index += 1
      continue
    }

    const fence = line.match(/^```(\w+)?\s*$/)
    if (fence) {
      const language = fence[1] ? ` class="language-${escapeHtml(fence[1])}"` : ''
      const code = []
      index += 1
      while (index < lines.length && !lines[index].startsWith('```')) {
        code.push(lines[index])
        index += 1
      }
      index += 1
      html.push(`<pre><code${language}>${escapeHtml(code.join('\n'))}</code></pre>`)
      continue
    }

    const heading = line.match(/^(#{1,4})\s+(.+)$/)
    if (heading) {
      const level = heading[1].length
      const text = stripHashes(heading[2])
      const id = slugify(text)
      html.push(`<h${level} id="${id}">${inline(text, doc)}</h${level}>`)
      index += 1
      continue
    }

    if (isTableStart(lines, index)) {
      const tableLines = []
      while (index < lines.length && lines[index].trim().startsWith('|')) {
        tableLines.push(lines[index])
        index += 1
      }
      html.push(renderTable(tableLines, doc))
      continue
    }

    if (/^\s*[-*]\s+/.test(line)) {
      const items = []
      while (index < lines.length && /^\s*[-*]\s+/.test(lines[index])) {
        items.push(lines[index].replace(/^\s*[-*]\s+/, ''))
        index += 1
      }
      html.push(`<ul>${items.map((item) => `<li>${inline(item, doc)}</li>`).join('')}</ul>`)
      continue
    }

    if (/^\s*\d+\.\s+/.test(line)) {
      const items = []
      while (index < lines.length && /^\s*\d+\.\s+/.test(lines[index])) {
        items.push(lines[index].replace(/^\s*\d+\.\s+/, ''))
        index += 1
      }
      html.push(`<ol>${items.map((item) => `<li>${inline(item, doc)}</li>`).join('')}</ol>`)
      continue
    }

    if (/^>\s+/.test(line)) {
      const quote = []
      while (index < lines.length && /^>\s?/.test(lines[index])) {
        quote.push(lines[index].replace(/^>\s?/, ''))
        index += 1
      }
      html.push(`<blockquote><p>${inline(quote.join(' '), doc)}</p></blockquote>`)
      continue
    }

    const paragraph = [line.trim()]
    index += 1
    while (
      index < lines.length &&
      lines[index].trim() &&
      !/^(#{1,4})\s+/.test(lines[index]) &&
      !/^```/.test(lines[index]) &&
      !/^\s*[-*]\s+/.test(lines[index]) &&
      !/^\s*\d+\.\s+/.test(lines[index]) &&
      !/^>\s?/.test(lines[index]) &&
      !isTableStart(lines, index)
    ) {
      paragraph.push(lines[index].trim())
      index += 1
    }
    html.push(`<p>${inline(paragraph.join(' '), doc)}</p>`)
  }

  return html.join('\n')
}

function renderTable(lines, doc) {
  const rows = lines
    .filter((line, index) => index !== 1)
    .map((line) =>
      line
        .trim()
        .replace(/^\||\|$/g, '')
        .split('|')
        .map((cell) => cell.trim()),
    )

  const [head = [], ...body] = rows
  return `<table><thead><tr>${head
    .map((cell) => `<th>${inline(cell, doc)}</th>`)
    .join('')}</tr></thead><tbody>${body
    .map((row) => `<tr>${row.map((cell) => `<td>${inline(cell, doc)}</td>`).join('')}</tr>`)
    .join('')}</tbody></table>`
}

function isTableStart(lines, index) {
  return (
    lines[index]?.trim().startsWith('|') &&
    /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(lines[index + 1] ?? '')
  )
}

function inline(value, doc) {
  const tokens = []
  let text = escapeHtml(value)

  text = text.replace(/`([^`]+)`/g, (_, code) => {
    const token = `@@CODE${tokens.length}@@`
    tokens.push(`<code>${code}</code>`)
    return token
  })

  text = text.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, href) => {
    return `<a href="${escapeAttribute(resolveHref(href, doc))}">${label}</a>`
  })
  text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
  text = text.replace(/\*([^*]+)\*/g, '<em>$1</em>')

  for (let index = 0; index < tokens.length; index += 1) {
    text = text.replace(`@@CODE${index}@@`, tokens[index])
  }
  return text
}

function resolveHref(href, doc) {
  if (/^(https?:|mailto:|#)/.test(href)) return href
  if (href.endsWith('.md') || href.includes('.md#')) {
    const [filePart, hash] = href.split('#')
    const sourceDir = path.posix.dirname(doc.relative)
    const target = path.posix.normalize(path.posix.join(sourceDir, filePart))
    const slug = target.replace(/\.md$/, '').replace(/\/index$/, '')
    return `/docs/${slug}/${hash ? `#${hash}` : ''}`
  }
  return href
}

function extractTitle(markdown) {
  return markdown.match(/^#\s+(.+)$/m)?.[1]?.trim()
}

function titleFromSlug(relative) {
  const base = relative.replace(/\.md$/, '').split('/').pop()
  return titleCase(base)
}

function titleCase(value) {
  return value
    .replace(/[-_]/g, ' ')
    .replace(/\b\w/g, (letter) => letter.toUpperCase())
}

function slugify(value) {
  return value
    .toLowerCase()
    .replace(/`/g, '')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-|-$/g, '')
}

function stripHashes(value) {
  return value.replace(/\s+#*$/, '')
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

function escapeAttribute(value) {
  return escapeHtml(value).replace(/'/g, '&#39;')
}

function slash(value) {
  return value.split(path.sep).join('/')
}

function renderSitemap(docs) {
  const urls = ['/', '/docs/', ...docs.map((doc) => doc.href)]
  return `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
${urls.map((url) => `  <url><loc>https://arbiter.run${url}</loc></url>`).join('\n')}
</urlset>
`
}
