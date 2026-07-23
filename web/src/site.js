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
