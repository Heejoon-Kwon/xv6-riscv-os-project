#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "user.h"
#include "../kernel/fcntl.h"
#include "../kernel/memlayout.h"
#include "../kernel/param.h"
#include "../kernel/spinlock.h"
#include "../kernel/sleeplock.h"
#include "../kernel/fs.h"
#include "../kernel/syscall.h"

#define PGSIZE 4096

#define README_BYTES 64

static int failures;

static void
fail(char *msg)
{
  printf("FAIL: %s\n", msg);
  failures++;
}

static void
check(int cond, char *msg)
{
  if(cond)
    printf("  ok: %s\n", msg);
  else
    fail(msg);
}

static int
pages(char *where)
{
  int n = freemem();
  printf("  freemem %s: %d pages\n", where, n);
  return n;
}

static int
read_readme_prefix(char *buf, int max)
{
  int fd;
  int n;

  fd = open("README", O_RDONLY);
  if(fd < 0){
    fail("open README");
    return -1;
  }

  n = read(fd, buf, max);
  close(fd);

  if(n <= 0){
    fail("read README prefix");
    return -1;
  }
  return n;
}

static void
cleanup_heap(char *oldbrk)
{
  char *now = sbrk(0);

  if(now > oldbrk){
    if(sbrk(-(now - oldbrk)) == SBRK_ERROR)
      fail("restore heap break");
  }
}

static void
lazy_sbrk_phase(void)
{
  char *oldbrk;
  char *p;
  int before;
  int after_grow;
  int after_touch;
  int fds[2];
  char ch = 'Z';

  printf("\n================ lazy sbrk phase ================\n");
  printf("Goal: page faults allocate lazy heap pages.\n");

  oldbrk = sbrk(0);
  before = pages("before sbrklazy");
  p = sbrklazy(4 * PGSIZE);
  after_grow = pages("after sbrklazy");

  if(p == SBRK_ERROR){
    fail("sbrklazy(4 pages) succeeds");
    return;
  }

  check(p == oldbrk, "sbrklazy returns old break");
  check(after_grow == before, "lazy grow does not consume physical pages immediately");

  p[0] = 1;
  p[2 * PGSIZE] = 2;
  after_touch = pages("after lazy touches");

  check(p[0] == 1, "first lazy page fault is handled");
  check(p[2 * PGSIZE] == 2, "third lazy page fault is handled");
  check(after_touch < after_grow, "touching lazy pages consumes physical memory");

  if(pipe(fds) < 0){
    fail("pipe for lazy copy test");
  } else {
    if(write(fds[1], &ch, 1) != 1)
      fail("pipe write before copyout test");
    else if(read(fds[0], p + PGSIZE, 1) != 1)
      fail("kernel copyout into lazy page");
    else
      check(p[PGSIZE] == 'Z', "read() copyout handles lazy page fault");

    p[PGSIZE + 1] = 'Q';
    if(write(fds[1], p + PGSIZE + 1, 1) != 1)
      fail("write() copyin from lazy page");
    else {
      ch = 0;
      if(read(fds[0], &ch, 1) != 1)
        fail("pipe read after copyin test");
      else
        check(ch == 'Q', "write() copyin reads lazy page data");
    }

    close(fds[0]);
    close(fds[1]);
  }

  cleanup_heap(oldbrk);
}

static void
alignment_phase(void)
{
  int before;
  uint64 addr;

  printf("\n================ alignment phase ================\n");
  printf("Goal: unaligned mmap/munmap address arguments return 0.\n");

  before = pages("before invalid calls");
  addr = mmap(1, PGSIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
  check(addr == 0, "mmap rejects unaligned address with 0");
  check(munmap(1) == 0, "munmap rejects unaligned address with 0");
  check(munmap(MMAPBASE + 120 * PGSIZE) == -1,
        "munmap returns -1 when no mapping starts at address");
  check(freemem() == before, "invalid alignment calls do not consume memory");
}

static void
anon_mmap_case(char *name, int flags, uint64 mapoff)
{
  uint64 addr;
  char *p;
  int before;
  int after_mmap;
  int after_touch;
  int after_munmap;

  printf("\n================ anonymous mmap: %s ================\n", name);

  before = pages("before mmap");
  addr = mmap(mapoff, 2 * PGSIZE, PROT_READ | PROT_WRITE,
              MAP_ANONYMOUS | flags, -1, 0);
  after_mmap = pages("after mmap");

  if(addr == 0){
    fail("anonymous mmap succeeds");
    return;
  }

  p = (char *)addr;
  printf("  mmap returned %p\n", p);

  if(flags & MAP_POPULATE)
    check(after_mmap < before, "MAP_POPULATE allocates anonymous pages during mmap");
  else
    check(after_mmap == before, "anonymous mmap without populate is lazy");

  check(p[0] == 0 && p[PGSIZE] == 0, "anonymous pages are zero-filled");
  p[0] = 'A';
  p[PGSIZE] = 'B';
  after_touch = pages("after first access");

  check(p[0] == 'A' && p[PGSIZE] == 'B', "anonymous pages are writable");
  if(flags & MAP_POPULATE)
    check(after_touch == after_mmap, "populated anonymous mapping needs no first-touch allocation");
  else
    check(after_touch < after_mmap, "anonymous first touch page fault is handled");

  check(munmap(addr) == 1, "munmap anonymous mapping");
  after_munmap = pages("after munmap");
  check(after_munmap == before, "anonymous munmap restores free pages");
}

static void
anon_mmap_fork_share_phase(void)
{
  uint64 addr;
  char *p;
  int before_fork;
  int after_wait;
  int pid;
  int st = -1;

  printf("\n================ anonymous mmap fork share phase ================\n");
  printf("Goal: fork maps the same mmap physical page as the parent.\n");

  addr = mmap(14 * PGSIZE, PGSIZE, PROT_READ | PROT_WRITE,
              MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if(addr == 0){
    fail("anonymous MAP_POPULATE mmap before fork succeeds");
    return;
  }

  p = (char *)addr;
  p[0] = 'P';
  before_fork = pages("before fork");

  pid = fork();
  if(pid < 0){
    fail("fork for shared anonymous mmap");
    munmap(addr);
    return;
  }

  if(pid == 0){
    if(p[0] != 'P')
      exit(1);
    p[0] = 'C';
    exit(0);
  }

  wait(&st);
  after_wait = pages("after child wait");

  check(st == 0, "child sees parent mmap contents");
  check(p[0] == 'C', "child write is visible through shared mmap page");
  check(after_wait == before_fork, "freemem restored after shared mmap fork child exits");
  check(munmap(addr) == 1, "munmap shared anonymous mapping");
}

static void
file_mmap_case(char *name, int flags, uint64 mapoff)
{
  char expected[README_BYTES];
  int expected_n;
  int fd;
  uint64 addr;
  char *p;
  int before;
  int after_mmap;
  int after_touch;
  int after_munmap;

  printf("\n================ README file mmap: %s ================\n", name);

  expected_n = read_readme_prefix(expected, sizeof(expected));
  if(expected_n < 0)
    return;

  fd = open("README", O_RDONLY);
  if(fd < 0){
    fail("open README for mmap");
    return;
  }

  before = pages("before mmap");
  addr = mmap(mapoff, PGSIZE, PROT_READ, flags, fd, 0);
  after_mmap = pages("after mmap");

  if(addr == 0){
    fail("README file mmap succeeds");
    close(fd);
    return;
  }

  p = (char *)addr;
  printf("  mmap returned %p\n", p);

  if(flags & MAP_POPULATE)
    check(after_mmap < before, "MAP_POPULATE allocates file page during mmap");
  else
    check(after_mmap == before, "file mmap without populate is lazy");

  check(memcmp(p, expected, expected_n) == 0, "README contents visible through mmap");
  after_touch = pages("after README read");

  if(flags & MAP_POPULATE)
    check(after_touch == after_mmap, "populated file mapping needs no first-touch allocation");
  else
    check(after_touch < after_mmap, "file mapping first read page fault is handled");

  check(munmap(addr) == 1, "munmap README file mapping");
  after_munmap = pages("after munmap");
  check(after_munmap == before, "file munmap restores free pages");
  close(fd);
}

static void
file_mmap_fork_phase(void)
{
  char expected[README_BYTES];
  int expected_n;
  int fd;
  uint64 addr;
  char *p;
  int ready[2];
  int release[2];
  int before_fork;
  int during_fork;
  int after_wait;
  int pid;
  int st = -1;
  char c = 0;

  printf("\n================ README mmap fork phase ================\n");
  printf("Goal: mapped README contents are the same before and after fork.\n");

  expected_n = read_readme_prefix(expected, sizeof(expected));
  if(expected_n < 0)
    return;

  fd = open("README", O_RDONLY);
  if(fd < 0){
    fail("open README for fork mmap");
    return;
  }

  addr = mmap(12 * PGSIZE, PGSIZE, PROT_READ, 0, fd, 0);
  if(addr == 0){
    fail("README mmap before fork succeeds");
    close(fd);
    return;
  }

  p = (char *)addr;
  check(memcmp(p, expected, expected_n) == 0, "parent sees README contents before fork");

  if(pipe(ready) < 0 || pipe(release) < 0){
    fail("pipe setup for mmap fork test");
    munmap(addr);
    close(fd);
    return;
  }

  before_fork = pages("before fork");
  pid = fork();
  if(pid < 0){
    fail("fork for README mmap");
    close(ready[0]);
    close(ready[1]);
    close(release[0]);
    close(release[1]);
    munmap(addr);
    close(fd);
    return;
  }

  if(pid == 0){
    int ok;

    close(ready[0]);
    close(release[1]);

    ok = (memcmp(p, expected, expected_n) == 0);
    c = ok ? '1' : '0';
    write(ready[1], &c, 1);
    read(release[0], &c, 1);

    close(ready[1]);
    close(release[0]);
    exit(ok ? 0 : 1);
  }

  close(ready[1]);
  close(release[0]);

  if(read(ready[0], &c, 1) != 1)
    fail("read child mmap comparison result");
  else
    check(c == '1', "child sees same README contents after fork");

  during_fork = pages("while child alive");
  check(during_fork < before_fork, "freemem decreases after fork while child is alive");

  c = 'x';
  write(release[1], &c, 1);
  wait(&st);

  after_wait = pages("after child wait");
  check(st == 0, "child exits successfully after mmap comparison");
  check(after_wait == before_fork, "freemem restored after forked child exits");

  close(ready[0]);
  close(release[1]);
  check(munmap(addr) == 1, "munmap fork README mapping");
  close(fd);
}

int
main(void)
{
  printf("=== vm_test start ===\n");
  printf("This tests lazy page faults plus anonymous/file mmap using README.\n");

  lazy_sbrk_phase();
  alignment_phase();
  anon_mmap_case("without populate", 0, 0);
  anon_mmap_case("with populate", MAP_POPULATE, 4 * PGSIZE);
  anon_mmap_fork_share_phase();
  file_mmap_case("without populate", 0, 8 * PGSIZE);
  file_mmap_case("with populate", MAP_POPULATE, 10 * PGSIZE);
  file_mmap_fork_phase();

  if(failures == 0){
    printf("\n=== vm_test PASS ===\n");
    exit(0);
  }

  printf("\n=== vm_test FAIL: %d failure(s) ===\n", failures);
  exit(1);
}
