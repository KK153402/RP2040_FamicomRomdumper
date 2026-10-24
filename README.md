# Famicom ROM Dumper rev2（AE-RP2040）

ファミコンカセットを USB 経由で吸い出して `.nes` ファイルに保存するダンパーの、rev2 基板用ファームウェアと PC 側スクリプト。

| 項目 | 内容 |
|---|---|
| 基板 | rev2（MCU：秋月 AE-RP2040、カート電源：TPS2051C）→ [hardware.md](hardware.md) |
| ファーム | v4.4（arduino-pico / PlatformIO） |
| PC スクリプト | `tools/dump_nes.py` v3.1（Python 3、pyserial） |
| プロトコル | [protocol.md](protocol.md) |
| 対応カセット | Mapper 0（NROM）、1（MMC1）、2（UNROM）、3（CNROM）、4（MMC3）は実機確認済み。22（VRC2a）、206（Namco 108 系）は実装のみ（未確認） |
| 実績 | スーパーマリオブラザーズ：CRC32 `D445F698`、Mesen で起動確認 |

## フォルダ構成

| パス | 内容 |
|---|---|
| `platformio.ini` | PlatformIO 設定 |
| `src/main.cpp` | ファームウェア |
| `tools/dump_nes.py` | 吸い出しスクリプト |
| `firmware.uf2` | ビルド済みファーム（ドラッグ＆ドロップ書き込み用） |
| `protocol.md` | シリアルコマンド仕様 |

---

## 1. 開発環境（PlatformIO）

1. VS Code に PlatformIO IDE 拡張を入れる
2. PIO Home → Platforms → Advanced Installation で `https://github.com/maxgerhardt/platform-raspberrypi.git` をインストール
3. このフォルダを開く（初回は toolchain 等のダウンロードに数分かかる）

`platformio.ini`：

```ini
[env:ae-rp2040]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git
board = pico                      ; AE-RP2040 は Pico と同じフラッシュ(2MB)・12MHz 水晶
framework = arduino
board_build.core = earlephilhower ; arduino-pico (Earle Philhower 版)
monitor_speed = 115200            ; USB CDC なので実際の速度とは無関係

; Windows でドライバ不要にするため、RPI-RP2 ドライブへのコピー方式で書き込む
upload_protocol = mbed
upload_port = D:                  ; RPI-RP2 に割り当てられたドライブ文字
```

- `board = rpipico`（「Pico (Raspberry Pi)」）を選んだ場合は `board_build.core` の行は不要
- `platform_packages` でコアのバージョンを古いもの（2.x 等）に固定しないこと（v4.1 以降は 5.x 系で確認）
- Windows でプラットフォームのインストール時に git のエラーが出たら、管理者権限で `git config --system core.longpaths true` を実行してから再インストール

## 2. 書き込み

### 通常（Upload ボタン）

PlatformIO の **Upload（→）** を押す。ファームが動いていれば、1200 bps タッチで自動的に BOOTSEL に入り、`RPI-RP2` ドライブへ `firmware.uf2` がコピーされる。

### 初回・ファームが止まったとき

1. **BOOT を押したまま RUN を押して離し、BOOT を離す** → `RPI-RP2` ドライブが現れる
2. `firmware.uf2`（またはビルド後の `.pio/build/ae-rp2040/firmware.uf2`）を `RPI-RP2` ドライブの中にドラッグ＆ドロップ
3. ドライブが自動で消え、ファームが起動する（「取り外された」等の警告は無視してよい）
4. デバイスマネージャーの「ポート（COM と LPT）」に `USB シリアル デバイス (COMx)` が出れば成功

`RPI-RP2` ドライブは書き込み用の仮想ドライブで、コピーしたファイルは中に残らない（常に `INDEX.HTM` と `INFO_UF2.TXT` だけが見える）。

### 書き込みで出るエラー

| メッセージ | 原因 | 対処 |
|---|---|---|
| `Please specify upload_port` | 書き込み先のドライブが見つからない | `upload_port` に `RPI-RP2` のドライブ文字を指定 |
| `picotool was unable to connect. You may need to install a driver via Zadig` | 既定の picotool 方式は Windows ではドライバが必要 | `upload_protocol = mbed`（上記）にする。または Zadig で **RP2 Boot (Interface 1)** に WinUSB を入れる（**Interface 0 は選ばない**。RPI-RP2 ドライブが見えなくなる） |
| デバイスマネージャーに「RP2 Boot」（！マーク） | BOOTSEL 中の picotool 用インターフェース。ドライバ未導入なだけ | 上と同じ。mbed 方式なら無視してよい |

書き込み中・BOOTSEL 中は GPIO がすべて入力になり、基板のプル抵抗でカート電源 OFF・バス Hi-Z に固定される。カセットを挿したままでも安全。

---

## 3. 動作確認

シリアルモニタ（VS Code の Serial Monitor、Tera Term 等）で行う。ポート欄の ↻ で COM を更新して選ぶ。ボーレートは何でもよい（1200 以外）。

### 3.1 カセットなし

| 操作 | 期待値 | 確認していること |
|---|---|---|
| USB 接続 | D1（+5V）点灯 | 電源 |
| `Q` | `VCC_CART=0〜数十mV  /FLT=1  power=OFF  BUS_EN_N=1` | 起動時の安全状態 |
| `O 1` | `PWR ON vcart=4500〜5100mV`、D2（VCC_CART）・D3（STATUS）点灯 | TPS2051C、VCART_SENSE |
| `T` | 12 行すべて `OK`、最後に `LOOPBACK OK` | データバス 16 本と 595/165/U10 の経路 |
| `R` | `CPU D=FF  PPU D=FF` | プルアップ R1〜R16 |
| `G` | `CIRAM_A10=1  CIRAM_CE_N=1` | プルアップ R33/R34、U11/U12 |
| `O 0` | `PWR OFF vcart=450〜500mV`（警告なし）、D2・D3 消灯 | 放電 |

### 3.2 カセットあり

**カセットの抜き差しは必ず D2（VCC_CART）が消えている状態で行う。** 動作確認が済むまでは USB も抜いておくとより安全。

| 操作 | 期待値（スーパーマリオブラザーズ） | 確認していること |
|---|---|---|
| `O 1` | `PWR ON vcart=4900mV 前後` | カセットの消費電流で極端に下がらないか |
| `V` | `NMI $8082`、`RESET $8000`、`IRQ $FFF0`、3 行とも `[OK]` | PRG 側（CPU アドレス、/ROMSEL、M2） |
| `P FFF0 16` | `FFF0: 19 19 1A 1A 1C 1D 1D 1E 1E 1F 82 80 00 80 F0 FF` | 同上 |
| `H 0000 16` | `0000: 03 0F 1F 1F 1C 24 26 66 00 00 00 00 1F 3F 3F 7F` | CHR 側（PPU アドレス、/RD） |
| `M` | `VERTICAL (follows PA10)` | CIRAM A10 |
| `O 0` | `PWR OFF` | |

`V` が `$FFFF [NG] open bus` のときはカセットが何も出力していない（未挿入・接触不良）。

### 3.3 吸い出し

**シリアルモニタを閉じてから**実行する（COM ポートは 1 つのアプリしか開けない）。

```
python -m pip install pyserial
python tools/dump_nes.py smb.nes
```

出力例（tool v3。`xxxxxxxx` は実測値が入る）：

```
ポート: COM8   マッパー: 0 (NROM)
  PWR ON vcart=4951mV
  [1] PRG 32 KB, CHR 8 KB, mirroring V, CRC32 PRG xxxxxxxx / CHR xxxxxxxx / PRG+CHR D445F698
      所要 1.0 s（うちカセット読み出し 0.9 s）
  [2] PRG 32 KB, CHR 8 KB, mirroring V, CRC32 PRG xxxxxxxx / CHR xxxxxxxx / PRG+CHR D445F698
      所要 1.0 s（うちカセット読み出し 0.9 s）
  2回読み 合計 2.0 s
  PWR OFF vcart=466mV
保存しました: smb.nes
```

- `pip install serial` ではなく **`pyserial`**。仮想環境を使っている場合は `python -m pip` で入れる
- ファーム v4.0 以前（`B` コマンドなし）の場合は `--legacy` を付ける

### 3.4 マッパー付きカセットの吸い出し [tool v3 / fw v4.3]

カセットにはマッパー情報が書かれていないので、**NesCartDB で日本版（日の丸の行）の iNES Mapper を調べて `--mapper` で指定**する。容量（バンク数）と CHR-RAM かどうかは自動で判定する。

```
python tools/dump_nes.py dq2.nes --mapper 2
```

| `--mapper` | マッパー | 主な基板 | 検証用カセット |
|---|---|---|---|
| 0（省略可） | NROM | HVC-NROM | スーパーマリオブラザーズ（確認済み） |
| 1 | MMC1 | HVC-SxROM（SLROM、SGROM、SNROM…） | 忍者龍剣伝（HVC-SLROM） |
| 2 | UNROM | HVC-UNROM、HVC-UOROM | ドラゴンクエストII（HVC-UNROM） |
| 3 | CNROM | HVC-CNROM | ソロモンの鍵（HVC-CNROM） |
| 4 | MMC3 | HVC-TxROM（TLROM、TSROM、TGROM…） | スーパーマリオブラザーズ3（HVC-TSROM） |
| 22 | VRC2a（コナミ） | 351618 など | がんばれペナントレース |
| 206 | Namco 108 系 | NAMCOT-34xx | ドラゴンバスターII |

確認のしかた：

1. 2 回の読み出しが一致して保存されること
2. 表示される `PRG` / `CHR` / `PRG+CHR` の CRC32 が、NesCartDB の ROM Details（PRG0 / CHR0 / ROMs Combined）と一致すること。CHR-RAM のカセットは PRG だけ比べる
3. Mesen で起動すること

CRC32 が合わないときは `--banks` を付けると、PRG は 8 KB、CHR は 1 KB ごとの CRC32 も表示する。版違いか、特定のバンクだけずれているかの切り分けに使う。

| 注意 | 理由 |
|---|---|
| マッパー番号を間違えると `固定バンクと一致するバンクが見つかりません` 等で止まる | 容量判定が成立しないため。保存はされない |
| **バッテリーバックアップ付き（セーブできる）カセットは、セーブデータが消えてもよいものだけにする** | 本ツールは `$6000`〜`$7FFF`（PRG-RAM）には書かないが、抜き差し時の電源の立ち上がり・立ち下がりで消えることがある（実機と同じリスク） |
| MMC1 の PRG 512 KB（SUROM：ドラゴンクエストIV 等）、MMC3 の 4 画面基板は未対応 | バンク切替の追加ビットが必要 |

所要時間の目安（実測の PRG 18.6 µs/B、CHR 35 µs/B から計算。USB 転送とバンク切替の往復を除く）：

| カセット | 容量 | 1 回の読み出し |
|---|---|---|
| ドラゴンクエストII | PRG 128 KB | 約 2.4 s |
| 忍者龍剣伝 | PRG 128 KB + CHR 128 KB | 約 7 s |
| スーパーマリオブラザーズ3 | PRG 256 KB + CHR 128 KB | 約 9.5 s |

---

## 4. 吸い出しがうまくいかないとき

| 症状 | 考えられる原因 | 対処 |
|---|---|---|
| `RP2040 のポートが見つかりません` | ファーム未書き込み、または BOOTSEL のまま | 2. の手順で書き込む |
| `応答がありません` | シリアルモニタが開いたまま | モニタを閉じる |
| `PWR NG` | カセットの短絡・逆挿し、または USB の電源不足 | カセットを確認。USB ハブを使わず PC に直結 |
| `ERR overcurrent` | 吸い出し中に過電流（/FLT） | カセットを抜いて `Q`、`O 1` で再確認 |
| `2回のダンプが一致しません` | カセットの接触不良 | 端子を清掃。それでも NG なら `S 125 1000`（アクセス待ちを延ばす）→ `S 500 1000`（クロックを下げる） |
| `転送エラー` が再送しても直らない | USB ケーブル・ハブ | 別のケーブル、PC の別のポートで試す |

---

## 5. 変更履歴

| 版 | 内容 |
|---|---|
| tool v3.1 | マッパー 22（VRC2a）、206（Namco 108 系）を追加。`--banks`（バンクごとの CRC32 表示）を追加 |
| fw v4.4 | `B P` で `$6000`〜`$7FFF`（PRG-RAM、セーブデータ）を読めるようにした。`$8000` 未満は /ROMSEL を H のまま読む |
| fw v4.3 | MMC1 で書き込みが無視される問題を修正（`W` の前にダミー読み出しサイクルを追加） |
| fw v4.2 / tool v3 | マッパー対応（MMC1 / UNROM / CNROM / MMC3）。CPU 書き込み `W`、PPU 書き込み `X` を追加。容量・CHR-RAM の自動判定、バス競合の回避、PRG / CHR / 合計の CRC32 表示 |
| fw v4.1 / tool v2 | 高速化：SIO 直接操作＋ns 単位のタイミング（シフトクロック 4 MHz）、PRG はアドレス 16 bit・データ 8 bit だけ転送。ブロック読み出し `B`（ブロック CRC、再送）、タイミング設定 `S`（ns 単位）を追加 |
| fw v4.0 / tool v1.2 | Windows で受信データが欠ける問題を修正（`writeAll()`、pySerial の `timeout` を途中で変更しない） |
| fw v4.0 | rev2 初版。v3 を AE-RP2040 に移植。カート電源制御 `O`、状態表示 `Q`、折り返し試験 `T`、過電流監視を追加。電圧を mV 表示に。ベクタ `$FFFF` を NG 判定に |
| v3（rev1） | ESP32 版。ビットバング、115200 bps |
