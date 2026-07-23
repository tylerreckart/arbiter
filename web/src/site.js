const copyButtons = document.querySelectorAll('[data-copy-install]')
for (const copyButton of copyButtons) {
  const command = copyButton.getAttribute('data-copy-install') || ''
  copyButton.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(command)
      copyButton.dataset.copied = 'true'
      copyButton.textContent = 'Copied'
      window.setTimeout(() => {
        copyButton.dataset.copied = 'false'
        copyButton.textContent = 'Copy'
      }, 1600)
    } catch {
      copyButton.textContent = 'Select & copy'
    }
  })
}

const prefersReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches
if (!prefersReducedMotion && 'IntersectionObserver' in window) {
  const reveals = [...document.querySelectorAll('.reveal')]
  let observer = null

  const revealIfInView = (reveal) => {
    const rect = reveal.getBoundingClientRect()
    return rect.top < window.innerHeight * 0.94 && rect.bottom > 0
  }

  const markVisible = (reveal) => {
    reveal.classList.add('is-visible')
    observer?.unobserve(reveal)
  }

  const syncVisible = () => {
    for (const reveal of reveals) {
      if (reveal.classList.contains('is-visible')) continue
      if (revealIfInView(reveal)) markVisible(reveal)
    }
  }

  observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue
        markVisible(entry.target)
      }
    },
    { rootMargin: '0px 0px -4% 0px', threshold: 0.06 },
  )

  syncVisible()
  document.documentElement.classList.add('motion-ok')
  for (const reveal of reveals) {
    if (!reveal.classList.contains('is-visible')) observer.observe(reveal)
  }

  window.addEventListener('load', syncVisible, { once: true })
  window.addEventListener('hashchange', syncVisible)
  window.setTimeout(syncVisible, 400)
}

initDocsSearch()
initTocSpy()

function initDocsSearch() {
  const input = document.querySelector('[data-docs-search]')
  const results = document.querySelector('[data-docs-search-results]')
  if (!input || !results) return

  let index = null
  let active = -1
  let hits = []
  let loadPromise = null

  const loadIndex = () => {
    if (index) return Promise.resolve(index)
    if (loadPromise) return loadPromise
    loadPromise = fetch('/docs/search-index.json')
      .then((response) => {
        if (!response.ok) throw new Error('search index missing')
        return response.json()
      })
      .then((data) => {
        index = data
        return index
      })
      .catch(() => {
        index = []
        return index
      })
    return loadPromise
  }

  const render = () => {
    if (!hits.length) {
      results.hidden = false
      results.innerHTML = '<div class="docs-search-empty">No matches</div>'
      return
    }

    results.hidden = false
    results.innerHTML = hits
      .map(
        (hit, i) =>
          `<a class="docs-search-hit${i === active ? ' is-active' : ''}" href="${hit.href}" role="option">` +
          `<span class="docs-search-hit-title">${escapeText(hit.title)}</span>` +
          `<span class="docs-search-hit-meta">${escapeText(hit.section)}</span>` +
          `</a>`,
      )
      .join('')
  }

  const clear = () => {
    hits = []
    active = -1
    results.hidden = true
    results.innerHTML = ''
  }

  const runSearch = async (query) => {
    const q = query.trim().toLowerCase()
    if (q.length < 2) {
      clear()
      return
    }
    await loadIndex()
    hits = (index || [])
      .map((entry) => {
        const title = entry.title.toLowerCase()
        const section = entry.section.toLowerCase()
        const description = (entry.description || '').toLowerCase()
        let score = 0
        if (title === q) score += 100
        if (title.startsWith(q)) score += 40
        if (title.includes(q)) score += 20
        if (section.includes(q)) score += 8
        if (description.includes(q)) score += 4
        const parts = q.split(/\s+/).filter(Boolean)
        if (parts.length > 1 && parts.every((part) => `${title} ${description}`.includes(part))) {
          score += 12
        }
        return { ...entry, score }
      })
      .filter((entry) => entry.score > 0)
      .sort((a, b) => b.score - a.score || a.title.localeCompare(b.title))
      .slice(0, 12)
    active = hits.length ? 0 : -1
    render()
  }

  input.addEventListener('input', () => {
    runSearch(input.value)
  })

  input.addEventListener('keydown', (event) => {
    if (results.hidden || !hits.length) return
    if (event.key === 'ArrowDown') {
      event.preventDefault()
      active = (active + 1) % hits.length
      render()
    } else if (event.key === 'ArrowUp') {
      event.preventDefault()
      active = (active - 1 + hits.length) % hits.length
      render()
    } else if (event.key === 'Enter' && active >= 0) {
      event.preventDefault()
      window.location.href = hits[active].href
    } else if (event.key === 'Escape') {
      clear()
      input.blur()
    }
  })

  document.addEventListener('click', (event) => {
    if (event.target === input || results.contains(event.target)) return
    clear()
  })
}

function initTocSpy() {
  const links = [...document.querySelectorAll('[data-toc-link]')]
  if (!links.length || !('IntersectionObserver' in window)) return

  const headings = links
    .map((link) => document.getElementById(link.dataset.tocLink))
    .filter(Boolean)

  let activeId = null
  const setActive = (id) => {
    if (!id || id === activeId) return
    activeId = id
    for (const link of links) {
      link.classList.toggle('is-active', link.dataset.tocLink === id)
    }
  }

  const observer = new IntersectionObserver(
    (entries) => {
      const visible = entries
        .filter((entry) => entry.isIntersecting)
        .sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)
      if (visible[0]) setActive(visible[0].target.id)
    },
    {
      rootMargin: `-${getComputedStyle(document.documentElement).getPropertyValue('--header-h') || '64px'} 0px -55% 0px`,
      threshold: [0, 1],
    },
  )

  for (const heading of headings) observer.observe(heading)
  if (headings[0]) setActive(headings[0].id)
}

function escapeText(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}
