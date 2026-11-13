/* Driver for tests/difftest: loads an initial CPU state and a memory image, runs the
 * AOT-generated blocks instrumented with SNAP(), and prints one trace line per guest
 * instruction. tests/difftest/oracle.py prints the same format from Unicorn Engine. */
#include "difftest.h"

uint8_t *dt_pages[1u << 20];
static unsigned long dt_npages, dt_budget = 100000;   /* 400 MB: a runaway trial stops instead of OOM-killing the run */
uint32_t dt_last_pc;

uint8_t *dt_page(uint32_t a) {
  unsigned k = a >> 12;
  if (!dt_pages[k]) {
    if (dt_npages >= dt_budget) { printf("B %08x\n", dt_last_pc); fflush(stdout); exit(0); }
    dt_pages[k] = calloc(1, 4096);
    if (!dt_pages[k]) { fprintf(stderr, "out of memory for page %#x\n", a & ~0xfffu); exit(2); }
    dt_npages++;
  }
  return dt_pages[k];
}

void dt_ld(void *dst, uint32_t a, unsigned n) {
  uint8_t *d = dst;
  while (n) {
    unsigned off = a & 0xfff, take = 4096 - off; if (take > n) take = n;
    memcpy(d, dt_page(a) + off, take);
    a += take; d += take; n -= take;
  }
}

void dt_st(uint32_t a, const void *src, unsigned n) {
  const uint8_t *s = src;
  while (n) {
    unsigned off = a & 0xfff, take = 4096 - off; if (take > n) take = n;
    memcpy(dt_page(a) + off, s, take);
    a += take; s += take; n -= take;
  }
}

/* ---- trace output ---- */
static unsigned long dt_steps;

void dt_snap(CPU *c, uint32_t pc, const uint64_t *r, uint8_t cf, uint8_t zf, uint8_t sf, uint8_t of, uint8_t pf) {
  (void)c;
  dt_last_pc = pc;
  printf("S %08x %08x %08x %08x %08x %08x %08x %08x %08x %u %u %u %u %u %u %u %u\n",
         pc,
         (uint32_t)r[0], (uint32_t)r[1], (uint32_t)r[2], (uint32_t)r[3],
         (uint32_t)r[4], (uint32_t)r[5], (uint32_t)r[6], (uint32_t)r[7],
         cf, zf, sf, of, pf, c->df, c->ftop, c->fsw);
  dt_steps++;
  if (dt_steps > 100000) { printf("E %08x\n", pc); fflush(stdout); exit(0); }
}

void dt_unimpl(uint32_t pc, const char *what) {
  printf("U %08x %s\n", pc, what ? what : "?");
}

/* ---- input files ---- */
static int load_init(const char *path, CPU *c) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return 0; }
  char key[32]; unsigned long long v;
  while (fscanf(f, "%31s %llx", key, &v) == 2) {
    static const char *nm[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    int hit = 0;
    for (int i = 0; i < 8; i++) if (!strcmp(key, nm[i])) { c->r[i] = (uint32_t)v; hit = 1; }
    if (hit) continue;
    if (!strcmp(key, "cf")) c->cf = (uint8_t)(v & 1);
    else if (!strcmp(key, "zf")) c->zf = (uint8_t)(v & 1);
    else if (!strcmp(key, "sf")) c->sf = (uint8_t)(v & 1);
    else if (!strcmp(key, "of")) c->of = (uint8_t)(v & 1);
    else if (!strcmp(key, "pf")) c->pf = (uint8_t)(v & 1);
    else if (!strcmp(key, "df")) c->df = (uint8_t)(v & 1);
    else if (!strcmp(key, "gs")) c->gs = (uint32_t)v;
    else if (!strcmp(key, "fs")) c->sregs[4] = (uint16_t)v;
    else if (!strcmp(key, "es")) c->sregs[0] = (uint16_t)v;
    else if (!strcmp(key, "cs")) c->sregs[1] = (uint16_t)v;
    else if (!strcmp(key, "ss")) c->sregs[2] = (uint16_t)v;
    else if (!strcmp(key, "ds")) c->sregs[3] = (uint16_t)v;
    else if (!strcmp(key, "fcw")) c->fcw = (uint16_t)v;
    else if (!strcmp(key, "fsw")) c->fsw = (uint16_t)v;
    else if (!strcmp(key, "ftop")) c->ftop = (uint8_t)v;
    else if (!strcmp(key, "pc")) c->pc = (uint32_t)v;
    else if (key[0] == 's' && key[1] == 't' && key[2] >= '0' && key[2] <= '7') memcpy(c->fst[key[2] - '0'].b, &v, 8);
  }
  fclose(f);
  return 1;
}

static int load_mem(const char *map_path, const char *bin_path) {
  FILE *m = fopen(map_path, "r");
  FILE *b = fopen(bin_path, "rb");
  if (!m || !b) { perror(map_path); return 0; }
  unsigned long long base; unsigned long size;
  while (fscanf(m, "%llx %lu", &base, &size) == 2) {
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    if (fread(buf, 1, size, b) != size) { fprintf(stderr, "short read in %s\n", bin_path); return 0; }
    for (unsigned long o = 0; o < size; o += 4096) {
      unsigned long take = size - o < 4096 ? size - o : 4096;
      memcpy(dt_page((uint32_t)(base + o)), buf + o, take);
    }
    free(buf);
  }
  fclose(m); fclose(b);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: %s init.txt mem.map mem.bin\n", argv[0]); return 2; }
  CPU c; memset(&c, 0, sizeof c);
  if (!load_init(argv[1], &c)) return 2;
  if (!load_mem(argv[2], argv[3])) return 2;
  uint32_t end = difftest_run(&c);
  printf("E %08x\n", end);
  fflush(stdout);
  return 0;
}
