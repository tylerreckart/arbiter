import { sectionLabels } from './config.mjs'
import { groupDocs } from './docs.mjs'
import { titleCase } from './html.mjs'

const apiGroupLabels = {
  a2a: 'A2A',
  admin: 'Admin',
  agents: 'Agents',
  artifacts: 'Artifacts',
  conversations: 'Conversations',
  lessons: 'Lessons',
  memory: 'Memory',
  notifications: 'Notifications',
  requests: 'Requests',
  runs: 'Runs',
  schedules: 'Schedules',
  todos: 'Todos',
}

const conceptGroups = [
  {
    title: 'Runtime',
    slugs: ['writ', 'advisor', 'sse-events', 'fleet-streaming', 'durable-execution'],
  },
  {
    title: 'Memory & work',
    slugs: ['structured-memory', 'artifacts', 'todos', 'lessons', 'data-model'],
  },
  {
    title: 'Security',
    slugs: ['authentication', 'tenants', 'sandbox'],
  },
  {
    title: 'Integrations',
    slugs: ['mcp', 'a2a', 'search', 'scheduler'],
  },
  {
    title: 'Operations',
    slugs: ['operations'],
  },
]

export function displayTitle(title) {
  return String(title ?? '')
    .replace(/`/g, '')
    .trim()
}

export function buildSidebarModel(docs, activeDoc) {
  const groups = groupDocs(docs)
  const sections = [...groups.keys()].map((section) => {
    const sectionDocs = groups.get(section) ?? []
    const indexDoc = sectionDocs.find(
      (doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`,
    )
    return {
      active: Boolean(activeDoc?.section) && section === activeDoc.section,
      href: indexDoc?.href ?? sectionDocs[0]?.href ?? '/docs/',
      label: sectionLabels[section] ?? titleCase(section),
      section,
    }
  })

  if (!activeDoc?.section) {
    return {
      nav: { kind: 'empty' },
      sections,
      title: 'Documentation',
    }
  }

  const activeSection = activeDoc.section
  const sectionDocs = groups.get(activeSection) ?? []

  if (activeSection === 'api') {
    return {
      nav: buildApiNav(sectionDocs, activeDoc),
      sections,
      title: sectionLabels.api,
    }
  }

  if (activeSection === 'concepts') {
    return {
      nav: buildConceptNav(sectionDocs, activeDoc),
      sections,
      title: sectionLabels.concepts,
    }
  }

  if (sectionDocs.length > 0) {
    return {
      nav: {
        kind: 'list',
        links: sectionDocs.map((doc) => linkFor(doc, activeDoc)),
        title: sectionLabels[activeSection] ?? titleCase(activeSection),
      },
      sections,
      title: sectionLabels[activeSection] ?? titleCase(activeSection),
    }
  }

  return {
    nav: { kind: 'empty' },
    sections,
    title: 'Documentation',
  }
}

export function buildDocsIndexCards(docs) {
  const blurbs = {
    'getting-started': 'Install the binary, seed starter agents, and open the TUI.',
    concepts: 'Runtime model, memory, security boundaries, and how the pieces fit.',
    cli: 'One-shot dispatch, API server mode, environment, and admin commands.',
    tui: 'Sessions, panes, keybindings, themes, and streaming output.',
    api: 'HTTP + SSE endpoints for orchestration, agents, memory, and admin.',
    philosophy: 'Why Arbiter is a local process, not a hosted chat product.',
  }

  const groups = groupDocs(docs)
  return [...groups.entries()].map(([section, sectionDocs]) => {
    const indexDoc = sectionDocs.find(
      (doc) => doc.relative.endsWith('/index.md') || doc.relative === `${section}.md`,
    )
    const first = indexDoc ?? sectionDocs[0]
    return {
      blurb: blurbs[section] ?? 'Reference documentation for this section.',
      count: sectionDocs.length,
      href: first.href,
      label: sectionLabels[section] ?? titleCase(section),
    }
  })
}

export function buildBreadcrumbs(doc) {
  const section = sectionLabels[doc.section] ?? titleCase(doc.section)
  const sectionHref = `/docs/${doc.section}/`
  const crumbs = [
    { href: '/docs/', label: 'Docs' },
    { href: sectionHref, label: section },
  ]
  if (!isSectionIndex(doc)) {
    crumbs.push({ href: doc.href, label: displayTitle(doc.title) })
  }
  return crumbs
}

export function buildSectionNeighbors(docs, doc) {
  // Walk docs in the same order the sidebar presents them.
  const sectionDocs = orderedSectionDocs(
    docs.filter((entry) => entry.section === doc.section),
    doc.section,
  )
  const index = sectionDocs.findIndex((entry) => entry.href === doc.href)
  if (index === -1) return { next: null, prev: null }
  return {
    next: sectionDocs[index + 1]
      ? { href: sectionDocs[index + 1].href, title: displayTitle(sectionDocs[index + 1].title) }
      : null,
    prev: sectionDocs[index - 1]
      ? { href: sectionDocs[index - 1].href, title: displayTitle(sectionDocs[index - 1].title) }
      : null,
  }
}

function orderedSectionDocs(sectionDocs, section) {
  if (section === 'concepts') return orderConceptDocs(sectionDocs)
  if (section === 'api') return orderApiDocs(sectionDocs)
  return sectionDocs
}

function orderConceptDocs(sectionDocs) {
  const bySlug = new Map()
  let overview = null

  for (const doc of sectionDocs) {
    if (doc.relative === 'concepts/index.md') {
      overview = doc
      continue
    }
    const slug = doc.relative.replace(/^concepts\//, '').replace(/\.md$/, '')
    bySlug.set(slug, doc)
  }

  const ordered = []
  if (overview) ordered.push(overview)

  const used = new Set()
  for (const group of conceptGroups) {
    for (const slug of group.slugs) {
      const doc = bySlug.get(slug)
      if (!doc) continue
      used.add(slug)
      ordered.push(doc)
    }
  }

  for (const [slug, doc] of [...bySlug.entries()].sort(([a], [b]) => a.localeCompare(b))) {
    if (!used.has(slug)) ordered.push(doc)
  }

  return ordered
}

function orderApiDocs(sectionDocs) {
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

  const ordered = [...topLevel]
  for (const [, docsInGroup] of [...subgroups.entries()].sort(([a], [b]) => a.localeCompare(b))) {
    ordered.push(...docsInGroup)
  }
  return ordered
}

export function buildSearchIndex(docs) {
  return docs.map((doc) => ({
    description: doc.description,
    href: doc.href,
    section: sectionLabels[doc.section] ?? titleCase(doc.section),
    title: displayTitle(doc.title),
  }))
}

export function buildTitleMap(docs) {
  const map = new Map()
  for (const doc of docs) {
    map.set(doc.href, displayTitle(doc.title))
    if (isSectionIndex(doc)) {
      map.set(doc.href, sectionLabels[doc.section] ?? displayTitle(doc.title))
    }
  }
  return map
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
        label: apiGroupLabels[key] ?? titleCase(key),
        open: docsInGroup.some((doc) => doc.href === activeDoc.href),
        links: docsInGroup.map((doc) => linkFor(doc, activeDoc, { api: true })),
      })),
    title: sectionLabels.api,
    topLevel: topLevel.map((doc) => linkFor(doc, activeDoc, { api: true })),
  }
}

function buildConceptNav(sectionDocs, activeDoc) {
  const bySlug = new Map()
  let overview = null

  for (const doc of sectionDocs) {
    if (doc.relative === 'concepts/index.md') {
      overview = doc
      continue
    }
    const slug = doc.relative.replace(/^concepts\//, '').replace(/\.md$/, '')
    bySlug.set(slug, doc)
  }

  const groups = []
  const used = new Set()

  if (overview) {
    groups.push({
      title: '',
      links: [linkFor(overview, activeDoc)],
    })
  }

  for (const group of conceptGroups) {
    const links = []
    for (const slug of group.slugs) {
      const doc = bySlug.get(slug)
      if (!doc) continue
      used.add(slug)
      links.push(linkFor(doc, activeDoc))
    }
    if (links.length > 0) {
      groups.push({ title: group.title, links })
    }
  }

  const leftover = [...bySlug.entries()]
    .filter(([slug]) => !used.has(slug))
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([, doc]) => linkFor(doc, activeDoc))

  if (leftover.length > 0) {
    groups.push({ title: 'More', links: leftover })
  }

  return {
    kind: 'groups',
    groups,
  }
}

function linkFor(doc, activeDoc, { api = false } = {}) {
  const label =
    isSectionIndex(doc) && doc.section !== 'api'
      ? 'Overview'
      : doc.relative === 'api/index.md'
        ? 'Overview'
        : displayTitle(doc.title)

  return {
    active: doc.href === activeDoc.href,
    api,
    href: doc.href,
    label,
  }
}

function isSectionIndex(doc) {
  return (
    doc.relative.endsWith('/index.md') ||
    doc.relative === `${doc.section}.md` ||
    doc.relative === 'index.md'
  )
}
