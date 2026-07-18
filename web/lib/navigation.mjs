import { sectionLabels } from './config.mjs'
import { groupDocs } from './docs.mjs'
import { titleCase } from './html.mjs'

export function buildSidebarModel(docs, activeDoc) {
  const groups = groupDocs(docs)
  const tabs = [...groups.keys()].map((section) => {
    const sectionDocs = groups.get(section) ?? []
    const indexDoc = sectionDocs.find(
      (doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`,
    )
    return {
      active: section === activeDoc.section,
      href: indexDoc?.href ?? sectionDocs[0]?.href ?? '/docs/',
      label: sectionLabels[section] ?? titleCase(section),
      section,
    }
  })

  const activeSection = activeDoc.section
  const sectionDocs = groups.get(activeSection) ?? []

  if (activeSection === 'api') {
    return {
      nav: buildApiNav(sectionDocs, activeDoc),
      tabs,
      title: sectionLabels.api,
    }
  }

  if (sectionDocs.length > 0) {
    return {
      nav: {
        kind: 'list',
        links: sectionDocs.map((doc) => ({
          active: doc.href === activeDoc.href,
          href: doc.href,
          label: doc.relative.endsWith('/index.md') ? 'Overview' : doc.title,
        })),
        title: sectionLabels[activeSection] ?? titleCase(activeSection),
      },
      tabs,
      title: sectionLabels[activeSection] ?? titleCase(activeSection),
    }
  }

  return {
    nav: {
      kind: 'groups',
      groups: [...groups.entries()].map(([section, docsInSection]) => ({
        links: docsInSection.map((doc) => ({
          active: doc.href === activeDoc.href,
          href: doc.href,
          label: doc.title,
        })),
        title: sectionLabels[section] ?? titleCase(section),
      })),
    },
    tabs,
    title: 'Docs',
  }
}

export function buildDocsIndexCards(docs) {
  const groups = groupDocs(docs)
  return [...groups.entries()].map(([section, sectionDocs]) => {
    const indexDoc = sectionDocs.find(
      (doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`,
    )
    const first = indexDoc ?? sectionDocs[0]
    const preview = sectionDocs
      .slice(0, 4)
      .map((doc) => doc.title)
      .join(', ')
    return {
      count: sectionDocs.length,
      href: first.href,
      label: sectionLabels[section] ?? titleCase(section),
      preview,
    }
  })
}

function buildApiNav(sectionDocs, activeDoc) {
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

  return {
    kind: 'api',
    subgroups: [...subgroups.entries()]
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, docsInGroup]) => ({
        key,
        label: titleCase(key),
        open: docsInGroup.some((doc) => doc.href === activeDoc.href),
        links: docsInGroup.map((doc) => ({
          active: doc.href === activeDoc.href,
          href: doc.href,
          label: shortApiTitle(doc),
        })),
      })),
    title: sectionLabels.api,
    topLevel: topLevel.map((doc) => ({
      active: doc.href === activeDoc.href,
      href: doc.href,
      label: doc.relative === 'api/index.md' ? 'Overview' : doc.title,
    })),
  }
}

function shortApiTitle(doc) {
  const leaf = doc.relative.split('/').pop().replace(/\.md$/, '')
  if (leaf === 'index') return 'Overview'
  return titleCase(leaf)
}
