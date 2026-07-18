import fs from 'node:fs/promises'
import path from 'node:path'

const root = path.resolve(new URL('..', import.meta.url).pathname)
const repoRoot = path.resolve(root, '..')
const docsRoot = process.env.ARBITER_DOCS_PATH
  ? path.resolve(process.env.ARBITER_DOCS_PATH)
  : path.join(repoRoot, 'docs')
const dist = path.join(root, 'dist')
const stylesPath = path.join(root, 'src', 'styles.css')
const siteScriptPath = path.join(root, 'src', 'site.js')
const installPath = path.join(root, 'install.sh')
const assetsPath = path.join(repoRoot, 'assets')
const siteOrigin = 'https://arbiter.run'
const githubBlobBase = 'https://github.com/tylerreckart/arbiter/blob/main'

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

const assetAllowlist = new Set(['terminal.jpg', 'themes.jpg', 'terminal_crop.jpg', 'themes_crop.jpg'])

await fs.rm(dist, { force: true, recursive: true })
await fs.mkdir(dist, { recursive: true })
await fs.copyFile(stylesPath, path.join(dist, 'styles.css'))
await fs.copyFile(siteScriptPath, path.join(dist, 'site.js'))
await fs.copyFile(installPath, path.join(dist, 'install.sh'))
await copyAssets()
await writeFavicon()

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

await writeFile('robots.txt', `User-agent: *\nAllow: /\nSitemap: ${siteOrigin}/sitemap.xml\n`)
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
    const description = extractDescription(source, title)
    const slug = relative.replace(/\.md$/, '').replace(/\/index$/, '')
    const section = relative.includes('/') ? relative.split('/')[0] : slug
    const href = `/docs/${slug}/`
    docs.push({
      description,
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
  const installCommand = 'curl -fsSL https://arbiter.run/install.sh | sh'

  return layout({
    title: 'Arbiter — the agent that runs anywhere',
    description:
      'Arbiter is a self-hosted agent runtime that turns prompts, webhooks, and device events into supervised, stateful work with durable memory and observable streams.',
    canonicalPath: '/',
    ogImage: '/assets/terminal.jpg',
    body: `
      <section class="hero">
        <div class="hero-inner">
          <p class="brand-lockup">Arbiter</p>
          <h1 class="hero-title">The agent that runs anywhere.</h1>
          <p class="hero-lede">One small binary for laptops, servers, CI, and edge — supervised agents with durable memory and a live event stream.</p>
          <div class="install-callout" aria-label="Install command">
            <span>Install</span>
            <code>${escapeHtml(installCommand)}</code>
            <button class="button" type="button" data-copy-install="${escapeAttribute(installCommand)}">Copy</button>
          </div>
        </div>
      </section>

      <section class="product-stage reveal" id="product">
        <div class="product-stage-inner">
          <p class="eyebrow">Terminal interface</p>
          <h2>Multi-pane sessions. Inline diffs. Live streams.</h2>
          <div class="product-frame">
            <img src="/assets/terminal.jpg" alt="Arbiter terminal interface with sessions and inline diff rendering" width="1600" height="1000">
            <div class="product-meta">
              <span>arbiter — interactive TUI</span>
              <span>same runtime as CLI + HTTP API</span>
            </div>
          </div>
        </div>
      </section>

      <section class="section reveal" id="platform">
        <div class="section-inner">
          <p class="eyebrow">Platform</p>
          <h2>Route. Supervise. Replay.</h2>
          <p class="section-lede">Map real events to the right agent, constrain tools with advisor gates, and keep every stream reconnectable.</p>
          <div class="feature-rail">
            <article class="feature">
              <h3>Route</h3>
              <p>Prompts, webhooks, incidents, schedules, and hardware events land on agents with explicit constitutions.</p>
            </article>
            <article class="feature">
              <h3>Supervise</h3>
              <p>Tool access is an allowlist. Advisor gates review consequential turns before work leaves the runtime.</p>
            </article>
            <article class="feature">
              <h3>Replay</h3>
              <p>Persist request streams so clients can reconnect, audit, or tail in-flight work without losing context.</p>
            </article>
          </div>
        </div>
      </section>

      <section class="section section-band reveal" id="workflow">
        <div class="section-inner split-section">
          <div>
            <p class="eyebrow">How it works</p>
            <h2>One event stream from request to result.</h2>
            <p class="section-lede">Routing, delegation, tool calls, memory reads, advisor gates, and token use stay visible as structured events.</p>
          </div>
          <div class="steps">
            <div class="step"><span>01</span><strong>Receive</strong><p>Accept a prompt, CLI send, HTTP request, webhook, schedule, or device event.</p></div>
            <div class="step"><span>02</span><strong>Route</strong><p>Match the work to an agent constitution with explicit tools, memory, and event patterns.</p></div>
            <div class="step"><span>03</span><strong>Act</strong><p>Execute writ commands inline while advisor gates review consequential decisions.</p></div>
            <div class="step"><span>04</span><strong>Observe</strong><p>Stream and persist every event so clients can reconnect, inspect, and audit outcomes.</p></div>
          </div>
        </div>
      </section>

      <section class="section reveal" id="surfaces">
        <div class="section-inner">
          <p class="eyebrow">Surfaces</p>
          <h2>Same runtime. Optional interfaces.</h2>
          <p class="section-lede">Drive agents from the TUI, one-shot CLI, HTTP API, or Agent2Agent clients without changing the binary underneath.</p>
          <div class="feature-rail">
            <article class="feature">
              <h3>Operators</h3>
              <p>Route production incidents, scheduled checks, queue messages, and service webhooks into supervised runs.</p>
            </article>
            <article class="feature">
              <h3>Developers</h3>
              <p>Script one-shots, embed the API, or keep a multi-pane TUI open on the machine that owns the work.</p>
            </article>
            <article class="feature">
              <h3>Devices</h3>
              <p>Let sensors and edge machines emit structured events while Arbiter handles routing, memory, and action.</p>
            </article>
          </div>
        </div>
      </section>

      <section class="section section-band reveal" id="themes">
        <div class="section-inner">
          <p class="eyebrow">Themeable</p>
          <h2>No color compiled into the binary.</h2>
          <p class="section-lede">The TUI reads a JSON theme at startup. Presets ship in-tree; save your own look without rebuilding.</p>
          <div class="themes-panel">
            <img src="/assets/themes.jpg" alt="Arbiter TUI theme gallery across multiple color palettes" width="1600" height="900" loading="lazy">
          </div>
        </div>
      </section>

      <section class="section reveal" id="docs">
        <div class="section-inner">
          <p class="eyebrow">Documentation</p>
          <h2>Docs that ship with the code.</h2>
          <p class="section-lede">This site ingests repository Markdown directly, so implementation and documentation stay on the same path.</p>
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

async function copyAssets() {
  try {
    await fs.access(assetsPath)
  } catch {
    return
  }

  const targetDir = path.join(dist, 'assets')
  await fs.mkdir(targetDir, { recursive: true })
  const entries = await fs.readdir(assetsPath, { withFileTypes: true })
  for (const entry of entries) {
    if (!entry.isFile()) continue
    if (!assetAllowlist.has(entry.name)) continue
    await fs.copyFile(path.join(assetsPath, entry.name), path.join(targetDir, entry.name))
  }
}

async function writeFavicon() {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img">
  <rect width="64" height="64" fill="#0c0c0c"/>
  <rect x="12" y="12" width="40" height="40" fill="#b8ff3c"/>
  <text x="32" y="41" text-anchor="middle" font-family="ui-monospace, monospace" font-size="26" fill="#0c0c0c">⛮</text>
</svg>`
  await writeFile('favicon.svg', svg)
}

function renderDocsIndex(docs) {
  const groups = groupDocs(docs)
  const cards = [...groups.entries()]
    .map(([section, sectionDocs]) => {
      const indexDoc = sectionDocs.find((doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`)
      const first = indexDoc ?? sectionDocs[0]
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
    description: 'Install, operate, and extend the Arbiter self-hosted agent runtime.',
    canonicalPath: '/docs/',
    variant: 'docs',
    body: `
      <div class="docs-shell">
        ${renderDocsSidebar(docs, { href: '/docs/', section: 'getting-started' })}
        <main class="docs-main" id="main">
          <div class="docs-main-inner">
            <div class="doc-shell docs-index-inner">
              <p class="eyebrow">Documentation</p>
              <h1>Build, operate, and extend Arbiter.</h1>
              <p class="lead">Generated from repository Markdown so implementation and documentation stay on the same path.</p>
              <div class="flow">${cards}</div>
            </div>
          </div>
        </main>
      </div>
    `,
  })
}

function renderDocPage({ doc, docs, html, next, prev }) {
  const toc = extractToc(doc.source)
  const breadcrumbs = `${escapeHtml(sectionLabels[doc.section] ?? titleCase(doc.section))} / ${escapeHtml(doc.title)}`
  return layout({
    title: `${doc.title} — Arbiter Docs`,
    description: doc.description,
    canonicalPath: doc.href,
    variant: 'docs',
    body: `
      <div class="docs-shell">
        ${renderDocsSidebar(docs, doc)}
        <main class="docs-main" id="main">
          <div class="docs-main-inner">
            <article class="doc-shell">
              <div class="breadcrumbs">${breadcrumbs}</div>
              <div class="doc-content">${html}</div>
              <nav class="doc-nav" aria-label="Documentation pagination">
                ${prev ? `<a href="${prev.href}"><span>Previous</span>${escapeHtml(prev.title)}</a>` : '<span></span>'}
                ${next ? `<a href="${next.href}"><span>Next</span>${escapeHtml(next.title)}</a>` : '<span></span>'}
              </nav>
            </article>
            ${renderToc(toc)}
          </div>
        </main>
      </div>
    `,
  })
}

function extractToc(markdown) {
  const toc = []
  for (const line of markdown.replace(/\r\n/g, '\n').split('\n')) {
    const match = line.match(/^(#{2,3})\s+(.+)$/)
    if (!match) continue
    const text = stripHashes(match[2])
    toc.push({
      id: slugify(text),
      level: match[1].length,
      text,
    })
  }
  return toc
}

function renderToc(toc) {
  if (toc.length < 3) return ''
  const links = toc
    .map((entry) => `<a href="#${escapeAttribute(entry.id)}">${escapeHtml(entry.text)}</a>`)
    .join('')
  return `<nav class="doc-toc" aria-label="On this page"><p>On this page</p>${links}</nav>`
}

function renderDocsSidebar(docs, activeDoc) {
  const groups = groupDocs(docs)
  const tabs = [...groups.keys()]
    .map((section) => {
      const active = section === activeDoc.section ? ' active' : ''
      const sectionDocs = groups.get(section) ?? []
      const indexDoc = sectionDocs.find((doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`)
      const href = indexDoc?.href ?? sectionDocs[0]?.href ?? '/docs/'
      return `<a class="${active.trim()}" href="${href}">${escapeHtml(sectionLabels[section] ?? titleCase(section))}</a>`
    })
    .join('')

  const activeSection = activeDoc.section
  const sectionDocs = groups.get(activeSection) ?? []
  let body = ''

  if (activeSection === 'api') {
    body = renderApiSidebarGroup(sectionDocs, activeDoc)
  } else if (sectionDocs.length > 0) {
    const links = sectionDocs
      .map((doc) => {
        const active = doc.href === activeDoc.href ? ' active' : ''
        const label = doc.relative.endsWith('/index.md') ? 'Overview' : doc.title
        return `<a class="docs-link${active}" href="${doc.href}">${escapeHtml(label)}</a>`
      })
      .join('')
    body = `<div class="docs-group"><p class="docs-group-title">${escapeHtml(
      sectionLabels[activeSection] ?? titleCase(activeSection),
    )}</p>${links}</div>`
  } else {
    body = [...groups.entries()]
      .map(([section, docsInSection]) => {
        const links = docsInSection
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
  }

  return `<aside class="docs-sidebar" aria-label="Documentation">
    <div class="docs-side-head">
      <h2>Docs</h2>
      <div class="docs-section-tabs">${tabs}</div>
    </div>
    <div class="docs-nav">${body}</div>
  </aside>`
}

function renderApiSidebarGroup(sectionDocs, activeDoc) {
  const subgroups = new Map()
  const topLevel = []

  for (const doc of sectionDocs) {
    const parts = doc.relative.replace(/^api\//, '').split('/')
    if (parts.length === 1) {
      topLevel.push(doc)
      continue
    }
    const key = parts[0]
    const list = subgroups.get(key) ?? []
    list.push(doc)
    subgroups.set(key, list)
  }

  const topLinks = topLevel
    .map((doc) => {
      const active = doc.href === activeDoc.href ? ' active' : ''
      const label = doc.relative === 'api/index.md' ? 'Overview' : doc.title
      return `<a class="docs-link${active}" href="${doc.href}">${escapeHtml(label)}</a>`
    })
    .join('')

  const nested = [...subgroups.entries()]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([key, docsInGroup]) => {
      const open = docsInGroup.some((doc) => doc.href === activeDoc.href) ? ' open' : ''
      const links = docsInGroup
        .map((doc) => {
          const active = doc.href === activeDoc.href ? ' active' : ''
          return `<a class="docs-link${active}" href="${doc.href}">${escapeHtml(shortApiTitle(doc))}</a>`
        })
        .join('')
      return `<details class="docs-subgroup"${open}><summary>${escapeHtml(titleCase(key))}</summary><div class="docs-subgroup-body">${links}</div></details>`
    })
    .join('')

  return `<div class="docs-group"><p class="docs-group-title">API Reference</p>${topLinks}${nested}</div>`
}

function shortApiTitle(doc) {
  const leaf = doc.relative.split('/').pop().replace(/\.md$/, '')
  if (leaf === 'index') return 'Overview'
  return titleCase(leaf)
}

function groupDocs(docs) {
  const groups = new Map()
  for (const section of sectionOrder) groups.set(section, [])
  for (const doc of docs) {
    const group = groups.get(doc.section) ?? []
    group.push(doc)
    groups.set(doc.section, group)
  }
  for (const [section, sectionDocs] of groups) {
    if (sectionDocs.length === 0) groups.delete(section)
  }
  return groups
}

function layout({
  title,
  description,
  body,
  canonicalPath = '/',
  ogImage = '/assets/terminal.jpg',
  variant = 'marketing',
}) {
  const canonical = `${siteOrigin}${canonicalPath}`
  const bodyClass = variant === 'docs' ? ' class="docs"' : ''
  const themeColor = variant === 'docs' ? '#0c0c0c' : '#ffffff'
  const navLinks =
    variant === 'docs'
      ? `
          <a href="/">Home</a>
          <a href="/docs/">Docs</a>
          <a href="https://github.com/tylerreckart/arbiter">GitHub</a>
          <a class="button" href="/docs/getting-started/">Start</a>`
      : `
          <a href="/#product">Product</a>
          <a href="/#platform">Platform</a>
          <a href="/docs/">Docs</a>
          <a href="https://github.com/tylerreckart/arbiter">GitHub</a>
          <a class="button" href="/docs/getting-started/">Start</a>`

  return `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="description" content="${escapeHtml(description)}">
    <meta name="theme-color" content="${themeColor}">
    <link rel="canonical" href="${escapeAttribute(canonical)}">
    <meta property="og:type" content="website">
    <meta property="og:site_name" content="Arbiter">
    <meta property="og:title" content="${escapeHtml(title)}">
    <meta property="og:description" content="${escapeHtml(description)}">
    <meta property="og:url" content="${escapeAttribute(canonical)}">
    <meta property="og:image" content="${escapeAttribute(`${siteOrigin}${ogImage}`)}">
    <meta name="twitter:card" content="summary_large_image">
    <meta name="twitter:title" content="${escapeHtml(title)}">
    <meta name="twitter:description" content="${escapeHtml(description)}">
    <meta name="twitter:image" content="${escapeAttribute(`${siteOrigin}${ogImage}`)}">
    <title>${escapeHtml(title)}</title>
    <link rel="icon" href="/favicon.svg" type="image/svg+xml">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;600;700&family=Public+Sans:wght@400;500;600;700&family=Syne:wght@700;800&display=swap" rel="stylesheet">
    <link rel="stylesheet" href="/styles.css">
  </head>
  <body${bodyClass}>
    <a class="skip-link" href="#main">Skip to content</a>
    <header class="site-header">
      <nav class="nav" aria-label="Main navigation">
        <a class="brand" href="/">
          <span class="brand-mark" aria-hidden="true">⛮</span>
          <span class="brand-name">Arbiter</span>
        </a>
        <div class="nav-links">${navLinks}
        </div>
      </nav>
    </header>
    <div id="main">
      ${body}
    </div>
    <footer class="site-footer">
      <div class="site-footer-inner">
        <span>Arbiter is experimental open-source infrastructure.</span>
        <a href="/docs/philosophy/">Philosophy</a>
      </div>
    </footer>
    <script src="/site.js" defer></script>
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

    if (/^(-{3,}|\*{3,}|_{3,})$/.test(line.trim())) {
      html.push('<hr>')
      index += 1
      continue
    }

    const fence = line.match(/^```(\w+)?\s*$/)
    if (fence) {
      const language = fence[1] ? ` class="language-${escapeHtml(fence[1])}"` : ''
      const code = []
      index += 1
      while (index < lines.length && !/^```/.test(lines[index])) {
        code.push(lines[index])
        index += 1
      }
      if (index < lines.length) index += 1
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
      const { html: listHtml, nextIndex } = renderList(lines, index, 'ul', doc)
      html.push(listHtml)
      index = nextIndex
      continue
    }

    if (/^\s*\d+\.\s+/.test(line)) {
      const { html: listHtml, nextIndex } = renderList(lines, index, 'ol', doc)
      html.push(listHtml)
      index = nextIndex
      continue
    }

    if (/^>\s?/.test(line)) {
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
      !/^(-{3,}|\*{3,}|_{3,})$/.test(lines[index].trim()) &&
      !isTableStart(lines, index)
    ) {
      paragraph.push(lines[index].trim())
      index += 1
    }
    html.push(`<p>${inline(paragraph.join(' '), doc)}</p>`)
  }

  return html.join('\n')
}

function renderList(lines, startIndex, type, doc) {
  const items = []
  let index = startIndex
  const itemRe = type === 'ul' ? /^(\s*)[-*]\s+(.*)$/ : /^(\s*)\d+\.\s+(.*)$/
  const anyItemRe = /^(\s*)(?:[-*]|\d+\.)\s+/
  const baseIndent = leadingSpaces(lines[index])

  while (index < lines.length) {
    const line = lines[index]
    if (!line.trim()) break

    const match = line.match(itemRe)
    if (!match || match[1].length !== baseIndent) break

    let itemHtml = inline(match[2], doc)
    index += 1

    if (index < lines.length && anyItemRe.test(lines[index]) && leadingSpaces(lines[index]) > baseIndent) {
      const nestedType = /^\s*\d+\.\s+/.test(lines[index]) ? 'ol' : 'ul'
      const nested = renderList(lines, index, nestedType, doc)
      itemHtml += nested.html
      index = nested.nextIndex
    }

    items.push(`<li>${itemHtml}</li>`)
  }

  return {
    html: `<${type}>${items.join('')}</${type}>`,
    nextIndex: index,
  }
}

function leadingSpaces(value) {
  return value.match(/^\s*/)?.[0].length ?? 0
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

  text = text.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (_, alt, href) => {
    const token = `@@TOK${tokens.length}@@`
    tokens.push(`<img src="${escapeAttribute(resolveHref(href, doc))}" alt="${escapeAttribute(alt)}">`)
    return token
  })

  text = text.replace(/`([^`]+)`/g, (_, code) => {
    const token = `@@TOK${tokens.length}@@`
    tokens.push(`<code>${code}</code>`)
    return token
  })

  text = text.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, href) => {
    return `<a href="${escapeAttribute(resolveHref(href, doc))}">${label}</a>`
  })
  text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
  text = text.replace(/(^|[^*])\*([^*]+)\*(?!\*)/g, '$1<em>$2</em>')

  for (let index = 0; index < tokens.length; index += 1) {
    text = text.replace(`@@TOK${index}@@`, tokens[index])
  }
  return text
}

function resolveHref(href, doc) {
  if (/^(https?:|mailto:|#|\/)/.test(href)) return href
  if (href.endsWith('.md') || href.includes('.md#')) {
    const [filePart, hash] = href.split('#')
    const sourceDir = path.posix.dirname(doc.relative)
    const target = path.posix.normalize(path.posix.join(sourceDir, filePart))

    if (target.startsWith('../') || target === '..') {
      const repoPath = path.posix.normalize(path.posix.join('docs', sourceDir, filePart))
      const cleaned = repoPath.replace(/^(\.\.\/)+/, '').replace(/^\//, '')
      return `${githubBlobBase}/${cleaned}${hash ? `#${hash}` : ''}`
    }

    const slug = target.replace(/\.md$/, '').replace(/\/index$/, '')
    return `/docs/${slug}/${hash ? `#${hash}` : ''}`
  }
  return href
}

function extractTitle(markdown) {
  return markdown.match(/^#\s+(.+)$/m)?.[1]?.trim()
}

function extractDescription(markdown, fallback) {
  const lines = markdown.replace(/\r\n/g, '\n').split('\n')
  for (const line of lines) {
    const trimmed = line.trim()
    if (!trimmed || trimmed.startsWith('#') || trimmed.startsWith('```') || trimmed.startsWith('|') || trimmed.startsWith('>') || trimmed.startsWith('-') || /^\d+\./.test(trimmed)) {
      continue
    }
    const plain = trimmed
      .replace(/!\[[^\]]*\]\([^)]+\)/g, '')
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
      .replace(/[`*_]/g, '')
      .replace(/\s+/g, ' ')
      .trim()
    if (plain.length > 40) return plain.slice(0, 180).replace(/\s+\S*$/, '') + (plain.length > 180 ? '…' : '')
  }
  return `Arbiter documentation: ${fallback}`
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
${urls.map((url) => `  <url><loc>${siteOrigin}${url}</loc></url>`).join('\n')}
</urlset>
`
}
