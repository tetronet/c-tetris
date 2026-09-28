/*
 * tetris.c - classic 10x20 Tetris for the terminal, pure C.
 *
 * Controls:
 *   Space .... start the game (from the menu)
 *   Enter .... force the block straight down (hard drop, locks it)
 *   Up ....... rotate block clockwise
 *   Down ..... rotate block counter-clockwise
 *   Left ..... move block one column left
 *   Right .... move block one column right
 *   q ....... quit to menu
 *
 * Score = number of destroyed lines.
 * If the whole board is ever empty after a lock: "You win!" and exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>

#define COLS 10
#define ROWS 20
#define CELL 4          /* pieces are stored in 4x4 matrices  */
#define NP   7          /* number of piece shapes            */
#define FRAME_MS 66     /* render/loop interval              */
#define TICK_MS  70     /* gravity: fall one row every 70ms  */

/* pieces in order: L, J, T, S, Z, I, O  (1 = filled cell) */
static const char P[NP][CELL][CELL] = {
    {"...1","111."},  /* L */
    {"1...","111."},  /* J */
    {".1..","111."},  /* T */
    {".11.","11.."},  /* S */
    {"11..",".11."},  /* Z */
    {"....","1111"},  /* I */
    {".11.",".11."}   /* O */
};
static const char PCHAR[NP] = "LTJSZIO"; /* ASCII char printed for each piece */

static char board[ROWS][COLS];
static char curMat[CELL][CELL];  /* matrix of the currently falling piece */
static int  curX = 3, curY = 0;  /* top-left anchor of the 4x4 matrix     */
static int  curIdx = 0;
static int  score = 0;

static struct termios origTerm;
static int termRestored = 0;

/* ------------------------------------------------------------------ */

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void restoreTerm(void)
{
    if (!termRestored) {
        tcsetattr(0, TCSANOW, &origTerm);
        termRestored = 1;
    }
}

/* is (y,x) blocked? out of bounds (except above the top) counts as blocked */
static int occupied(int y, int x)
{
    if (x < 0 || x >= COLS || y >= ROWS) return 1;
    if (y < 0) return 0;
    return board[y][x] != 0;
}

/* does a 4x4 matrix fit at (x,y)? */
static int fits(const char mat[CELL][CELL], int x, int y)
{
    for (int i = 0; i < CELL; i++)
        for (int j = 0; j < CELL; j++)
            if (mat[i][j] && occupied(y + i, x + j))
                return 0;
    return 1;
}

static void lockPiece(void)
{
    for (int i = 0; i < CELL; i++)
        for (int j = 0; j < CELL; j++)
            if (curMat[i][j] && curY + i >= 0 && curY + i < ROWS)
                board[curY + i][curX + j] = PCHAR[curIdx];
}

/* remove full lines, +1 score per line */
static void clearLines(void)
{
    for (int r = 0; r < ROWS; r++) {
        int full = 1;
        for (int c = 0; c < COLS; c++)
            if (!board[r][c]) full = 0;
        if (full) {
            for (int y = r; y > 0; y--)
                for (int c = 0; c < COLS; c++)
                    board[y][c] = board[y - 1][c];
            for (int c = 0; c < COLS; c++) board[0][c] = 0;
            score++;
        }
    }
}

static int boardEmpty(void)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (board[r][c]) return 0;
    return 1;
}

static void spawn(void)
{
    curIdx = rand() % NP;
    memcpy(curMat, P[curIdx], sizeof curMat);
    curX = (COLS - CELL) / 2;   /* = 3 */
    curY = 0;
}

/* piece landed: lock, clear, check win, spawn next, check game over */
static void advance(void)
{
    lockPiece();
    clearLines();

    if (boardEmpty()) {               /* the whole board is empty: victory */
        restoreTerm();
        puts("\nYou win!\n");
        exit(0);
    }

    spawn();
    if (!fits(curMat, curX, curY)) {  /* new piece collides: game over */
        restoreTerm();
        printf("\nGame over\nScore: %d\n", score);
        exit(1);
    }
}

static void hardDrop(void)
{
    while (fits(curMat, curX, curY + 1)) curY++;
    advance();
}

static void gravityTick(void)
{
    if (fits(curMat, curX, curY + 1))
        curY++;
    else
        advance();
}

/* rotate: 0 = clockwise, 1 = counter-clockwise; with simple wall kicks */
static void rotate(int ccw)
{
    if (curIdx == 6) return;          /* O piece looks the same rotated */

    char m2[CELL][CELL];
    for (int i = 0; i < CELL; i++)
        for (int j = 0; j < CELL; j++)
            m2[i][j] = ccw ? curMat[CELL - 1 - j][i]
                           : curMat[j][CELL - 1 - i];

    static const int kicks[5] = { 0, -1, 1, -2, 2 };
    for (int k = 0; k < 5; k++) {
        int dx = kicks[k];
        if (fits(m2, curX + dx, curY)) {
            memcpy(curMat, m2, sizeof curMat);
            curX += dx;
            return;
        }
    }
}

/* move the current piece one column left (dx=-1) or right (dx=+1) */
static void movePiece(int dx)
{
    if (fits(curMat, curX + dx, curY))
        curX += dx;
}

/* ------------------------------------------------------------------ */

enum { K_NONE, K_DROP, K_CW, K_CCW, K_LEFT, K_RIGHT, K_QUIT };

static int readKey(void)
{
    unsigned char b;
    if (read(0, &b, 1) != 1) return K_NONE;

    if (b == '\r' || b == '\n') return K_DROP;      /* Enter: force down */
    if (b == 'q') return K_QUIT;

    if (b == 27) {                                  /* arrow key: ESC [ A/B/C/D */
        unsigned char seq, k;
        if (read(0, &seq, 1) != 1 || seq != '[') return K_NONE;
        if (read(0, &k, 1) != 1) return K_NONE;
        if (k == 'A') return K_CW;                   /* up:    CW  */
        if (k == 'B') return K_CCW;                  /* down:  CCW */
        if (k == 'C') return K_RIGHT;                /* right: move right */
        if (k == 'D') return K_LEFT;                 /* left:  move left  */
    }
    return K_NONE;
}

static void render(void)
{
    char line[64];
    char top[COLS + 3];

    top[0] = '+'; top[COLS + 1] = '+';
    memset(top + 1, '-', COLS);
    top[COLS + 2] = '\0';

    fputs("\033[2J\033[H", stdout);      /* clear + home */
    puts(top);

    for (int r = 0; r < ROWS; r++) {
        line[0] = '|';
        for (int c = 0; c < COLS; c++) {
            int i = r - curY, j = c - curX;
            char ch = ' ';
            if (board[r][c]) ch = board[r][c];
            if (i >= 0 && i < CELL && j >= 0 && j < CELL && curMat[i][j])
                ch = PCHAR[curIdx];
            line[1 + c] = ch;
        }
        line[COLS + 1] = '|';
        line[COLS + 2] = '\0';
        puts(line);
    }

    puts(top);
    printf("Score: %d\n", score);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */

static int waitForSpace(void)
{
    unsigned char b;
    puts("");
    puts("Press Space to start. Press q to quit.");
    puts("");
    while (read(0, &b, 1) == 1)
        if (b == ' ')  return 1;
        if (b == 'q')  return 0;
    return 0;
}

int main(void)
{
    srand((unsigned)time(NULL));

    /* terminal: raw mode, no echo, non-blocking reads */
    tcgetattr(0, &origTerm);
    struct termios raw = origTerm;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    atexit(restoreTerm);

    for (;;) {
        /* -------- game loop -------- */
        memset(board, 0, sizeof board);
        score = 0;
        spawn();

        long last = now_ms();
        for (;;) {
            int key = readKey();

            if (key == K_QUIT)
                break;
            if (key == K_DROP) {
                hardDrop();
                last = now_ms();
            } else if (key == K_CW)
                rotate(0);
            else if (key == K_CCW)
                rotate(1);
            else if (key == K_LEFT)
                movePiece(-1);
            else if (key == K_RIGHT)
                movePiece(1);

            render();

            if (now_ms() - last >= TICK_MS) {
                gravityTick();
                last = now_ms();
            }

            struct timespec ts = { 0, FRAME_MS * 1000000L };
            nanosleep(&ts, NULL);
        }
        /* -------- back to menu after a game over / quit -------- */

        if (!waitForSpace()) {
            restoreTerm();
            return 0;
        }
    }
}
