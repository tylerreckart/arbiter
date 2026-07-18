const copyButton = document.querySelector('[data-copy-install]')
if (copyButton) {
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

const revealItems = document.querySelectorAll('.reveal')
if (revealItems.length > 0 && 'IntersectionObserver' in window) {
  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue
        entry.target.classList.add('is-visible')
        observer.unobserve(entry.target)
      }
    },
    { rootMargin: '0px 0px -8% 0px', threshold: 0.15 },
  )
  for (const item of revealItems) observer.observe(item)
} else {
  for (const item of revealItems) item.classList.add('is-visible')
}
