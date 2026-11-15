// Micro-benchmark: cost of first touch of POSIX shared memory and
// whether madvise/posix_madvise variants remove it.
//
// Sender side: fresh shm segment (no page allocated yet), write `size` bytes.
// Receiver side: segment already populated by sender; a fresh mapping in
// another process reads `size` bytes (page-table population only).
//
// For each strategy we report:
//   prep_us  : time spent in the prefault call itself (setup phase)
//   touch_us : time of the first full write/read of the payload (hot path)
//   minflt   : minor page faults during the touch phase
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

enum strategy { NONE, POSIX_WILLNEED, POPULATE, MAP_POP, MLOCK, TOUCH, NSTRAT };
static const char *names[] = {"none", "posix_madvise(WILLNEED)",
                              "madvise(POPULATE_*)", "mmap(MAP_POPULATE)",
                              "mlock", "manual touch (1B/page)"};

static double now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static long minflt(void) {
  struct rusage r;
  getrusage(RUSAGE_SELF, &r);
  return r.ru_minflt;
}

// returns mapping; prep time written to *prep
static uint8_t *map_with(int fd, size_t size, enum strategy s, int writing,
                         double *prep, int *err) {
  int flags = MAP_SHARED | (s == MAP_POP ? MAP_POPULATE : 0);
  double t0 = now_us();
  uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, fd, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  *err = 0;
  switch (s) {
  case POSIX_WILLNEED:
    *err = posix_madvise(p, size, POSIX_MADV_WILLNEED);
    break;
  case POPULATE:
    *err = madvise(p, size, writing ? MADV_POPULATE_WRITE : MADV_POPULATE_READ);
    break;
  case MLOCK:
    *err = mlock(p, size);
    break;
  case TOUCH: {
    long ps = sysconf(_SC_PAGESIZE);
    if (writing)
      for (size_t i = 0; i < size; i += ps) ((volatile uint8_t *)p)[i] = 0;
    else {
      volatile uint8_t sink = 0;
      for (size_t i = 0; i < size; i += ps) sink += ((volatile uint8_t *)p)[i];
      (void)sink;
    }
  } break;
  default:
    break;
  }
  *prep = now_us() - t0;
  return p;
}

static int cmpd(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  size_t sizes_mb[] = {1, 4, 16, 64};
  int reps = argc > 1 ? atoi(argv[1]) : 15;
  char name[64];
  snprintf(name, sizeof name, "/prefault_bench_%d", getpid());

  printf("side,size_mb,strategy,prep_us,touch_us,touch_minflt,warm_us,err\n");
  for (size_t si = 0; si < sizeof sizes_mb / sizeof *sizes_mb; si++) {
    size_t size = sizes_mb[si] << 20;
    uint8_t *src = malloc(size);
    memset(src, 0xab, size);
    for (int s = 0; s < NSTRAT; s++) {
      for (int side = 0; side < 2; side++) { // 0 = sender, 1 = receiver
        double prep[64], touch[64], warm[64];
        long flt[64];
        int err = 0;
        for (int r = 0; r < reps; r++) {
          shm_unlink(name);
          int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
          if (fd < 0 || ftruncate(fd, size)) { perror("shm"); return 1; }
          if (side == 1) { // pre-populate as the sender would have
            uint8_t *w = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            memcpy(w, src, size);
            munmap(w, size);
          }
          // run the measured part in a child so page tables start empty
          int pipefd[2];
          pipe(pipefd);
          pid_t pid = fork();
          if (pid == 0) {
            double pr; int e;
            uint8_t *p = map_with(fd, size, s, side == 0, &pr, &e);
            long f0 = minflt();
            double t0 = now_us();
            volatile uint64_t sum = 0;
            if (side == 0) memcpy(p, src, size);
            else for (size_t i = 0; i < size; i += 64) sum += p[i];
            double t1 = now_us();
            long f1 = minflt();
            double t2 = now_us();
            if (side == 0) memcpy(p, src, size);
            else for (size_t i = 0; i < size; i += 64) sum += p[i];
            double t3 = now_us();
            double out[4] = {pr, t1 - t0, (double)(f1 - f0), t3 - t2};
            int ee = e;
            write(pipefd[1], out, sizeof out);
            write(pipefd[1], &ee, sizeof ee);
            _exit(0);
          }
          double in[4];
          read(pipefd[0], in, sizeof in);
          read(pipefd[0], &err, sizeof err);
          waitpid(pid, NULL, 0);
          close(pipefd[0]); close(pipefd[1]); close(fd);
          prep[r] = in[0]; touch[r] = in[1]; flt[r] = (long)in[2]; warm[r] = in[3];
        }
        shm_unlink(name);
        qsort(prep, reps, sizeof(double), cmpd);
        qsort(touch, reps, sizeof(double), cmpd);
        qsort(warm, reps, sizeof(double), cmpd);
        printf("%s,%zu,%s,%.0f,%.0f,%ld,%.0f,%d\n", side ? "receiver" : "sender",
               sizes_mb[si], names[s], prep[reps / 2], touch[reps / 2], flt[reps / 2],
               warm[reps / 2], err);
        fflush(stdout);
      }
    }
    free(src);
  }
  return 0;
}
