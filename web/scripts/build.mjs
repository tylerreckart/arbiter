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
  installCommand,
  installPath,
  repoRoot,
  sectionLabels,
  siteScriptPath,
  stylesPath,
} from '../lib/config.mjs'
import { readDocs, sortDocs } from '../lib/docs.mjs'
import { extractToc, markdownToHtml } from '../lib/markdown.mjs'
import { buildDocsIndexCards, buildSidebarModel } from '../lib/navigation.mjs'
import { renderPage } from '../lib/render.mjs'

await fs.rm(dist, { force: true, recursive: true })
await fs.mkdir(dist, { recursive: true })
await fs.copyFile(stylesPath, path.join(dist, 'styles.css'))
await fs.copyFile(siteScriptPath, path.join(dist, 'site.js'))
await fs.copyFile(installPath, path.join(dist, 'install.sh'))
await copyAssets()
await writeFavicon()

const docs = sortDocs(await readDocs(docsRoot))

await writeFile(
  'index.html',
  renderPage('home.pug', {
    canonicalPath: '/',
    description:
      'Arbiter is an open-source, local-first multi-agent workspace for the terminal. Run parallel conversations and watch tools and diffs stream live.',
    gettingStartedHref:
      docs.find((doc) => doc.relative === 'getting-started/local.md')?.href ?? '/docs/',
    installCommand,
    ogImage: '/assets/terminal.jpg',
    philosophyHref: docs.find((doc) => doc.relative === 'philosophy.md')?.href ?? '/docs/',
    title: 'Arbiter — your agent workspace, in the terminal',
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
