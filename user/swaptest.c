#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

/*
 * These sizes are meant to be run with reduced PHYSTOP, for example:
 *   make -B PHYSTOP_MB=32 CPUS=1 qemu
 *
 * With the normal 128 MB xv6 setting, the test intentionally fails if no
 * swap I/O happens, because that would not validate page replacement.
 */
#define BASIC_PAGES 4096
#define CLOCK_PAGES 3072
#define CLOCK_EXTRA_PAGES 1024
#define FORK_PAGES 2048
#define DEALLOC_PAGES 4096
#define DEALLOC_ROUNDS 4
#define CHILDREN 4
#define CHILD_PAGES 1024
#define MULTI_PARENT_CHUNK_PAGES 256
#define MULTI_PARENT_MAX_PAGES 12288

static void
fail(const char *test, const char *reason)
{
  printf("swaptest: FAIL at %s: %s\n", test, reason);
  exit(1);
}

static void
stats(int *reads, int *writes)
{
  *reads = -1;
  *writes = -1;
  swapstat(reads, writes);
}

static void
require_swap_activity(const char *test, int r0, int w0, int r1, int w1)
{
  if(w1 <= w0)
    fail(test, "swap write count did not increase; reduce PHYSTOP");
  if(r1 <= r0)
    fail(test, "swap read count did not increase");
}

static uchar
pat(int seed, int page, int off)
{
  return (uchar)((seed * 31 + page * 17 + off * 13) & 0xff);
}

static void
write_page(char *base, int page, int seed)
{
  char *p = base + page * PGSIZE;

  p[0] = pat(seed, page, 0);
  p[37] = pat(seed, page, 37);
  p[PGSIZE / 2] = pat(seed, page, PGSIZE / 2);
  p[PGSIZE - 1] = pat(seed, page, PGSIZE - 1);
}

static int
check_page(char *base, int page, int seed)
{
  char *p = base + page * PGSIZE;

  if(p[0] != pat(seed, page, 0))
    return -1;
  if(p[37] != pat(seed, page, 37))
    return -1;
  if(p[PGSIZE / 2] != pat(seed, page, PGSIZE / 2))
    return -1;
  if(p[PGSIZE - 1] != pat(seed, page, PGSIZE - 1))
    return -1;
  return 0;
}

static void
fill_pages(char *base, int pages, int seed)
{
  int i;

  for(i = 0; i < pages; i++)
    write_page(base, i, seed);
}

static int
verify_pages(char *base, int pages, int seed)
{
  int i;

  for(i = 0; i < pages; i++){
    if(check_page(base, i, seed) < 0)
      return i;
  }
  return -1;
}

static char*
alloc_pages(const char *test, int pages)
{
  char *base;

  base = sbrk(pages * PGSIZE);
  if(base == (char*)-1)
    fail(test, "sbrk failed");
  return base;
}

static void
free_pages(const char *test, int pages)
{
  if(sbrk(-(pages * PGSIZE)) == (char*)-1)
    fail(test, "sbrk shrink failed");
}

static void
test1_basic(void)
{
  const char *test = "test1 basic swap in/out";
  int r0, w0, r1, w1;
  char *base;
  int bad;

  stats(&r0, &w0);
  base = alloc_pages(test, BASIC_PAGES);
  fill_pages(base, BASIC_PAGES, 1);
  bad = verify_pages(base, BASIC_PAGES, 1);
  if(bad >= 0)
    fail(test, "data mismatch");
  stats(&r1, &w1);
  require_swap_activity(test, r0, w0, r1, w1);
  free_pages(test, BASIC_PAGES);
  printf("%s: OK\n", test);
}

static void
test2_clock(void)
{
  const char *test = "test2 clock/access stress";
  int r0, w0, r1, w1;
  int i, round, bad;
  char *base;

  stats(&r0, &w0);
  base = alloc_pages(test, CLOCK_PAGES + CLOCK_EXTRA_PAGES);
  fill_pages(base, CLOCK_PAGES, 2);

  for(round = 0; round < 64; round++){
    for(i = 0; i < 128; i += 3)
      base[i * PGSIZE]++;
  }

  for(i = 0; i < CLOCK_EXTRA_PAGES; i++)
    write_page(base + CLOCK_PAGES * PGSIZE, i, 3);

  for(round = 0; round < 64; round++){
    for(i = 0; i < 128; i += 3)
      base[i * PGSIZE]--;
  }

  bad = verify_pages(base, CLOCK_PAGES, 2);
  if(bad >= 0)
    fail(test, "old page data mismatch");
  bad = verify_pages(base + CLOCK_PAGES * PGSIZE, CLOCK_EXTRA_PAGES, 3);
  if(bad >= 0)
    fail(test, "extra page data mismatch");
  stats(&r1, &w1);
  require_swap_activity(test, r0, w0, r1, w1);
  free_pages(test, CLOCK_PAGES + CLOCK_EXTRA_PAGES);
  printf("%s: OK\n", test);
}

static void
test3_fork(void)
{
  const char *test = "test3 fork swapped page copy";
  int r0, w0, r1, w1;
  int i, pid, st, bad;
  char *base;

  stats(&r0, &w0);
  base = alloc_pages(test, FORK_PAGES);
  fill_pages(base, FORK_PAGES, 4);

  pid = fork();
  if(pid < 0)
    fail(test, "fork failed");

  if(pid == 0){
    bad = verify_pages(base, FORK_PAGES, 4);
    if(bad >= 0)
      exit(1);
    for(i = 0; i < FORK_PAGES; i += 7)
      write_page(base, i, 5);
    exit(0);
  }

  if(wait(&st) < 0)
    fail(test, "wait failed");
  if(st != 0)
    fail(test, "child verification failed");

  bad = verify_pages(base, FORK_PAGES, 4);
  if(bad >= 0)
    fail(test, "parent page changed");
  stats(&r1, &w1);
  require_swap_activity(test, r0, w0, r1, w1);
  free_pages(test, FORK_PAGES);
  printf("%s: OK\n", test);
}

static void
test4_dealloc(void)
{
  const char *test = "test4 dealloc bitmap cleanup";
  int r0, w0, r1, w1;
  int round, bad;
  char *base;

  stats(&r0, &w0);
  for(round = 0; round < DEALLOC_ROUNDS; round++){
    base = alloc_pages(test, DEALLOC_PAGES);
    fill_pages(base, DEALLOC_PAGES, 6 + round);
    bad = verify_pages(base, DEALLOC_PAGES, 6 + round);
    if(bad >= 0)
      fail(test, "data mismatch before dealloc");
    free_pages(test, DEALLOC_PAGES);
  }
  stats(&r1, &w1);
  require_swap_activity(test, r0, w0, r1, w1);
  printf("%s: OK\n", test);
}

static void
child_pressure(int child)
{
  int round, i, bad;
  char *base;

  base = alloc_pages("test5 multi process pressure", CHILD_PAGES);
  fill_pages(base, CHILD_PAGES, 20 + child);
  for(round = 0; round < 8; round++){
    for(i = 0; i < CHILD_PAGES; i++)
      base[i * PGSIZE + (round % 4)] ^= (char)(child + round);
    for(i = 0; i < CHILD_PAGES; i++)
      base[i * PGSIZE + (round % 4)] ^= (char)(child + round);
  }
  sleep(200);
  bad = verify_pages(base, CHILD_PAGES, 20 + child);
  if(bad >= 0)
    exit(1);
  free_pages("test5 multi process pressure", CHILD_PAGES);
  exit(0);
}

static void
test5_multi(void)
{
  const char *test = "test5 multi process pressure";
  int r0, w0, r1, w1, rc, wc;
  int i, pid, st, bad, parent_pages;
  char *base;
  char *chunk;

  stats(&r0, &w0);
  for(i = 0; i < CHILDREN; i++){
    pid = fork();
    if(pid < 0)
      fail(test, "fork failed");
    if(pid == 0)
      child_pressure(i);
  }

  sleep(20);
  base = 0;
  parent_pages = 0;
  for(;;){
    stats(&rc, &wc);
    if(wc > w0)
      break;
    if(parent_pages >= MULTI_PARENT_MAX_PAGES)
      fail(test, "swap write count did not increase; reduce PHYSTOP");

    chunk = sbrk(MULTI_PARENT_CHUNK_PAGES * PGSIZE);
    if(chunk == (char*)-1)
      fail(test, "sbrk failed before swap pressure");
    if(base == 0)
      base = chunk;
    if(chunk != base + parent_pages * PGSIZE)
      fail(test, "sbrk returned non-contiguous memory");

    for(i = 0; i < MULTI_PARENT_CHUNK_PAGES; i++)
      write_page(base, parent_pages + i, 40);
    parent_pages += MULTI_PARENT_CHUNK_PAGES;
  }

  bad = verify_pages(base, parent_pages, 40);
  if(bad >= 0)
    fail(test, "parent pressure data mismatch");

  for(i = 0; i < CHILDREN; i++){
    if(wait(&st) < 0)
      fail(test, "wait failed");
    if(st != 0)
      fail(test, "child failed");
  }
  bad = verify_pages(base, parent_pages, 40);
  if(bad >= 0)
    fail(test, "parent pressure changed");
  free_pages(test, parent_pages);
  stats(&r1, &w1);
  require_swap_activity(test, r0, w0, r1, w1);
  printf("%s: OK\n", test);
}

static void
test6_exec(void)
{
  const char *test = "test6 exec ls";
  char *argv[] = { "ls", 0 };
  int pid, st;

  pid = fork();
  if(pid < 0)
    fail(test, "fork failed");
  if(pid == 0){
    exec("ls", argv);
    exit(1);
  }
  if(wait(&st) < 0)
    fail(test, "wait failed");
  if(st != 0)
    fail(test, "ls failed");
  printf("%s: OK\n", test);
}

int
main(void)
{
  printf("swaptest: start\n");
  test1_basic();
  test2_clock();
  test3_fork();
  test4_dealloc();
  test5_multi();
  test6_exec();
  printf("swaptest: all tests passed\n");
  exit(0);
}
