import fs from 'node:fs/promises'
import path from 'node:path'
import {
  copyAssets,
  renderRobots,
  renderSitemap,
  writeFavicon,
  writeFile,
} from '../lib/assets.mjs'
import {
  dist,
  docsRoot,
  binaryRelease,
  installCommand,
  installPath,
  liquidHeroPath,
  macDownloadUrl,
  repoRoot,
  sectionLabels,
  siteScriptPath,
} from '../lib/config.mjs'
import { readDocs, sortDocs } from '../lib/docs.mjs'
import { extractToc, markdownToHtml } from '../lib/markdown.mjs'
import { buildDocsIndexCards, buildSidebarModel } from '../lib/navigation.mjs'
import { renderPage } from '../lib/render.mjs'
import { writeStyles } from '../lib/styles.mjs'

await fs.rm(dist, { force: true, recursive: true })
await fs.mkdir(dist, { recursive: true })
await writeStyles(path.join(dist, 'styles.css'))
await fs.copyFile(siteScriptPath, path.join(dist, 'site.js'))
await fs.copyFile(liquidHeroPath, path.join(dist, 'liquid-hero.js'))
await fs.copyFile(installPath, path.join(dist, 'install.sh'))
await copyAssets()
await writeFavicon()

const docs = sortDocs(await readDocs(docsRoot))

await writeFile(
  'index.html',
  renderPage('home.pug', {
    canonicalPath: '/',
    description:
      'Arbiter is a local agent runtime in a single native binary — TUI, one-shot CLI, or HTTP+SSE — with hard tool allowlists and a shared streaming event bus.',
    gettingStartedHref:
      docs.find((doc) => doc.relative === 'getting-started/local.md')?.href ?? '/docs/',
    binaryRelease,
    installCommand,
    macDownloadUrl,
    ogImage: '/assets/terminal.jpg',
    philosophyHref: docs.find((doc) => doc.relative === 'philosophy.md')?.href ?? '/docs/',
    title: 'Arbiter — the agent runtime that lives on your machine',
    variant: 'marketing',
  }),
)

await writeFile(
  'docs/index.html',
  renderPage('docs-index.pug', {
    cards: buildDocsIndexCards(docs),
    canonicalPath: '/docs/',
    description: 'Install, operate, and extend the Arbiter self-hosted agent runtime.',
    sidebar: buildSidebarModel(docs, { href: '/docs/', section: 'getting-started' }),
    title: 'Arbiter Documentation',
    variant: 'docs',
  }),
)

for (let index = 0; index < docs.length; index += 1) {
  const doc = docs[index]
  const prev = docs[index - 1]
  const next = docs[index + 1]
  const toc = extractToc(doc.source)

  await writeFile(
    doc.outputPath,
    renderPage('doc.pug', {
      breadcrumbs: `${sectionLabels[doc.section] ?? doc.section} / ${doc.title}`,
      canonicalPath: doc.href,
      contentHtml: markdownToHtml(doc.source, doc),
      description: doc.description,
      next,
      prev,
      sidebar: buildSidebarModel(docs, doc),
      title: `${doc.title} — Arbiter Docs`,
      toc,
      variant: 'docs',
    }),
  )
}

await writeFile('robots.txt', renderRobots())
await writeFile('sitemap.xml', renderSitemap(docs))

console.log(`Built ${docs.length + 2} pages into ${path.relative(repoRoot, dist)}`)
