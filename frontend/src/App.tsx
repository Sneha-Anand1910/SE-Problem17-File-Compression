import { useState } from 'react'

type Action = 'compress' | 'extract' | 'list'

const sections: { id: Action; title: string; description: string; button: string; accept?: string }[] = [
  { id: 'compress', title: 'Compress', description: 'Choose files to add to a ZIP archive.', button: 'Compress files' },
  { id: 'extract', title: 'Extract', description: 'Choose a ZIP archive to unpack.', button: 'Extract archive', accept: '.zip,application/zip' },
  { id: 'list', title: 'List ZIP Contents', description: 'Inspect the files inside a ZIP archive.', button: 'List contents', accept: '.zip,application/zip' },
]

function App() {
  const [files, setFiles] = useState<Partial<Record<Action, File[]>>>({})
  const [status, setStatus] = useState<Partial<Record<Action, string>>>({})
  const [archiveName, setArchiveName] = useState('archive.zip')
  const zipName = archiveName.trim().replace(/\.zip$/i, '') || 'archive'
  const finalArchiveName = `${zipName}.zip`

  function selectFiles(action: Action, selection: FileList | null) {
    const selected = Array.from(selection ?? [])
    if (action === 'compress') {
      const existing = files.compress ?? []
      const additions = selected.filter((file, index) =>
        !existing.some(saved => saved.name.toLowerCase() === file.name.toLowerCase()) &&
        selected.findIndex(candidate => candidate.name.toLowerCase() === file.name.toLowerCase()) === index,
      )
      const updated = [...existing, ...additions]
      setFiles(current => ({ ...current, compress: updated }))
      setStatus(current => ({ ...current, compress: additions.length ? `${updated.length} file${updated.length === 1 ? '' : 's'} selected.` : 'Those files are already selected.' }))
      return
    }
    setFiles(current => ({ ...current, [action]: selected }))
    setStatus(current => ({ ...current, [action]: selected.length ? `${selected.length} file${selected.length === 1 ? '' : 's'} selected.` : '' }))
  }

  function removeCompressFile(name: string) {
    const updated = (files.compress ?? []).filter(file => file.name !== name)
    setFiles(current => ({ ...current, compress: updated }))
    setStatus(current => ({ ...current, compress: updated.length ? `${updated.length} file${updated.length === 1 ? '' : 's'} selected.` : 'Select a file to begin.' }))
  }

  function requestAction(action: Action) {
    setStatus(current => ({ ...current, [action]: 'Backend integration is not connected yet.' }))
  }

  return (
    <div className="page">
      <header className="hero">
        <div className="hero-inner">
          <h1>ZIP tools, in one place.</h1>
          <p>Select files or an archive to get started.</p>
        </div>
      </header>
      <main className="workspace">
        <div className="section-heading">
          <div><span className="eyebrow">Workspace</span><h2>Choose an action</h2></div>
          <span className="connection-status"><span className="status-dot" /> Backend not connected</span>
        </div>
        <div className="cards">
          {sections.map((section, index) => (
            <section className="card" key={section.id} aria-labelledby={`${section.id}-title`}>
              <div className="card-top"><span className="step">0{index + 1}</span><span className="card-icon" aria-hidden="true">{section.id === 'compress' ? '↓' : section.id === 'extract' ? '↑' : '≡'}</span></div>
              <h3 id={`${section.id}-title`}>{section.title}</h3>
              <p className="description">{section.description}</p>
              <label className="file-picker">
                <span className="picker-title">{files[section.id]?.length ? 'Change selection' : `Choose file${section.id === 'compress' ? 's' : ''}`}</span>
                <span className="picker-hint">{section.id === 'compress' ? 'Select one or more files' : 'ZIP files only'}</span>
                <input type="file" multiple={section.id === 'compress'} accept={section.accept} onChange={event => { selectFiles(section.id, event.target.files); if (section.id === 'compress') event.target.value = '' }} />
              </label>
              <div className="selection" aria-live="polite">
                {files[section.id]?.length ? files[section.id]!.map((file, fileIndex) => section.id === 'compress'
                  ? <span className="selected-file" key={file.name}>{file.name}<button className="remove-file" type="button" aria-label={`Remove ${file.name}`} onClick={() => removeCompressFile(file.name)}>×</button></span>
                  : <span key={`${file.name}-${fileIndex}`}>{file.name}</span>) : <span className="empty">No file selected</span>}
              </div>
              {section.id === 'compress' && <label className="archive-name">Archive name
                <input type="text" value={archiveName} onChange={event => setArchiveName(event.target.value)} onBlur={() => setArchiveName(finalArchiveName)} placeholder="archive.zip" />
                <span>Output name: {finalArchiveName}</span>
              </label>}
              <button type="button" disabled={!files[section.id]?.length} onClick={() => requestAction(section.id)}>{section.button} <span aria-hidden="true">→</span></button>
              <p className="message" role="status">{status[section.id] || 'Select a file to begin.'}</p>
              <div className="result"><strong>Result</strong><span>Awaiting backend connection</span></div>
            </section>
          ))}
        </div>
      </main>
    </div>
  )
}

export default App
