import path from 'node:path'
import { githubBlobBase } from './config.mjs'
import { escapeAttribute, escapeHtml, slugify } from './html.mjs'

export function markdownToHtml(markdown, doc) {
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
      const list = renderList(lines, index, 'ul', doc)
      html.push(list.html)
      index = list.nextIndex
      continue
    }

    if (/^\s*\d+\.\s+/.test(line)) {
      const list = renderList(lines, index, 'ol', doc)
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

export function extractTitle(markdown) {
  return markdown.match(/^#\s+(.+)$/m)?.[1]?.trim()
}

export function extractDescription(markdown, fallback) {
  const lines = markdown.replace(/\r\n/g, '\n').split('\n')
  for (const line of lines) {
    const trimmed = line.trim()
    if (
      !trimmed ||
      trimmed.startsWith('#') ||
      trimmed.startsWith('```') ||
      trimmed.startsWith('|') ||
      trimmed.startsWith('>') ||
      trimmed.startsWith('-') ||
      /^\d+\./.test(trimmed)
    ) {
      continue
    }
    const plain = trimmed
      .replace(/!\[[^\]]*\]\([^)]+\)/g, '')
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
      .replace(/[`*_]/g, '')
      .replace(/\s+/g, ' ')
      .trim()
    if (plain.length > 40) {
      return plain.slice(0, 180).replace(/\s+\S*$/, '') + (plain.length > 180 ? '…' : '')
    }
  }
  return `Arbiter documentation: ${fallback}`
}

export function extractToc(markdown) {
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

function stripHashes(value) {
  return value.replace(/\s+#*$/, '')
}
