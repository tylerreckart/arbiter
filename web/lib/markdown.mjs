import path from 'node:path'
import { githubBlobBase } from './config.mjs'
import { escapeAttribute, escapeHtml, slugify } from './html.mjs'

export function markdownToHtml(markdown, doc, options = {}) {
  const ctx = { doc, titlesByHref: options.titlesByHref ?? new Map() }
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
      html.push(`<h${level} id="${id}">${inline(text, ctx)}</h${level}>`)
      index += 1
      continue
    }

    if (isTableStart(lines, index)) {
      const tableLines = []
      while (index < lines.length && lines[index].trim().startsWith('|')) {
        tableLines.push(lines[index])
        index += 1
      }
      html.push(renderTable(tableLines, ctx))
      continue
    }

    if (/^\s*[-*]\s+/.test(line)) {
      const list = renderList(lines, index, 'ul', ctx)
      html.push(list.html)
      index = list.nextIndex
      continue
    }

    if (/^\s*\d+\.\s+/.test(line)) {
      const list = renderList(lines, index, 'ol', ctx)
      html.push(list.html)
      index = list.nextIndex
      continue
    }

    if (/^>\s?/.test(line)) {
      const quote = []
      while (index < lines.length && /^>\s?/.test(lines[index])) {
        quote.push(lines[index].replace(/^>\s?/, ''))
        index += 1
      }
      html.push(`<blockquote><p>${inline(quote.join(' '), ctx)}</p></blockquote>`)
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
    html.push(`<p>${inline(paragraph.join(' '), ctx)}</p>`)
  }

  return html.join('\n')
}

export function extractTitle(markdown) {
  return markdown.match(/^#\s+(.+)$/m)?.[1]?.trim()
}

export function extractDescription(markdown, fallback) {
  const lines = markdown.replace(/\r\n/g, '\n').split('\n')
  let inFence = false
  const paragraphs = []
  let current = []

  const flush = () => {
    if (!current.length) return
    const plain = current
      .join(' ')
      .replace(/!\[[^\]]*\]\([^)]+\)/g, '')
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
      .replace(/[`*_]/g, '')
      .replace(/\s+/g, ' ')
      .trim()
    current = []
    if (
      plain.length >= 40 &&
      !/^https?:\/\//i.test(plain) &&
      !/^[A-Z][A-Za-z0-9_./:-]*\s+[-|]/.test(plain)
    ) {
      paragraphs.push(plain)
    }
  }

  for (const line of lines) {
    const trimmed = line.trim()
    if (trimmed.startsWith('```')) {
      flush()
      inFence = !inFence
      continue
    }
    if (inFence) continue
    if (
      !trimmed ||
      trimmed.startsWith('#') ||
      trimmed.startsWith('|') ||
      trimmed.startsWith('>') ||
      trimmed.startsWith('-') ||
      trimmed.startsWith('*') ||
      /^\d+\./.test(trimmed) ||
      /^\*\*[A-Za-z][^:*]*:\*\*/.test(trimmed)
    ) {
      flush()
      continue
    }
    current.push(trimmed)
  }
  flush()

  const plain = paragraphs[0]
  if (!plain) {
    const cleanFallback = String(fallback ?? '')
      .replace(/`/g, '')
      .trim()
    return `Arbiter documentation for ${cleanFallback}.`
  }
  if (plain.length <= 160) return plain
  const clipped = plain.slice(0, 157).replace(/\s+\S*$/, '').replace(/[,:;–—-]\s*$/, '')
  return `${clipped}…`
}

export function extractToc(markdown) {
  const toc = []
  for (const line of markdown.replace(/\r\n/g, '\n').split('\n')) {
    const match = line.match(/^(#{2,3})\s+(.+)$/)
    if (!match) continue
    const text = stripHashes(match[2]).replace(/`/g, '')
    toc.push({
      id: slugify(text),
      level: match[1].length,
      text,
    })
  }
  return toc
}

function renderList(lines, startIndex, type, ctx) {
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

    let itemHtml = inline(match[2], ctx)
    index += 1

    if (index < lines.length && anyItemRe.test(lines[index]) && leadingSpaces(lines[index]) > baseIndent) {
      const nestedType = /^\s*\d+\.\s+/.test(lines[index]) ? 'ol' : 'ul'
      const nested = renderList(lines, index, nestedType, ctx)
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

function renderTable(lines, ctx) {
  const rows = lines
    .filter((_line, index) => index !== 1)
    .map((line) =>
      line
        .trim()
        .replace(/^\||\|$/g, '')
        .split('|')
        .map((cell) => cell.trim()),
    )

  const [head = [], ...body] = rows
  return `<table><thead><tr>${head
    .map((cell) => `<th>${inline(cell, ctx)}</th>`)
    .join('')}</tr></thead><tbody>${body
    .map((row) => `<tr>${row.map((cell) => `<td>${inline(cell, ctx)}</td>`).join('')}</tr>`)
    .join('')}</tbody></table>`
}

function isTableStart(lines, index) {
  return (
    lines[index]?.trim().startsWith('|') &&
    /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(lines[index + 1] ?? '')
  )
}

function inline(value, ctx) {
  const tokens = []
  let text = escapeHtml(value)

  text = text.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (_, alt, href) => {
    const token = `@@TOK${tokens.length}@@`
    tokens.push(
      `<img src="${escapeAttribute(resolveHref(href, ctx.doc))}" alt="${escapeAttribute(alt)}">`,
    )
    return token
  })

  text = text.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_, label, href) => {
    const resolved = resolveHref(href, ctx.doc)
    const display = formatLinkLabel(label, resolved, ctx)
    return `<a href="${escapeAttribute(resolved)}">${display}</a>`
  })

  text = text.replace(/`([^`]+)`/g, (_, code) => {
    const token = `@@TOK${tokens.length}@@`
    tokens.push(`<code>${code}</code>`)
    return token
  })

  text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
  text = text.replace(/(^|[^*])\*([^*]+)\*(?!\*)/g, '$1<em>$2</em>')

  for (let index = 0; index < tokens.length; index += 1) {
    text = text.replace(`@@TOK${index}@@`, tokens[index])
  }
  return text
}

function formatLinkLabel(label, resolvedHref, ctx) {
  const raw = unescapeHtml(label)
  const plain = raw.replace(/`/g, '').trim()
  if (looksLikeDocPath(plain)) {
    const baseHref = resolvedHref.split('#')[0]
    const normalized = baseHref.endsWith('/') ? baseHref : `${baseHref}/`
    if (normalized.startsWith('/docs/')) {
      const title = ctx.titlesByHref.get(normalized)
      if (title) return escapeHtml(title)
    }
  }

  // Always escape label text; only inject <code> for backtick spans.
  return raw
    .split(/(`[^`]+`)/g)
    .map((part) => {
      const code = part.match(/^`([^`]+)`$/)
      if (code) return `<code>${escapeHtml(code[1])}</code>`
      return escapeHtml(part)
    })
    .join('')
}

function unescapeHtml(value) {
  return String(value)
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&')
}

function looksLikeDocPath(label) {
  return (
    /\.md\b/i.test(label) ||
    /^docs\//i.test(label) ||
    /^(?:\.\.\/)+(?:[a-z0-9._/-]+)$/i.test(label) ||
    /^[a-z0-9_-]+(?:\/[a-z0-9._-]+)+\/?$/i.test(label)
  )
}

function resolveHref(href, doc) {
  const trimmed = String(href).trim()

  // Allow only safe absolute schemes; neutralize javascript:/data:/etc.
  if (/^[a-z][a-z0-9+.-]*:/i.test(trimmed)) {
    if (/^(https?:|mailto:)/i.test(trimmed)) return trimmed
    return '#'
  }

  // Protocol-relative URLs (//evil.example) look like paths but leave the origin.
  if (trimmed.startsWith('//')) return '#'

  if (trimmed.startsWith('#') || trimmed.startsWith('/')) return trimmed

  if (trimmed.endsWith('.md') || trimmed.includes('.md#')) {
    const [filePart, hash] = trimmed.split('#')
    const sourceDir = path.posix.dirname(doc.relative)
    const repoPath = path.posix.normalize(path.posix.join('docs', sourceDir, filePart))

    // Links that climb out of docs/ (e.g. ../../CONTRIBUTING.md) point at GitHub.
    if (!repoPath.startsWith('docs/') && repoPath !== 'docs') {
      const cleaned = repoPath.replace(/^(\.\.\/)+/, '').replace(/^\//, '')
      return `${githubBlobBase}/${cleaned}${hash ? `#${hash}` : ''}`
    }

    const slug = repoPath
      .replace(/^docs\//, '')
      .replace(/\.md$/, '')
      .replace(/\/index$/, '')
    return `/docs/${slug}/${hash ? `#${hash}` : ''}`
  }
  return trimmed
}

function stripHashes(value) {
  return value.replace(/\s+#*$/, '')
}
