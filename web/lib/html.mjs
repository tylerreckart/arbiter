import path from 'node:path'

export function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

export function escapeAttribute(value) {
  return escapeHtml(value).replace(/'/g, '&#39;')
}

export function titleCase(value) {
  return value
    .replace(/[-_]/g, ' ')
    .replace(/\b\w/g, (letter) => letter.toUpperCase())
}

export function slugify(value) {
  return value
    .toLowerCase()
    .replace(/`/g, '')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-|-$/g, '')
}

export function slash(value) {
  return value.split(path.sep).join('/')
}
