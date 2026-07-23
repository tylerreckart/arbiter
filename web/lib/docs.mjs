import fs from 'node:fs/promises'
import path from 'node:path'
import { sectionOrder } from './config.mjs'
import { slash, titleCase } from './html.mjs'
import { extractDescription, extractTitle } from './markdown.mjs'

export async function readDocs(base) {
  const files = await walk(base)
  const markdownFiles = files.filter((file) => file.endsWith('.md'))
  const docs = []

  for (const file of markdownFiles) {
    const relative = slash(path.relative(base, file))
    const source = await fs.readFile(file, 'utf8')
    const stat = await fs.stat(file)
    const title = extractTitle(source) ?? titleFromSlug(relative)
    const description = extractDescription(source, title)
    const slug = relative.replace(/\.md$/, '').replace(/\/index$/, '')
    const section = relative.includes('/') ? relative.split('/')[0] : slug
    const href = `/docs/${slug}/`
    docs.push({
      description,
      href,
      lastmod: stat.mtime.toISOString().slice(0, 10),
      outputPath: `docs/${slug}/index.html`,
      relative,
      section,
      source,
      title,
    })
  }

  return docs
}

export function sortDocs(docs) {
  return [...docs].sort((a, b) => {
    const sectionDiff = sectionRank(a.section) - sectionRank(b.section)
    if (sectionDiff !== 0) return sectionDiff
    const aIndex = a.relative.endsWith('/index.md') || a.relative === 'index.md'
    const bIndex = b.relative.endsWith('/index.md') || b.relative === 'index.md'
    if (aIndex !== bIndex) return aIndex ? -1 : 1
    return a.relative.localeCompare(b.relative)
  })
}

export function groupDocs(docs) {
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

function sectionRank(section) {
  const index = sectionOrder.indexOf(section)
  return index === -1 ? 999 : index
}

function titleFromSlug(relative) {
  const base = relative.replace(/\.md$/, '').split('/').pop()
  return titleCase(base)
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
