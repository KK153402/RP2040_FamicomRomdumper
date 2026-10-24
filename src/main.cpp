// Famicom ROM Dumper rev2 - AE-RP2040 firmware v4.2 (hardware.md rev2 準拠)
// USB CDC (ボーレート指定は無関係)。"?" でコマンド一覧。
//
// v4.2（マッパー対応）
//   - CPU 書き込み W / PPU 書き込み X を追加（バンク切替・CHR-RAM 判定用）
//     バンク切替の手順は PC 側（dump_nes.py）が持つ
//
// v4.1（高速化）
//   - シフトレジスタ駆動を SIO 直接操作＋ns 単位の待ちに変更（既定: 半周期 125 ns = 4 MHz）
//     根拠: CD74HCT165 tpd(CLK→QH) max 40 ns, fMAX min 22 MHz (VCC 4.5 V, -40〜85°C)
//   - PRG 読み出しは CPU アドレス 16 bit だけシフト、165 も CPU D の 8 bit だけ読む
//   - ブロック読み出し B コマンド（バイナリ＋ブロック CRC＋所要時間）を追加
//
// v4（rev2 初版）: rev1(ESP32) v3 からの変更点
//   - ピン割り当てを GP2〜GP20, GP26 に変更（hardware.md §5.2）
//   - カート電源 TPS2051C の制御を追加（O コマンド、hardware.md §9.5 の電源シーケンス）
//   - /FLT（過電流）と VCART_SENSE（ADC）の監視を追加
//   - 165 の読み出しは v3 と同じく「QH を読んでからクロック」のビットバング
#include <Arduino.h>
#include <stdarg.h>
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "pico/platform.h"

// ---- ピン割り当て（hardware.md §5.2）----
constexpr uint PIN_SCK       = 2;
constexpr uint PIN_PL_165_N  = 3;
constexpr uint PIN_RCLK_ADDR = 4;
constexpr uint PIN_RCLK_DATA = 5;
constexpr uint PIN_MOSI      = 6;
constexpr uint PIN_MISO      = 7;
constexpr uint PIN_M2        = 8;
constexpr uint PIN_ROMSEL_N  = 9;
constexpr uint PIN_RW        = 10;
constexpr uint PIN_PPU_RD_N  = 11;
constexpr uint PIN_PPU_WR_N  = 12;
constexpr uint PIN_PPU_A13_N = 13;
constexpr uint PIN_OE_DATA_N = 14;
constexpr uint PIN_BUS_EN_N  = 15;
constexpr uint PIN_CIRAM_A10 = 16;
constexpr uint PIN_CIRAM_CE_N= 17;
constexpr uint PIN_CART_PWR_EN = 18;
constexpr uint PIN_CART_FLT_N  = 19;
constexpr uint PIN_STATUS_LED       = 20;
constexpr uint PIN_VCART_SENSE = 26;   // ADC0

// ---- 電源判定しきい値（hardware.md §6.5 / §9.5）----
constexpr float VCART_ON_MIN  = 4.0f;  // ON 判定 (ADC 2.0 V)
constexpr float VCART_OFF_MAX = 0.5f;  // 放電完了判定 (ADC 0.25 V)

// ---- バス駆動タイミング（S コマンドで変更可）----
uint32_t clkHalfNs = 125;              // シフトクロック H/L 各幅 [ns]
uint32_t accessNs  = 500;              // アドレス確定→データ取り込みまでの待ち [ns]（マスクROM tACC 150〜250 ns に余裕）
static uint32_t clkCyc, accCyc, cpuMHz;
bool cartPower = false;

static void updateTiming() {
  cpuMHz = clock_get_hz(clk_sys) / 1000000;
  clkCyc = clkHalfNs * cpuMHz / 1000;
  accCyc = accessNs * cpuMHz / 1000;
}

// ---------------------------------------------------------------
// グリッチなしで出力に切り替える（gpio_init は出力値を 0 にリセットするため、
// 値を書いてから方向を出力にする）
static void outInit(uint pin, bool level) {
  gpio_init(pin);
  gpio_put(pin, level);
  gpio_set_dir(pin, GPIO_OUT);
}
static inline void put(uint pin, bool v) { gpio_put(pin, v); }
static inline bool get(uint pin) { return gpio_get(pin); }

static int mV(float v) { return (int)(v * 1000.0f + 0.5f); }   // printf の %f は使えないため

float readVcart() {                    // R35/R36 = 1/2 分圧
  uint32_t s = 0;
  for (int i = 0; i < 8; i++) s += analogRead(PIN_VCART_SENSE);
  return (s / 8.0f) * 3.3f / 4095.0f * 2.0f;
}

// ---- 高速 I/O（SIO 直接）-------------------------------------------
#define BIT(p) (1u << (p))
static inline void hi(uint p)  { sio_hw->gpio_set = BIT(p); }
static inline void lo(uint p)  { sio_hw->gpio_clr = BIT(p); }
static inline void waitCyc(uint32_t c) { if (c) busy_wait_at_least_cycles(c); }

// MSB first で n bit 送る。SCK 立ち上がりで 595 がシフト（165 も同時にシフトするが無害）
static void __not_in_flash_func(shiftOutBits)(uint32_t v, int n) {
  for (int i = n - 1; i >= 0; i--) {
    if ((v >> i) & 1) hi(PIN_MOSI); else lo(PIN_MOSI);
    waitCyc(clkCyc);                   // SER→SCK セットアップ
    hi(PIN_SCK);
    waitCyc(clkCyc);
    lo(PIN_SCK);
  }
}

// QH を読んでからクロック（hardware.md §13-15）。n bit 読む
static uint32_t __not_in_flash_func(shiftInBits)(int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    waitCyc(clkCyc);                   // 前の立ち上がりから 2 半周期 ≥ tpd 40 ns + 1T45 + 入力同期
    v = (v << 1) | ((sio_hw->gpio_in >> PIN_MISO) & 1);
    hi(PIN_SCK);
    waitCyc(clkCyc);
    lo(PIN_SCK);
  }
  return v;
}

static inline void pulse(uint p) { hi(p); waitCyc(clkCyc); lo(p); }

static inline void loadPL() {          // 165 にパラレルロード
  lo(PIN_PL_165_N);
  waitCyc(clkCyc);                     // tw(SH/LD) ≥ 25 ns
  hi(PIN_PL_165_N);
  waitCyc(clkCyc);                     // tREC ≥ 25 ns, tpd(SH/LD→QH) ≤ 40 ns
}

// 互換用（バイト単位 API）
void shiftOutByte(uint8_t b) { shiftOutBits(b, 8); }
uint8_t shiftInByte() { return shiftInBits(8); }

void idleBus() {                       // 全制御線を非アクティブ側へ
  put(PIN_M2, 0);
  put(PIN_ROMSEL_N, 1);
  put(PIN_RW, 1);
  put(PIN_PPU_RD_N, 1);
  put(PIN_PPU_WR_N, 1);
  put(PIN_PPU_A13_N, 1);
}

// 4 個すべて（U4 U3 U2 U1 の順に送る）
void setAddr(uint16_t cpuA, uint16_t ppuA) {
  uint32_t w = ((uint32_t)(ppuA & 0x3FFF) << 16) | (cpuA & 0x7FFF);
  shiftOutBits(w, 32);
  pulse(PIN_RCLK_ADDR);
}

// 165 に現在のバス値をロードして 16bit 読む。上位 = CPU D, 下位 = PPU D
uint16_t read165() {
  loadPL();
  return (uint16_t)shiftInBits(16);    // U7 (CPU D) が先
}

// PRG 1 バイト。CPU アドレス (U1/U2) だけ 16 bit シフトする。
// U3/U4 (PPU アドレス) には前の値が押し出されて入るが、PPU /RD=1 なので CHR 側は出力しない。
// v4.4: $6000-$7FFF（PRG-RAM）も読める。$8000 未満は /ROMSEL を H のまま（M2 = H、R/W = H の読み出しサイクル）
uint8_t __not_in_flash_func(readPrg)(uint16_t addr) {   // addr: $6000-$FFFF
  shiftOutBits(addr & 0x7FFF, 16);
  pulse(PIN_RCLK_ADDR);
  hi(PIN_RW);
  hi(PIN_M2);
  if (addr & 0x8000) lo(PIN_ROMSEL_N);
  waitCyc(accCyc);
  loadPL();
  uint8_t v = shiftInBits(8);          // U7 = CPU D だけ読む
  hi(PIN_ROMSEL_N);
  lo(PIN_M2);
  return v;
}

uint8_t __not_in_flash_func(readChr)(uint16_t addr) {   // addr: $0000-$1FFF
  setAddr(0x0000, addr & 0x1FFF);      // PA13 = 0
  hi(PIN_PPU_A13_N);
  lo(PIN_PPU_RD_N);
  waitCyc(accCyc);
  loadPL();
  uint8_t v = shiftInBits(16) & 0xFF;  // U7 の 8 bit を送り出してから U8 = PPU D
  hi(PIN_PPU_RD_N);
  return v;
}

// ---- カート電源（hardware.md §9.5）---------------------------------
void cartOff() {
  put(PIN_OE_DATA_N, 1);
  idleBus();
  put(PIN_BUS_EN_N, 1);
  put(PIN_CART_PWR_EN, 0);
  cartPower = false;
  uint32_t t0 = millis();
  float v;
  while ((v = readVcart()) >= VCART_OFF_MAX && millis() - t0 < 200) delay(1);
  put(PIN_STATUS_LED, 0);
  Serial.printf("PWR OFF vcart=%dmV%s\n", mV(v),
                v >= VCART_OFF_MAX ? " (WARN: not discharged)" : "");
}

bool cartOn() {
  put(PIN_OE_DATA_N, 1);
  idleBus();
  put(PIN_BUS_EN_N, 1);
  put(PIN_CART_PWR_EN, 1);
  delay(10);                           // tON 1.8ms + tR 0.7ms (max) に余裕
  float v = readVcart();
  bool flt = !get(PIN_CART_FLT_N);
  if (flt || v < VCART_ON_MIN) {
    put(PIN_CART_PWR_EN, 0);
    cartPower = false;
    Serial.printf("PWR NG vcart=%dmV flt=%d\n", mV(v), flt);
    return false;
  }
  put(PIN_BUS_EN_N, 0);
  cartPower = true;
  put(PIN_STATUS_LED, 1);
  Serial.printf("PWR ON vcart=%dmV\n", mV(v));
  return true;
}

// 動作中の過電流監視。/FLT が出ていたら即遮断して false
bool faultCheck() {
  if (cartPower && !get(PIN_CART_FLT_N)) {
    put(PIN_OE_DATA_N, 1);
    put(PIN_BUS_EN_N, 1);
    put(PIN_CART_PWR_EN, 0);
    cartPower = false;
    put(PIN_STATUS_LED, 0);
    Serial.println("ERR overcurrent (/FLT) - cart power cut");
    return false;
  }
  return true;
}

// ---- データバス折り返し試験-----------------------------
// U5/U6(595) に値を出し、U7/U8(165) で読み戻す。SCK/MOSI/MISO/PL/RCLK_DATA/OE_DATA_N と
// 595/165/U10 の経路、CPU D・PPU D 16 本の配線をまとめて確認できる。
bool needPower();

void loadData(uint8_t cpuD, uint8_t ppuD) {
  shiftOutByte(ppuD);                  // 先に送った方が U6 (PPU D) に入る
  shiftOutByte(cpuD);                  // U5 (CPU D)
  pulse(PIN_RCLK_DATA);
}

void cmdLoopback() {
  // プルアップ(R1-R16)は VCC_CART に吊っているので、カート電源 ON で行う。
  // 制御線は非アクティブ（/ROMSEL=1, /RD=1, R/W=1）なのでカセットが挿さっていてもバスは駆動しない。
  if (!needPower()) return;
  idleBus();
  const uint8_t pat[] = {0x00, 0xFF, 0x55, 0xAA, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
  uint8_t badCpu = 0, badPpu = 0;
  for (uint8_t p : pat) {
    uint8_t q = (uint8_t)~p;           // PPU 側は反転パターン（CPU/PPU 間の短絡も検出）
    loadData(p, q);
    put(PIN_OE_DATA_N, 0);
    delayMicroseconds(2);
    uint16_t r = read165();
    put(PIN_OE_DATA_N, 1);
    uint8_t rc = r >> 8, rp = r & 0xFF;
    badCpu |= rc ^ p; badPpu |= rp ^ q;
    Serial.printf("write CPU=%02X PPU=%02X  read CPU=%02X PPU=%02X %s\n", p, q, rc, rp,
                  (rc == p && rp == q) ? "OK" : "NG");
  }
  setAddr(0, 0);                       // アドレス側のシフトレジスタを整える
  if (!badCpu && !badPpu) Serial.println("LOOPBACK OK");
  else Serial.printf("LOOPBACK NG  bad bits: CPU D=%02X  PPU D=%02X\n", badCpu, badPpu);
}

// ---- 書き込みサイクル（v4.2）---------------------------------------
// CPU 書き込み: $4020-$FFFF。A15=1（$8000 以上）なら /ROMSEL も L にする。
//   1. U5 にデータ、U1/U2 にアドレスをラッチ
//   2. R/W=L → OE_DATA_N=L（データ駆動）→ M2=H、/ROMSEL=L
//   3. アクセス待ち
//   4. /ROMSEL=H（74161/377 等のディスクリートラッチはこの立ち上がりで取り込む）
//      → M2=L（MMC1/MMC3 は M2 の期間で取り込む）
//   5. データを外してから R/W=H
// 注意: UNROM/CNROM などはバス競合がある（ROM も同時にデータを出す）。
//       書き込む値と同じ値が ROM に入っている番地を選ぶのは PC 側の責任。
// v4.3: 書き込みの前にダミーの読み出しサイクル（R/W=H、/ROMSEL=H で M2 を 1 回）を入れる。
//       MMC1 は「直前の M2 サイクルも書き込み」だと書き込みを無視する（RMW 命令対策）。
//       ダンパーは連続する W の間に M2 を出さないので、入れないと 2 回目以降が捨てられる。
//       実機の 6502 も STA の書き込みサイクルの前は必ず読み出しサイクル。
void __not_in_flash_func(cpuWrite)(uint16_t addr, uint8_t d) {
  idleBus();
  loadData(d, 0xFF);
  shiftOutBits(addr & 0x7FFF, 16);
  pulse(PIN_RCLK_ADDR);
  hi(PIN_M2);                 // ダミー読み出し（/ROMSEL=H なので ROM は出力しない）
  waitCyc(accCyc);
  lo(PIN_M2);
  waitCyc(clkCyc);
  lo(PIN_RW);
  waitCyc(clkCyc);
  lo(PIN_OE_DATA_N);
  hi(PIN_M2);
  if (addr & 0x8000) lo(PIN_ROMSEL_N);
  waitCyc(accCyc);
  hi(PIN_ROMSEL_N);
  lo(PIN_M2);
  waitCyc(clkCyc);
  hi(PIN_OE_DATA_N);
  waitCyc(clkCyc);
  hi(PIN_RW);
}

// PPU 書き込み: $0000-$1FFF（CHR-RAM 判定用）。/RD=H のままなので CHR-ROM は出力しない。
void __not_in_flash_func(ppuWrite)(uint16_t addr, uint8_t d) {
  idleBus();
  loadData(0xFF, d);
  setAddr(0x0000, addr & 0x1FFF);      // PA13 = 0, /A13 = 1
  lo(PIN_OE_DATA_N);
  waitCyc(clkCyc);
  lo(PIN_PPU_WR_N);
  waitCyc(accCyc);
  hi(PIN_PPU_WR_N);
  waitCyc(clkCyc);
  hi(PIN_OE_DATA_N);
}

// ---------------------------------------------------------------
void help() {
  Serial.println(F(
    "\nFamicom Dumper rev2 (AE-RP2040) fw v4.4\n"
    "--- power / status ---\n"
    " O 1|0        cart power on (then BUS_EN_N=0) / off (safe sequence)\n"
    " Q            status (VCC_CART, /FLT, power)\n"
    "--- bus (no cartridge OK) ---\n"
    " E 0|1        BUS_EN_N (0=enable, 1=Hi-Z)  * O 1 does this automatically\n"
    " A cccc pppp  set address (CPU 15bit, PPU 14bit) ex: A 7FFF 3FFF\n"
    " C n 0|1      control line n: M2 ROMSEL RW RD WR A13\n"
    " I            all control lines inactive\n"
    " R            raw 165 read (CPU D / PPU D)\n"
    " G            read CIRAM A10 / CIRAM /CE\n"
    " S [clk acc]  timing: shift half-period ns (125) / access wait ns (500)\n"
    " T            data bus loopback test U5/U6 -> U7/U8 (after O 1)\n"
    "--- cartridge (after O 1) ---\n"
    " V            vectors ($FFFA-$FFFF)\n"
    " P aaaa [n]   PRG dump ex: P FFF0 16\n"
    " H aaaa [n]   CHR dump ex: H 0000 16\n"
    " M            mirroring check\n"
    " B P|C aaaa nnnn  binary block read (hex addr/len, max 8000) + CRC\n"
    " W aaaa dd    CPU write ($4020-$FFFF, mapper register)  -> OK\n"
    " X aaaa dd    PPU write ($0000-$1FFF, CHR-RAM test)      -> OK\n"
    " D            full NROM dump (binary, legacy v3 format)"));
}

void dumpLine(uint16_t base, const uint8_t* d, int n) {
  Serial.printf("%04X:", base);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

void cmdVector() {
  const char* name[] = {"NMI  ", "RESET", "IRQ  "};
  for (int i = 0; i < 3; i++) {
    uint16_t a = 0xFFFA + i * 2;
    uint16_t v = readPrg(a) | (uint16_t)readPrg(a + 1) << 8;
    const char* j = (v == 0xFFFF) ? "[NG] open bus (no data from cart)"
                  : (v >= 0x8000) ? "[OK]" : "[NG]";
    Serial.printf("%s $%04X = $%04X %s\n", name[i], a, v, j);
  }
}

char detectMirror(int* base, int* a10, int* a11) {   // 'V' / 'H' / '?'
  // CIRAM A10 はカセット内の配線（または マッパー）経由で返ってくるので、ラッチ後に少し待って読む
  setAddr(0, 0x2000);          delayMicroseconds(1); *base = get(PIN_CIRAM_A10);
  setAddr(0, 0x2000 | 0x0400); delayMicroseconds(1); *a10  = get(PIN_CIRAM_A10);   // PA10=1
  setAddr(0, 0x2000 | 0x0800); delayMicroseconds(1); *a11  = get(PIN_CIRAM_A10);   // PA11=1
  if (*base == 0 && *a10 == 1 && *a11 == 0) return 'V';
  if (*base == 0 && *a10 == 0 && *a11 == 1) return 'H';
  return '?';
}

void cmdMirror() {
  int b, a10, a11;
  char m = detectMirror(&b, &a10, &a11);
  Serial.printf("CIRAM A10: base=%d PA10=%d PA11=%d -> %s\n", b, a10, a11,
                m == 'V' ? "VERTICAL (follows PA10)" :
                m == 'H' ? "HORIZONTAL (follows PA11)" :
                           "UNKNOWN (1-screen / 4-screen / wiring fault)");
}

// ---- 全ダンプ (NROM) ----------------------------------------------
static uint8_t romBuf[0x8000 + 0x2000];          // PRG 32KB + CHR 8KB

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFF) {
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
  }
  return c;
}

// USB CDC へ確実に全バイト送る。
// arduino-pico の Serial.write() は、PC 側の受信が 1 秒以上止まると途中で打ち切り、
// 書けたバイト数だけを返す（SerialUSB.cpp）。戻り値を見て残りを送り直す。
bool writeAll(const uint8_t* p, size_t n) {
  uint32_t t0 = millis();
  while (n) {
    size_t w = Serial.write(p, n);
    if (w) { p += w; n -= w; t0 = millis(); continue; }
    if (!Serial || millis() - t0 > 10000) return false;   // 切断 or 10 秒無応答
    delay(1);
  }
  return true;
}

bool printAll(const char* fmt, ...) {
  char buf[96];
  va_list ap; va_start(ap, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return writeAll((const uint8_t*)buf, len);
}

// プロトコル（rev1 と同一）:
//   "DUMP prg=<n> chr=<n> mir=<c>\n" + PRG + CHR + "\nEND crc=<hex>\n"
void cmdDump() {
  uint8_t* prg = romBuf;
  uint8_t* chr = romBuf + 0x8000;
  for (uint32_t i = 0; i < 0x8000; i++) {
    prg[i] = readPrg(0x8000 + i);
    if ((i & 0x3FF) == 0) {
      if (!faultCheck()) return;
      put(PIN_STATUS_LED, (i >> 10) & 1);              // 読み出し中は点滅
    }
  }
  for (uint32_t i = 0; i < 0x2000; i++) {
    chr[i] = readChr(i);
    if ((i & 0x3FF) == 0 && !faultCheck()) return;
  }
  put(PIN_STATUS_LED, 1);
  int b, a10, a11;
  char mir = detectMirror(&b, &a10, &a11);

  // NROM-128 判定: 前半16KBと後半16KBが一致すれば 16KB 品（$C000側を採用）
  size_t prgSize = memcmp(prg, prg + 0x4000, 0x4000) == 0 ? 0x4000 : 0x8000;
  const uint8_t* prgOut = (prgSize == 0x4000) ? prg + 0x4000 : prg;

  uint32_t c = crc32(prgOut, prgSize);
  c = ~crc32(chr, 0x2000, c);

  printAll("DUMP prg=%u chr=%u mir=%c\n", (unsigned)prgSize, 0x2000u, mir);
  writeAll(prgOut, prgSize);
  writeAll(chr, 0x2000);
  printAll("\nEND crc=%08X\n", (unsigned)c);
  Serial.flush();
}

// ---- ブロック読み出し（v4.1）--------------------------------------
// 要求: "B P aaaa nnnn"（PRG, CPU $6000-$FFFF。$6000-$7FFF は PRG-RAM, v4.4）/ "B C aaaa nnnn"（CHR, PPU $0000-$1FFF）
//       aaaa = 開始アドレス, nnnn = バイト数（いずれも 16 進, nnnn ≤ 8000）
// 応答: "BLK n=<len> crc=<8桁hex> us=<読み出し時間>\n" + <len> バイト
//       エラー時は "ERR ..." の 1 行のみ
void cmdBlock(const String& kind, const String& args) {
  int sp = args.indexOf(' ');
  if (sp < 0 || (kind != "P" && kind != "C")) { Serial.println("ERR usage: B P|C aaaa nnnn"); return; }
  uint32_t a = strtoul(args.substring(0, sp).c_str(), nullptr, 16);
  uint32_t n = strtoul(args.substring(sp + 1).c_str(), nullptr, 16);
  bool prg = (kind == "P");
  uint32_t lo_ = prg ? 0x6000 : 0x0000, hi_ = prg ? 0x10000 : 0x2000;
  if (n == 0 || n > sizeof(romBuf) || a < lo_ || a + n > hi_) {
    Serial.println("ERR range"); return;
  }
  uint32_t t0 = micros();
  for (uint32_t i = 0; i < n; i++) {
    romBuf[i] = prg ? readPrg(a + i) : readChr(a + i);
    if ((i & 0x3FF) == 0x3FF) {
      if (!faultCheck()) return;
      put(PIN_STATUS_LED, (i >> 10) & 1);
    }
  }
  uint32_t us = micros() - t0;
  put(PIN_STATUS_LED, 1);
  uint32_t c = ~crc32(romBuf, n);
  printAll("BLK n=%u crc=%08X us=%u\n", (unsigned)n, (unsigned)c, (unsigned)us);
  writeAll(romBuf, n);
  Serial.flush();
}

void setCtrl(const String& n, int v) {
  int pin = -1;
  if      (n == "M2")     pin = PIN_M2;
  else if (n == "ROMSEL") pin = PIN_ROMSEL_N;
  else if (n == "RW")     pin = PIN_RW;
  else if (n == "RD")     pin = PIN_PPU_RD_N;
  else if (n == "WR")     pin = PIN_PPU_WR_N;
  else if (n == "A13")    pin = PIN_PPU_A13_N;
  if (pin < 0) { Serial.println("bad control line name"); return; }
  put(pin, v);
  Serial.printf("%s = %d\n", n.c_str(), v);
}

bool needPower() {
  if (cartPower) return true;
  Serial.println("ERR cart power off (send O 1 first)");
  return false;
}

void handle(String line) {
  line.trim(); line.toUpperCase();
  if (line.isEmpty()) return;
  char c = line[0];
  String a1, a2;
  int sp1 = line.indexOf(' ');
  if (sp1 > 0) {
    String rest = line.substring(sp1 + 1); rest.trim();
    int sp2 = rest.indexOf(' ');
    a1 = sp2 > 0 ? rest.substring(0, sp2) : rest;
    a2 = sp2 > 0 ? rest.substring(sp2 + 1) : "";
    a2.trim();
  }
  auto hex = [](const String& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 16); };

  switch (c) {
    case '?': help(); break;
    case 'O': if (a1.toInt()) cartOn(); else cartOff(); break;
    case 'Q':
      Serial.printf("VCC_CART=%dmV  /FLT=%d  power=%s  BUS_EN_N=%d\n", mV(readVcart()),
                    get(PIN_CART_FLT_N), cartPower ? "ON" : "OFF", get(PIN_BUS_EN_N));
      break;
    case 'E':
      if (!a1.toInt() && !cartPower) {
        Serial.println("WARN: cart power is OFF. BUS_EN_N=0 drives an unpowered cart.");
      }
      put(PIN_BUS_EN_N, a1.toInt() ? 1 : 0);
      Serial.printf("BUS_EN_N = %d (%s)\n", (int)a1.toInt() ? 1 : 0, a1.toInt() ? "Hi-Z" : "enabled");
      break;
    case 'A':
      setAddr(hex(a1), hex(a2));
      Serial.printf("CPU A=$%04X  PPU A=$%04X\n", hex(a1) & 0x7FFF, hex(a2) & 0x3FFF);
      break;
    case 'C': setCtrl(a1, a2.toInt() ? 1 : 0); break;
    case 'I': idleBus(); Serial.println("control lines idle"); break;
    case 'R': { uint16_t v = read165();
      Serial.printf("CPU D=%02X  PPU D=%02X\n", v >> 8, v & 0xFF); } break;
    case 'G':
      Serial.printf("CIRAM_A10=%d  CIRAM_CE_N=%d\n", get(PIN_CIRAM_A10), get(PIN_CIRAM_CE_N));
      break;
    case 'T': cmdLoopback(); break;
    case 'S':
      if (a1.length()) clkHalfNs = max(20L, a1.toInt());
      if (a2.length()) accessNs  = max(50L, a2.toInt());
      updateTiming();
      Serial.printf("shift clock half-period = %u ns (%u kHz), access wait = %u ns  [clk_sys %u MHz]\n",
                    (unsigned)clkHalfNs, (unsigned)(500000 / clkHalfNs), (unsigned)accessNs, (unsigned)cpuMHz);
      break;
    case 'B': if (needPower()) cmdBlock(a1, a2); break;
    case 'W': {
      if (!needPower()) break;
      uint32_t a = hex(a1), d = hex(a2);
      if (a < 0x4020 || a > 0xFFFF || !a2.length()) { Serial.println("ERR usage: W aaaa dd ($4020-$FFFF)"); break; }
      cpuWrite(a, d);
      if (faultCheck()) Serial.println("OK");
    } break;
    case 'X': {
      if (!needPower()) break;
      uint32_t a = hex(a1), d = hex(a2);
      if (a > 0x1FFF || !a2.length()) { Serial.println("ERR usage: X aaaa dd ($0000-$1FFF)"); break; }
      ppuWrite(a, d);
      if (faultCheck()) Serial.println("OK");
    } break;
    case 'V': if (needPower()) cmdVector(); break;
    case 'P': case 'H': {
      if (!needPower()) break;
      uint16_t base = hex(a1);
      int n = a2.length() ? a2.toInt() : 16;
      uint8_t buf[16];
      for (int i = 0; i < n; i += 16) {
        int m = min(16, n - i);
        for (int j = 0; j < m; j++)
          buf[j] = (c == 'P') ? readPrg(base + i + j) : readChr(base + i + j);
        dumpLine(base + i, buf, m);
      }
    } break;
    case 'M': if (needPower()) cmdMirror(); break;
    case 'D': if (needPower()) cmdDump(); break;
    default: Serial.println("unknown command. ? for help");
  }
}

// ---------------------------------------------------------------
void setup() {
  // 安全側の値を書いてから出力にする（リセット中は基板のプル抵抗で同じ状態）
  outInit(PIN_CART_PWR_EN, 0);         // カート電源 OFF
  outInit(PIN_BUS_EN_N, 1);            // U1-U4, U9 Hi-Z
  outInit(PIN_OE_DATA_N, 1);           // U5-U6 Hi-Z
  outInit(PIN_M2, 0);
  outInit(PIN_ROMSEL_N, 1);
  outInit(PIN_RW, 1);
  outInit(PIN_PPU_RD_N, 1);
  outInit(PIN_PPU_WR_N, 1);
  outInit(PIN_PPU_A13_N, 1);
  outInit(PIN_PL_165_N, 1);
  outInit(PIN_RCLK_ADDR, 0);
  outInit(PIN_RCLK_DATA, 0);
  outInit(PIN_SCK, 0);
  outInit(PIN_MOSI, 0);
  outInit(PIN_STATUS_LED, 0);
  const uint inputs[] = {PIN_MISO, PIN_CIRAM_A10, PIN_CIRAM_CE_N, PIN_CART_FLT_N};
  for (uint p : inputs) {
    gpio_init(p);                      // 入力。プルは基板側にあるので内部プルは無効
    gpio_disable_pulls(p);
  }
  analogReadResolution(12);
  updateTiming();
  setAddr(0, 0);

  Serial.begin(115200);                // while(!Serial) は使わない（PC未接続で止まるため）
}

void loop() {
  static String buf;
  static bool greeted = false;
  if (Serial && !greeted) {            // PC がポートを開いたら一度だけ表示
    greeted = true;
    Serial.println("\nFamicom Dumper rev2 (AE-RP2040) fw v4.4. cart power OFF. '?' for help");
  }
  if (!Serial) greeted = false;

  faultCheck();
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') { handle(buf); buf = ""; }
    else buf += ch;
  }
}
