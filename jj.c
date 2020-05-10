#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

/*** GLOBALS ***/

/**
 * Original reference to the termios struct
 */
struct termios orig_termios;

/*** TERMINAL ***/

/**
 * Fail with an error message, as it should always be
 */
void die(const char* message) {
  perror(message);
  exit(1);
}

/**
 * Restore original reference of termios struct
 */
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH,  &orig_termios) == -1)
    die("disableRawMode(): tcsetattr"); 
}

/**
 * Enables RawMode on the terminal
 * effects include:
 * * (ECHO)   disable echoing 
 * * (ICANON) disable return key 
 * * (ISIG)   disable signal of ctrl-c & ctrl-z
 * * (IEXTEN) disable signal of ctrl-v
 * * (IXON)   disable signal of ctrl-s & ctrl-q
 * * (ICRNL)  disable Carriage Return New Line (... ctrl-m)
 * * (OPOST)  disable output processing
 * * (.....)  legacy for RawMode, probably already disabled 
 */
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("enableRawMode(): tcgetattr");

  atexit(disableRawMode);
  struct termios raw = orig_termios;
  raw.c_iflag &= ~(ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // create timeout for read() to make it non-render-blocking
  raw.c_cc[VMIN] = 0;   // 0 bytes needed for input
  raw.c_cc[VTIME] = 1; // 10ms

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("enableRawMode(): tcsetattr");
}

/*** INITIALIZE ***/

int main(void) {
  enableRawMode();

  while (1) {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("main(): read");
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q') break;
  } 
  return 0;
}
