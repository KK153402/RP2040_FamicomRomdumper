#!/usr/bin/env python3
"""Famicom ROM Dumper rev2 (AE-RP2040) - ファミコンカセットを .nes に吸い出す  (tool v3.1)

使い方:
    python dump_nes.py 出力ファイル名.nes [--mapper N] [--port COM5]

    --mapper  0 = NROM（既定）
              1 = MMC1  (SxROM: SLROM, SGROM, SNROM など。PRG 256 KB まで)
              2 = UNROM (UNROM / UOROM)
              3 = CNROM
              4 = MMC3  (TxROM: TLROM, TSROM, TGROM など)
             22 = VRC2a (コナミ。PCB 351618 など)
            206 = Namco 108 系 (NAMCOT-34xx。チップ 108/109/118/119)
    --port    省略時は RP2040 (USB VID 0x2E8A) を自動検出
    --banks   8 KB（CHR は 1 KB）ごとの CRC32 も表示する（NesCartDB と合わないときの切り分け用）
    --legacy  v3 形式の D コマンドで吸い出す（NROM のみ、fw v4.0 以前用）

  必要なライブラリ: pyserial  (python -m pip install pyserial)
  ファーム: v4.3 以降（W / X コマンドが必要。MMC1 は v4.3 必須）

カセットにマッパー情報は書かれていないので、マッパー番号は NesCartDB 等で調べて指定する。
PRG/CHR の容量はバンクの折り返しから自動で判定する。CHR-RAM かどうかも書き込み試験で自動判定する。
"""
import argparse
import sys
import time
import zlib

import serial
import serial.tools.list_ports

RP2040_VID = 0x2E8A
KB = 1024


class DumpError(Exception):
    pass


# ======================================================================
# シリアル
# ======================================================================
def find_port():
    ports = [p for p in serial.tools.list_ports.comports() if p.vid == RP2040_VID]
    if not ports:
        raise DumpError("RP2040 のポートが見つかりません。USB接続と書き込みを確認してください (--port で指定も可)")
    if len(ports) > 1:
        names = ", ".join(p.device for p in ports)
        raise DumpError(f"RP2040 が複数見つかりました ({names})。--port で指定してください")
    return ports[0].device


def open_port(name):
    # ボーレートは USB CDC では無関係。ただし 1200 を指定すると BOOTSEL に入るので使わない。
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = 115200
    ser.timeout = 0.5   # 以後は変更しない（Windows では変更のたびに SetCommState が出て受信を取りこぼすため）
    ser.dtr = True      # arduino-pico は DTR が立っていないと送信データを捨てる
    ser.rts = True
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def readline(ser, timeout=5.0):
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            continue
        buf += c
        if c == b"\n":
            return buf.decode("ascii", errors="replace").strip()
    raise DumpError("応答がありません（タイムアウト）")


def command(ser, cmd, expect_prefix, timeout=5.0):
    """コマンドを送り、expect_prefix で始まる行が来るまで読む（途中の案内表示は読み飛ばす）"""
    ser.write((cmd + "\n").encode("ascii"))
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = readline(ser, timeout)
        if line.startswith("ERR"):
            raise DumpError(f"'{cmd}' → {line}")
        if any(line.startswith(p) for p in expect_prefix):
            return line
    raise DumpError(f"'{cmd}' の応答が来ません")


def read_exact(ser, n, timeout=30.0):
    deadline = time.time() + timeout
    data = bytearray()
    while len(data) < n and time.time() < deadline:
        data += ser.read(min(4096, n - len(data)))
    if len(data) != n:
        raise DumpError(f"受信バイト不足 ({len(data)}/{n})")
    return bytes(data)


# ======================================================================
# ダンパーの基本操作
# ======================================================================
class Dumper:
    def __init__(self, ser):
        self.ser = ser
        self.read_us = 0          # カセットからの読み出し時間の合計（USB 転送を除く）

    def block(self, kind, addr, length, retries=3):
        """B コマンドで 1 ブロック読む。ブロック CRC が合わなければ再送"""
        for attempt in range(1, retries + 1):
            hdr = command(self.ser, f"B {kind} {addr:04X} {length:04X}", ("BLK",), timeout=30)
            f = dict(kv.split("=") for kv in hdr.split()[1:])
            n, crc_dev, us = int(f["n"]), int(f["crc"], 16), int(f["us"])
            data = read_exact(self.ser, n)
            if n == length and (zlib.crc32(data) & 0xFFFFFFFF) == crc_dev:
                self.read_us += us
                return data
            print(f"    転送エラー ({kind} ${addr:04X}, {attempt}回目)、再送します")
        raise DumpError(f"ブロック {kind} ${addr:04X} を {retries} 回読んでも転送エラーになります")

    def prg(self, addr, length):
        return self.block("P", addr, length)

    def chr(self, addr, length):
        return self.block("C", addr, length)

    def cpu_write(self, addr, val):
        command(self.ser, f"W {addr:04X} {val & 0xFF:02X}", ("OK",))

    def ppu_write(self, addr, val):
        command(self.ser, f"X {addr:04X} {val & 0xFF:02X}", ("OK",))

    def mirroring(self):
        line = command(self.ser, "M", ("CIRAM A10",))
        return "V" if "VERTICAL" in line else "H" if "HORIZONTAL" in line else "?"

    def chr_is_ram(self, addr=0x0000, n=8):
        """CHR 領域に反転値を書いて読み戻す。変われば RAM。元の値に戻して終わる"""
        orig = self.chr(addr, n)
        for i, b in enumerate(orig):
            self.ppu_write(addr + i, b ^ 0xFF)
        after = self.chr(addr, n)
        for i, b in enumerate(orig):
            self.ppu_write(addr + i, b)
        return after == bytes(b ^ 0xFF for b in orig)


def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def find_value(data, base, value):
    """バス競合回避: ROM 上で value と同じ値が入っている番地を探す"""
    i = data.find(bytes([value]))
    if i < 0:
        raise DumpError(f"バス競合を避けられません: 値 ${value:02X} が ROM 内に見つかりません")
    return base + i


def banks_until_fixed(read_bank, fixed, max_banks, name):
    """最後のバンクが固定されているマッパー用。バンク i が固定バンクと一致したら i+1 個"""
    banks = []
    for i in range(max_banks):
        b = read_bank(i)
        banks.append(b)
        if b == fixed and is_pow2(i + 1):
            return banks
    raise DumpError(f"{name}: 固定バンクと一致するバンクが見つかりません（マッパー番号の指定を確認）")


def banks_until_wrap(read_bank, max_banks):
    """固定バンクのない領域用。2 のべき乗番目で先頭 2 バンクと一致したら折り返しとみなす"""
    banks = []
    i = 0
    while i < max_banks:
        b = read_bank(i)
        if i >= 2 and is_pow2(i) and b == banks[0]:
            b1 = read_bank(i + 1) if i + 1 < max_banks else None
            if b1 is None or b1 == banks[1]:
                return banks
            banks += [b, b1]
            i += 2
            continue
        banks.append(b)
        i += 1
    return banks


# ======================================================================
# マッパーごとの吸い出し。戻り値: (prg, chr, mirroring)  chr が空なら CHR-RAM
# ======================================================================
def dump_nrom(d):
    mir = d.mirroring()
    prg = d.prg(0x8000, 0x8000)
    if prg[:0x4000] == prg[0x4000:]:                 # NROM-128
        prg = prg[0x4000:]
    chr_ = b"" if d.chr_is_ram() else d.chr(0x0000, 0x2000)
    return prg, chr_, mir


def dump_unrom(d):
    # $8000-$BFFF: 16 KB 切替（$8000-$FFFF への書き込み）、$C000-$FFFF: 最終バンク固定
    mir = d.mirroring()
    fixed = d.prg(0xC000, 0x4000)

    def bank(i):
        d.cpu_write(find_value(fixed, 0xC000, i), i)  # 固定バンク内の同じ値の番地に書く
        return d.prg(0x8000, 0x4000)

    prg = b"".join(banks_until_fixed(bank, fixed, 16, "UNROM"))
    chr_ = b"" if d.chr_is_ram() else d.chr(0x0000, 0x2000)
    return prg, chr_, mir


def dump_cnrom(d):
    # PRG 32 KB 固定、CHR 8 KB 切替（$8000-$FFFF への書き込み、バス競合あり）
    mir = d.mirroring()
    prg = d.prg(0x8000, 0x8000)

    def bank(i):
        d.cpu_write(find_value(prg, 0x8000, i), i)
        return d.chr(0x0000, 0x2000)

    chr_ = b"".join(banks_until_wrap(bank, 4))
    if prg[:0x4000] == prg[0x4000:]:
        prg = prg[0x4000:]
    return prg, chr_, mir


def dump_mmc1(d):
    # バス競合なし。レジスタは $8000-$FFFF への 5 回のシリアル書き込み（bit0 から）
    def reg(addr, val):
        for i in range(5):
            d.cpu_write(addr, (val >> i) & 1)

    d.cpu_write(0x8000, 0x80)                        # シフトレジスタのリセット
    reg(0x8000, 0x1E)                                # 制御: CHR 4KB モード, PRG $C000 固定モード, 垂直
    fixed = d.prg(0xC000, 0x4000)

    def prg_bank(i):
        reg(0xE000, i)                               # PRG バンク（bit4=0: PRG-RAM 有効のまま）
        return d.prg(0x8000, 0x4000)

    prg = b"".join(banks_until_fixed(prg_bank, fixed, 16, "MMC1"))

    reg(0xA000, 0)
    if d.chr_is_ram():
        chr_ = b""
    else:
        def chr_bank(i):
            reg(0xA000, i)                           # CHR バンク 0（$0000-$0FFF, 4 KB）
            return d.chr(0x0000, 0x1000)
        chr_ = b"".join(banks_until_wrap(chr_bank, 32))
    return prg, chr_, "H"                            # ミラーリングはマッパーが制御（ヘッダ値は無視される）


def _mmc3_like(d, name, prg_max, chr_max):
    """MMC3 と Namco 108 系の共通手順。$8000 = バンク選択、$8001 = バンク番号"""
    fixed = d.prg(0xE000, 0x2000)                    # $E000-$FFFF は最終 8 KB に固定

    def prg_bank(i):
        d.cpu_write(0x8000, 0x06)                    # R6: $8000-$9FFF（MMC3 は PRG モード 0）
        d.cpu_write(0x8001, i)
        return d.prg(0x8000, 0x2000)

    prg = b"".join(banks_until_fixed(prg_bank, fixed, prg_max, name))

    d.cpu_write(0x8000, 0x00)
    d.cpu_write(0x8001, 0x00)
    if d.chr_is_ram():
        return prg, b""

    def chr_bank(i):
        d.cpu_write(0x8000, 0x00)                    # R0: $0000-$07FF（2 KB, 下位ビットは無視される）
        d.cpu_write(0x8001, i * 2)
        return d.chr(0x0000, 0x0800)

    return prg, b"".join(banks_until_wrap(chr_bank, chr_max))


def dump_mmc3(d):
    # バス競合なし。PRG 最大 512 KB（8 KB × 64）、CHR 最大 256 KB（2 KB × 128）
    prg, chr_ = _mmc3_like(d, "MMC3", 64, 128)
    return prg, chr_, "H"                            # ミラーリングはマッパーが制御


def dump_namco108(d):
    # Namco 108/109/118/119（iNES 206）。MMC3 のサブセット（PRG モード・CHR 反転・ミラー制御・IRQ なし）
    # PRG 最大 128 KB（R6/R7 は 4 bit）、CHR 最大 64 KB（R0/R1 は 6 bit → 2 KB × 32）
    # ミラーリングは基板の配線で固定なので M で読む
    mir = d.mirroring()
    prg, chr_ = _mmc3_like(d, "Namco 108", 16, 32)
    return prg, chr_, mir


def dump_vrc2a(d):
    # Konami VRC2a（iNES 22）。CPU A0/A1 が VRC の A1/A0 に入れ替わって配線されている
    #   $8000: PRG0（8 KB, $8000）  $A000: PRG1（8 KB, $A000）  $C000-$FFFF: 最終 16 KB 固定
    #   $B000 / $B002: CHR0（1 KB, $0000）の下位 4 bit / 上位 bit
    # VRC2a は CHR バンク値の最下位ビットを無視するので、1 KB バンク n には n << 1 を書く
    fixed = d.prg(0xE000, 0x2000)

    def prg_bank(i):
        d.cpu_write(0x8000, i)
        return d.prg(0x8000, 0x2000)

    prg = b"".join(banks_until_fixed(prg_bank, fixed, 32, "VRC2a"))

    def set_chr0(n):
        v = n << 1
        d.cpu_write(0xB000, v & 0x0F)
        d.cpu_write(0xB002, (v >> 4) & 0x0F)

    set_chr0(0)
    if d.chr_is_ram():
        return prg, b"", "H"

    def chr_bank(i):
        set_chr0(i)
        return d.chr(0x0000, 0x0400)

    chr_ = b"".join(banks_until_wrap(chr_bank, 128))  # 最大 128 KB（1 KB × 128）
    return prg, chr_, "H"                            # ミラーリングはマッパーが制御（$9000）


MAPPERS = {
    0: ("NROM", dump_nrom),
    1: ("MMC1", dump_mmc1),
    2: ("UNROM", dump_unrom),
    3: ("CNROM", dump_cnrom),
    4: ("MMC3", dump_mmc3),
    22: ("VRC2a", dump_vrc2a),
    206: ("Namco 108", dump_namco108),
}


# ======================================================================
# v3 互換（D コマンド, NROM のみ）
# ======================================================================
def dump_legacy(ser):
    hdr = command(ser, "D", ("DUMP",), timeout=60)
    f = dict(kv.split("=") for kv in hdr.split()[1:])
    prg_size, chr_size, mir = int(f["prg"]), int(f["chr"]), f["mir"]
    prg = read_exact(ser, prg_size)
    chr_ = read_exact(ser, chr_size)
    end = readline(ser)
    if not end:
        end = readline(ser)
    if not end.startswith("END crc="):
        raise DumpError(f"終端行が不正です: {end!r}")
    if int(end.split("=")[1], 16) != (zlib.crc32(prg + chr_) & 0xFFFFFFFF):
        raise DumpError("転送エラー: CRC 不一致")
    return prg, chr_, mir


# ======================================================================
def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def ines_header(prg_size, chr_size, mir, mapper):
    flags6 = ((mapper & 0x0F) << 4) | (0x01 if mir == "V" else 0x00)
    flags7 = mapper & 0xF0
    return bytes([0x4E, 0x45, 0x53, 0x1A, prg_size // (16 * KB), chr_size // (8 * KB),
                  flags6, flags7]) + bytes(8)


def describe(prg, chr_, mir):
    c = f"CHR {len(chr_) // KB} KB" if chr_ else "CHR-RAM"
    return (f"PRG {len(prg) // KB} KB, {c}, mirroring {mir}, "
            f"CRC32 PRG {crc(prg):08X} / CHR {crc(chr_):08X} / PRG+CHR {crc(prg + chr_):08X}")


def print_banks(name, data, size):
    for i in range(0, len(data), size):
        if (i // size) % 8 == 0:
            print(f"  {name} ", end="")
        print(f"{i // size:3d}:{crc(data[i:i + size]):08X}", end=" ")
        if (i // size) % 8 == 7 or i + size >= len(data):
            print()


def main():
    ap = argparse.ArgumentParser(description="Famicom dumper (rev2 / AE-RP2040) tool v3.1")
    ap.add_argument("output", help="保存する .nes ファイル名")
    ap.add_argument("--mapper", type=int, default=0, choices=sorted(MAPPERS), help="iNES マッパー番号")
    ap.add_argument("--port", help="シリアルポート (例: COM5, /dev/ttyACM0)")
    ap.add_argument("--banks", action="store_true", help="8 KB（CHR は 1 KB）ごとの CRC32 も表示")
    ap.add_argument("--legacy", action="store_true", help="v3 形式の D コマンドを使う（NROM のみ）")
    args = ap.parse_args()
    name, func = MAPPERS[args.mapper]

    try:
        port = args.port or find_port()
        print(f"ポート: {port}   マッパー: {args.mapper} ({name})")
        ser = open_port(port)
    except (DumpError, serial.SerialException) as e:
        print(f"エラー: {e}")
        return 1

    results = []
    try:
        line = command(ser, "O 1", ("PWR ON", "PWR NG"))
        print(f"  {line}")
        if line.startswith("PWR NG"):
            raise DumpError("カート電源を入れられません（カセットの挿し込み・短絡を確認）")
        t_all = time.time()
        for n in (1, 2):
            t0 = time.time()
            if args.legacy:
                r = dump_legacy(ser)
                read_s = 0
            else:
                d = Dumper(ser)
                r = func(d)
                read_s = d.read_us / 1e6
            results.append(r)
            print(f"  [{n}] {describe(*r)}")
            print(f"      所要 {time.time() - t0:.1f} s（うちカセット読み出し {read_s:.1f} s）")
        print(f"  2回読み 合計 {time.time() - t_all:.1f} s")
    except (DumpError, serial.SerialException) as e:
        print(f"エラー: {e}")
        return 1
    finally:
        try:
            print("  " + command(ser, "O 0", ("PWR OFF",)))
        except Exception:
            pass
        ser.close()

    if results[0] != results[1]:
        print("エラー: 2回のダンプが一致しません。カセットの接点を清掃して再試行してください")
        return 2

    prg, chr_, mir = results[0]
    if args.banks:
        print_banks("PRG", prg, 0x2000)
        print_banks("CHR", chr_, 0x0400)
    if mir == "?":
        print("警告: ミラーリングを判定できませんでした（水平として保存します）")
    with open(args.output, "wb") as f:
        f.write(ines_header(len(prg), len(chr_), mir, args.mapper))
        f.write(prg)
        f.write(chr_)
    print(f"保存しました: {args.output}")
    print("  NesCartDB の ROM Details（PRG0 / CHR0 / ROMs Combined の CRC32）と照合してください")
    print("カセットを抜いて大丈夫です（VCC_CART の LED (D2) が消えていることを確認）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
