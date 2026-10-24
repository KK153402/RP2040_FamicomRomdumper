// nes-db (npm, data from nesdir/cartdb.xml = NesCartDB by BootGod) から日本版の事実データだけを抜き出して CSV にする
// 使い方: node build_db.mjs <nes-db の dist/esm/nesdb.js> > famicom_db.csv
import { pathToFileURL } from 'node:url';
const db = (await import(pathToFileURL(process.argv[2]).href)).default;
const kb = s => s ? parseInt(s) : 0;                       // "256k" -> 256
const q = s => /[",\n]/.test(s) ? `"${String(s).replace(/"/g, '""')}"` : (s ?? '');
const rows = new Map();
for (const g of db.games.filter(x => x.region === 'Japan')) {
  for (const c of g.cartridge) {
    const b = c.board;
    const prg = b.prg || [], chr = b.chr || [];
    const mir = b.pad ? (b.pad.h ? 'V' : b.pad.v ? 'H' : '') : 'M';   // pad h 短絡 = 垂直ミラー、パッドなし = マッパー制御
    const key = c.crc;
    const r = rows.get(key);
    if (r) { if (!r.pcb.includes(b.pcb)) r.pcb.push(b.pcb); continue; }
    rows.set(key, {
      crc: c.crc,
      prg_crc: prg.map(p => p.crc).join('+'),
      chr_crc: chr.map(p => p.crc).join('+'),
      prg_kb: prg.reduce((a, p) => a + kb(p.size), 0),
      chr_kb: chr.reduce((a, p) => a + kb(p.size), 0),
      chr_ram_kb: kb(b.vram?.size),
      mapper: b.mapper, board: b.type, pcb: [b.pcb],
      mirroring: mir,
      battery: b.wram?.battery ? 1 : 0,
      wram_kb: kb(b.wram?.size),
      title_ja: g.altname || '', title_en: g.name, catalog: g.catalog || '',
      revision: c.revision || '', publisher: g.publisher || '', date: g.date || '',
    });
  }
}
const cols = ['crc','prg_crc','chr_crc','prg_kb','chr_kb','chr_ram_kb','mapper','board','pcb','mirroring','battery','wram_kb','title_ja','title_en','catalog','revision','publisher','date'];
console.log(cols.join(','));
for (const r of [...rows.values()].sort((a, b) => a.title_en.localeCompare(b.title_en)))
  console.log(cols.map(k => q(k === 'pcb' ? r.pcb.join('|') : r[k])).join(','));
