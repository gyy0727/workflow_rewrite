#include <aio.h> /* For struct iocb */
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h> /* For ssize_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h> /* For syscall() */
#include <sys/uio.h>
#include <unistd.h>

#define BUFSIZE 4096
#define FILENAME "/path/to/your/file"

int main(void) {
  struct iocb cmd;
  char *buf = (char *)malloc(BUFSIZE);
  ssize_t r;

  if (!buf) {
    return -ENOMEM;
  }

  memset(&cmd, 0, sizeof(cmd));

  int ctx;
  ctx = syscall(__NR_io_setup, 1, NULL);
  if (ctx == -1) {
    perror("io_setup");
    free(buf);
    return -1;
  }

  int fd = open(FILENAME, O_RDONLY);
  if (fd == -1) {
    perror("open");
    goto cleanup;
  }

  aio_fill_read(&cmd, fd, buf, BUFSIZE, 0 /* offset */);

  int ret = syscall(__NR_io_submit, ctx, 1, &cmd);
  if (ret != 1) {
    perror("io_submit");
    goto cleanup;
  }

  /* Wait for completion using another method like polling or signals */

cleanup:
  close(fd);
  syscall(__NR_io_destroy, ctx);
  free(buf);

  return 0;
}