#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

/** DEFINES ***/

#define JJ_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT  = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,

  DEL_KEY,

  HOME_KEY,
  END_KEY,

  PAGE_UP,
  PAGE_DOWN,
};

/*** GLOBALS ***/

struct editorConfig {
  /** 
   * Coursor position.
   */
  int cx;
  int cy;
  /**
   * Terminal window size
   */
  int screenrows;
  int screencols;
  /**
   * Original reference to the termios struct
   */
  struct termios orig_termios;
};

struct editorConfig E;

/*** TERMINAL ***/

/**
 * Fail with an error message.
 * @param const char* meessage display before quitting
 */
void die(const char *message) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(message);
  exit(1);
}

/**
 * Restore original reference of termios struct.
 */
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH,  &E.orig_termios) == -1)
    die("disableRawMode: tcsetattr"); 
}

/**
 * Enables RawMode on the terminal.
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
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("enableRawMode: tcgetattr");

  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // create timeout for read() to make it non-render-blocking
  raw.c_cc[VMIN] = 0;   // 0 bytes needed for input
  raw.c_cc[VTIME] = 1; // 10ms

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("enableRawMode: tcsetattr");
}

/**
 * Fetch latest keypress. The keypress is fetched with a timeout
 * to make it non-render-blocking, and if it appears as an escape
 * sequece is decoded to returned as an editor key
 * @return int key
 */
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1  && errno != EAGAIN) die("editorReadKey: read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == '0') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

/**
 * Fetch cursor position in the terminal window.
 * @param int *rows fetched row
 * @param int *cols fetched col
 * @return int status 
 */
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0; 

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0'; 

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

/**
 * Fetch window size of the terminal window.
 * @param int *rows fetched row
 * @param int *cols fetched col
 * @return int status 
 */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    editorReadKey();
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** APPEND BUFFER ***/

/**
 * To better reduce writing instructions to STDOUT
 * we can use a dynamically allocated string that
 * words can be appended to, reducing the need of 
 * writing operations
 */
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

/**
 * Append to abuf (Append Buffer).
 * @param struct abuf *ab append buffer
 * @param const char  *s  text to append
 * @param int         len text size
 */
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/**
 * Deallocate abuf (Append Buffer).
 * @param struct abuf *ab append buffer
 */
void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** OUTPUT ***/

/**
 * Render editor rows writing text to append buffer.
 * @param struct abuf *ab append buffer
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
          "JJ editor -- version %s", JJ_VERSION);
      if (welcomelen > E.screencols) welcomelen = E.screencols;
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } else {
      abAppend(ab, "~", 1);
    }
    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/**
 * Reset cursor position and render editor.
 */
void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  
  editorDrawRows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** INPUT ***/

/** 
 * Handle editor movement keys.
 * @param int key editor key
 */
void editorMoveCursor(int key) {
  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      }
      break;
    case ARROW_RIGHT:
      if (E.cx != E.screencols - 1) {
        E.cx++;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy != E.screenrows - 1) {
        E.cy++;
      }
      break;
  }
}

/**
 * Handle editor keys execution and behaviour.
 */
void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    
    case END_KEY:
      E.cx = E.screencols - 1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/*** INITIALIZE ***/

/**
 * Initialize E (Editor Config).
 */
void initEditor() {
  E.cx = 0;
  E.cy = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("initEditor: getWindowSize"); 
}

/**
 * Entrypoint
 * @param int   argc   argument count
 * @param char* argv[] argument values
 * @return int status
 */
int main(int argc, char* argv[]) {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  } 
  return 0;
}
