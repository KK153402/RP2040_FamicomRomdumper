# famicom_db.csv（日本版カセットのデータベース）

吸い出しアプリが、CRC32 からゲーム名・マッパー・基板を引くための表。日本版（Region = Japan）の事実データだけを収録している。

## 出典

| 段 | 出典 | 備考 |
|---|---|---|
| 元データ | NesCartDB（nescartdb.com、originally by BootGod） | カセットの実物を吸い出して登録したデータベース |
| 中間 | nesdir（github.com/nesdir/nesdir）の `cartdb.xml` | BootGod 時代のデータベースの XML 版 |
| 取得元 | npm パッケージ `nes-db` 1.2.0（justjavac、コードは MIT） | `cartdb.xml` を JS に変換したもの |

アプリの「情報」画面と取扱説明書に、次のクレジットを表示する。

> Game data: NesCartDB (nescartdb.com, originally by BootGod)

## 再生成

```
npm pack nes-db
tar xzf nes-db-1.2.0.tgz
node build_db.mjs package/dist/esm/nesdb.js > famicom_db.csv
```

## 列

| 列 | 内容 | 例 |
|---|---|---|
| `crc` | PRG+CHR（ヘッダなし）の CRC32。**照合のキー** | `D445F698` |
| `prg_crc` / `chr_crc` | PRG / CHR の CRC32。ROM が複数個のときは `+` でつなぐ | `5CF548D3` |
| `prg_kb` / `chr_kb` | PRG / CHR-ROM の容量（KB）。CHR-RAM のカセットは `chr_kb` = 0 | `32` |
| `chr_ram_kb` | CHR-RAM の容量（KB） | `8` |
| `mapper` | iNES マッパー番号 | `0` |
| `board` / `pcb` | 基板の種類 / 基板の型番（同じ CRC で複数あれば `\|` でつなぐ） | `HVC-NROM-256` |
| `mirroring` | `V` 垂直、`H` 水平（基板のはんだパッドで固定）、`M` マッパー制御 | `V` |
| `battery` | バッテリーバックアップの有無（1 / 0） | `0` |
| `wram_kb` | PRG-RAM（WRAM）の容量（KB） | `8` |
| `title_ja` / `title_en` | タイトル（日本語 / ローマ字） | `スーパーマリオブラザーズ` |
| `catalog` / `revision` / `publisher` / `date` | 型番 / 版 / 発売元 / 発売日 | `HVC-SM` |

- 同じ CRC32 の行は 1 行にまとめている（基板の違いだけのものは `pcb` に並べる）
- ミラーリングは、元データの `pad h="1"`（H パッド短絡）を垂直ミラー、`pad v="1"` を水平ミラーとしている（スーパーマリオブラザーズ＝垂直、ソロモンの鍵＝水平で確認）

## 収録数（2026-10 時点）

| 項目 | 件数 |
|---|---|
| 全体（日本版、CRC 単位） | 929 |
| 対応マッパー（0 / 1 / 2 / 3 / 4 / 22 / 206） | 696 |
| うちバッテリーなし | 531 |
| バッテリーあり（全マッパー） | 200 |

## 実機の吸い出しとの照合

| カセット | `crc` | 結果 |
|---|---|---|
| スーパーマリオブラザーズ | `D445F698` | 一致 |
| ソロモンの鍵 | `D821A1C6` | 一致 |
| ドラゴンクエストII | `48349B0B` | 一致 |
| 忍者龍剣伝 | `D7CB398F` | 一致 |
| スーパーマリオブラザーズ3（初版） | `E7DDFEE3` | 一致 |
| ドラゴンバスターII | `D7AA0B6D` | 一致 |
| がんばれペナントレース | `90F6FA33` | 一致 |
