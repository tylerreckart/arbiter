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
